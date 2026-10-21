#include "archive/ArchiveReader.h"

#include <archive.h>
#include <archive_entry.h>

#include <cwctype>
#include <format>
#include <memory>
#include <vector>

namespace sd::archive
{
    namespace
    {
        struct Client
        {
            HANDLE file;
            std::vector<char> buffer = std::vector<char>(1 << 20);
        };

        la_ssize_t ReadCallback(struct archive* a, void* data, const void** out)
        {
            auto* c = static_cast<Client*>(data);
            DWORD read = 0;
            if (!ReadFile(c->file, c->buffer.data(), static_cast<DWORD>(c->buffer.size()), &read, nullptr))
            {
                archive_set_error(a, static_cast<int>(GetLastError()), "cannot read the archive");
                return -1;
            }
            *out = c->buffer.data();
            return static_cast<la_ssize_t>(read);
        }

        la_int64_t SeekCallback(struct archive*, void* data, la_int64_t offset, int whence)
        {
            auto* c = static_cast<Client*>(data);
            LARGE_INTEGER to{}, now{};
            to.QuadPart = offset;
            const DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
            if (!SetFilePointerEx(c->file, to, &now, method))
                return ARCHIVE_FATAL;
            return now.QuadPart;
        }

        la_int64_t SkipCallback(struct archive* a, void* data, la_int64_t request)
        {
            return SeekCallback(a, data, request, SEEK_CUR) < 0 ? 0 : request;
        }

        bool Mentions(const char* text, std::initializer_list<const char*> words)
        {
            if (!text)
                return false;
            std::string lower(text);
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const char* w : words)
                if (lower.find(w) != std::string::npos)
                    return true;
            return false;
        }

        // libarchive reports encryption in several ways depending on the format: a flag on the entry,
        // a flag on the archive, or only an error message ("Passphrase required", "encrypted").
        bool IsPasswordError(struct archive* a)
        {
            return archive_read_has_encrypted_entries(a) > 0 ||
                   Mentions(archive_error_string(a), { "passphrase", "encrypt", "password", "decrypt" });
        }

        // Whether libarchive, as built here, can decrypt WinZip AES: its ZIP writer accepts AES only
        // with a crypto library behind it, and the reader then has one too.
        bool CanDecryptAes()
        {
            static const bool can = [] {
                std::unique_ptr<struct archive, decltype(&archive_write_free)> w(archive_write_new(), &archive_write_free);
                return w && archive_write_set_format_zip(w.get()) == ARCHIVE_OK &&
                       archive_write_set_format_option(w.get(), "zip", "encryption", "aes256") == ARCHIVE_OK;
            }();
            return can;
        }

        // Why an encrypted archive can't be opened even with its password, or empty when it can:
        // libarchive decrypts ZIP's own ZipCrypto, and WinZip AES when it has a crypto library, but
        // not 7z or RAR encryption.
        std::wstring CantDecrypt(struct archive* a)
        {
            switch (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK)
            {
            case ARCHIVE_FORMAT_ZIP:
                if (Mentions(archive_format_name(a), { "(aes)" }) && !CanDecryptAes())
                    return L"this ZIP is protected with AES encryption, which can't be opened yet";
                if (Mentions(archive_error_string(a), { "encrypted file is unsupported", "encryption format", "encryption algorithm", "encryption flag",
                                                        "lack of crypto" }))
                    return L"this ZIP uses an encryption that can't be opened";
                return {};
            case ARCHIVE_FORMAT_7ZIP: return L"password-protected 7z archives can't be opened yet";
            case ARCHIVE_FORMAT_RAR:
            case ARCHIVE_FORMAT_RAR_V5: return L"password-protected RAR archives can't be opened yet";
            default: return L"this archive is password-protected; protected archives can't be opened yet";
            }
        }

        // libarchive's passphrase callback: each password the user types, in each encoding it may
        // have been stored in, then the user again (told the last one was wrong).
        class Passwords
        {
        public:
            explicit Passwords(const PasswordFn& ask) : m_ask(ask) {}

            static const char* Callback(struct archive*, void* self) { return static_cast<Passwords*>(self)->Next(); }

            int asked = 0;          // passwords the user gave
            bool refused = false;   // the user cancelled

        private:
            const char* Next()
            {
                if (m_next < m_forms.size())
                    return m_forms[m_next++].c_str();
                m_forms.clear();   // wipes every form of the one that failed (libarchive keeps its own copies until it is freed, and wipes them then)
                m_next = 0;
                if (refused || !m_ask)
                    return nullptr;
                Secret typed;
                if (!m_ask(asked > 0, typed) || typed.empty())
                {
                    refused = true;
                    return nullptr;
                }
                ++asked;
                Encode(typed.View());
                return m_forms.empty() ? Next() : m_forms[m_next++].c_str();
            }

            // ZIP stores no charset for its password: WinZip AES and 7-Zip use UTF-8, older tools the
            // ANSI or OEM code page. Each distinct form is tried; for a plain ASCII password that is one.
            void Encode(std::wstring_view password)
            {
                for (const UINT codePage : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP), static_cast<UINT>(CP_OEMCP) })
                {
                    SecretBytes form;
                    BOOL lossy = FALSE;
                    const int n = WideCharToMultiByte(codePage, codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS, password.data(),
                                                      static_cast<int>(password.size()), form.Buffer(), static_cast<int>(SecretBytes::kCapacity),
                                                      nullptr, codePage == CP_UTF8 ? nullptr : &lossy);
                    if (n <= 0 || lossy)
                        continue;
                    form.SetSize(static_cast<size_t>(n));
                    bool seen = false;
                    for (const auto& f : m_forms)
                        seen = seen || f.View() == form.View();
                    if (!seen)
                        m_forms.push_back(std::move(form));
                }
            }

            const PasswordFn& m_ask;
            std::vector<SecretBytes> m_forms;
            size_t m_next = 0;
        };

        Result PasswordRefusal(struct archive* a)
        {
            const auto why = CantDecrypt(a);
            return Fail(Status::Password, why.empty() ? L"this archive is password-protected; protected archives can't be opened yet" : why);
        }

        Result ArchiveError(struct archive* a, const wchar_t* what, const Passwords* passwords = nullptr)
        {
            if (passwords && passwords->refused)
                return Fail(Status::Password, L"no password was given");
            if (passwords && passwords->asked > 0)
            {
                if (const auto why = CantDecrypt(a); !why.empty())
                    return Fail(Status::Password, why);
                // A wrong ZipCrypto password passes its one-byte check once in 256 tries; the data
                // then fails to inflate or its CRC is wrong.
                return Fail(Status::Corrupt, L"the password is wrong, or the archive is damaged");
            }
            if (IsPasswordError(a))
                return PasswordRefusal(a);
            const char* text = archive_error_string(a);
            std::wstring detail;
            if (text)
                for (const char* p = text; *p; ++p)
                    detail += static_cast<wchar_t>(static_cast<unsigned char>(*p));   // libarchive's messages are ASCII
            return Fail(Status::Corrupt, detail.empty() ? std::wstring(what) : std::format(L"{}: {}", what, detail));
        }

        std::wstring NameOf(struct archive_entry* e, const wchar_t* wide, const char* narrow)
        {
            (void)e;
            if (wide && *wide)
                return wide;
            // libarchive could not convert the stored bytes (no charset flag, odd code page): decode
            // them ourselves, UTF-8 first, then the OEM code page.
            return narrow ? DecodeName(narrow, GetOEMCP()) : std::wstring();
        }

        int64_t Ticks(bool set, la_int64_t seconds, long nanoseconds)
        {
            return set ? TicksFromUnix(seconds, nanoseconds) : 0;
        }
    }

    Result ReadArchive(HANDLE input, const ReadOptions& options, IEntrySink& sink, const std::function<bool(uint64_t)>& tick,
                       const PasswordFn& password)
    {
        std::unique_ptr<struct archive, decltype(&archive_read_free)> owner(archive_read_new(), &archive_read_free);
        struct archive* a = owner.get();
        if (!a)
            return Fail(Status::Io, L"out of memory");
        // Only the formats we promise, so a crafted file can't reach rarely used parsers. Filters are
        // the compression wrappers around a tar (and the single-file .gz/.bz2/.xz). External-program
        // filters (lrzip, lzop...) are never enabled; the worker could not start them anyway.
        archive_read_support_format_zip(a);
        archive_read_support_format_tar(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);
        if (IsRaw(options.kind))
            archive_read_support_format_raw(a);
        archive_read_support_filter_gzip(a);
        archive_read_support_filter_bzip2(a);
        archive_read_support_filter_xz(a);
        archive_read_support_filter_lzma(a);
        archive_read_support_filter_zstd(a);
        // ZIP names without the UTF-8 flag were written in the OEM code page (CP437 in the US, CP850 in
        // Western Europe...). Entries that have the flag are read as UTF-8 whatever this says.
        const auto charset = std::format("CP{}", GetOEMCP());
        archive_read_set_format_option(a, "zip", "hdrcharset", charset.c_str());

        // Before the archive is opened: libarchive asks for a password only when an entry needs one.
        Passwords passwords(password);
        if (password)
            archive_read_set_passphrase_callback(a, &passwords, &Passwords::Callback);

        Client client{ input };
        archive_read_set_seek_callback(a, &SeekCallback);
        if (archive_read_open2(a, &client, nullptr, &ReadCallback, &SkipCallback, nullptr) != ARCHIVE_OK)
            return ArchiveError(a, L"not a readable archive");

        uint64_t entries = 0, total = 0;
        std::vector<char> buffer(256 * 1024);
        const auto overLimit = [&]() -> Result {
            if (total > options.limits.maxTotalBytes)
                return Fail(Status::Limit, std::format(L"it unpacks to more than {} GB", options.limits.maxTotalBytes >> 30));
            if (total > options.limits.ratioAfterBytes && options.inputSize > 0 &&
                static_cast<double>(total) > options.limits.maxRatio * static_cast<double>(options.inputSize))
                return Fail(Status::Limit, L"it unpacks to far more than its own size (a zip bomb?)");
            return {};
        };

        for (;;)
        {
            struct archive_entry* e = nullptr;
            const int r = archive_read_next_header(a, &e);
            if (r == ARCHIVE_EOF)
                break;
            if (r < ARCHIVE_WARN)   // ARCHIVE_WARN: e.g. a name in an unknown charset; the entry is still usable
                return ArchiveError(a, L"damaged archive", &passwords);
            // An encrypted entry: libarchive calls Passwords when it reads the data, if it can
            // decrypt this kind at all; otherwise stop before asking for a password that can't help.
            if (archive_entry_is_encrypted(e) && (!password || !CantDecrypt(a).empty()))
                return PasswordRefusal(a);
            if (++entries > options.limits.maxEntries)
                return Fail(Status::Limit, std::format(L"it has more than {} entries", options.limits.maxEntries));

            Entry entry;
            entry.name = NameOf(e, archive_entry_pathname_w(e), archive_entry_pathname(e));
            const char* hardlink = archive_entry_hardlink(e);
            switch (archive_entry_filetype(e))
            {
            case AE_IFREG: entry.type = hardlink ? EntryType::Hardlink : EntryType::File; break;
            case AE_IFDIR: entry.type = EntryType::Directory; break;
            case AE_IFLNK: entry.type = EntryType::Symlink; break;
            default: entry.type = hardlink ? EntryType::Hardlink : EntryType::Other; break;
            }
            if (entry.type == EntryType::Hardlink)
                entry.link = NameOf(e, archive_entry_hardlink_w(e), hardlink);
            else if (entry.type == EntryType::Symlink)
                entry.link = NameOf(e, archive_entry_symlink_w(e), archive_entry_symlink(e));
            entry.size = archive_entry_size_is_set(e) ? archive_entry_size(e) : -1;
            entry.mtime = Ticks(archive_entry_mtime_is_set(e), archive_entry_mtime(e), archive_entry_mtime_nsec(e));
            entry.atime = Ticks(archive_entry_atime_is_set(e), archive_entry_atime(e), archive_entry_atime_nsec(e));
            if (entry.type != EntryType::File)
                entry.size = entry.type == EntryType::Directory ? 0 : -1;

            if (auto res = sink.Begin(entry); !res.ok())
                return res;
            if (entry.type == EntryType::File)
            {
                for (;;)
                {
                    const la_ssize_t n = archive_read_data(a, buffer.data(), buffer.size());
                    if (n == 0)
                        break;
                    if (n < 0)
                        return ArchiveError(a, L"damaged entry", &passwords);
                    total += static_cast<uint64_t>(n);
                    if (auto res = overLimit(); !res.ok())
                        return res;
                    if (auto res = sink.Data(buffer.data(), static_cast<size_t>(n)); !res.ok())
                        return res;
                    if (tick && !tick(static_cast<uint64_t>(archive_filter_bytes(a, -1))))
                        return Fail(Status::Cancelled, L"cancelled");
                }
            }
            if (auto res = sink.End(); !res.ok())
                return res;
            if (tick && !tick(static_cast<uint64_t>(archive_filter_bytes(a, -1))))
                return Fail(Status::Cancelled, L"cancelled");
        }
        return {};
    }
}
