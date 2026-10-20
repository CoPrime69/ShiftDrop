#include "metadata/MediaMetadata.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace sd::metadata
{
    namespace
    {
        std::string Lower(std::string s)
        {
            for (auto& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        size_t Indent(std::string_view line)
        {
            size_t n = 0;
            while (n < line.size() && line[n] == ' ')
                ++n;
            return n;
        }

        bool Has(const std::string& key, std::initializer_list<std::string_view> words)
        {
            return std::any_of(words.begin(), words.end(), [&](std::string_view w) { return key.find(w) != std::string::npos; });
        }
    }

    bool IsPlumbingTag(const std::string& key)
    {
        static const std::string_view kPlumbing[] = { "major_brand", "minor_version", "compatible_brands", "encoder", "handler_name",
                                                      "vendor_id", "language", "duration", "bps", "number_of_frames", "number_of_bytes" };
        const std::string k = Lower(key);
        for (const auto p : kPlumbing)
            if (k == p)
                return true;
        return k.starts_with("_statistics");
    }

    std::vector<MediaTag> MediaTags::Personal() const
    {
        std::vector<MediaTag> out;
        for (const auto& t : tags)
            if (!IsPlumbingTag(t.key))
                out.push_back(t);
        return out;
    }

    size_t MediaTags::PersonalCount() const
    {
        return Personal().size() + chapters + dataStreams + coverArt + attachments;
    }

    std::vector<std::wstring> MediaTags::Kinds() const
    {
        bool gps = false, camera = false, author = false, dates = false, captions = false;
        for (const auto& t : Personal())
        {
            const std::string k = Lower(t.key);
            gps |= Has(k, { "location", "gps", "iso6709" });
            camera |= Has(k, { "make", "model", "software", "camera", "lens" });
            author |= Has(k, { "artist", "author", "composer", "copyright", "performer", "publisher" });
            dates |= Has(k, { "creation_time", "date", "year" });
            captions |= Has(k, { "title", "comment", "description", "synopsis", "lyrics", "album", "genre", "keywords" });
        }
        std::vector<std::wstring> out;
        if (gps) out.emplace_back(L"GPS");
        if (camera || dataStreams) out.emplace_back(L"camera");
        if (author) out.emplace_back(L"author");
        if (dates) out.emplace_back(L"dates");
        if (captions) out.emplace_back(L"titles");
        if (chapters) out.emplace_back(L"chapters");
        if (coverArt) out.emplace_back(L"cover art");
        if (attachments) out.emplace_back(L"attachments");
        return out;
    }

    MediaTags ParseMediaTags(const std::vector<std::string>& lines)
    {
        // The probe log:
        //   Input #0, mov,mp4,..., from 'x.mp4':
        //     Metadata:
        //       creation_time   : 2026-10-20T10:00:00.000000Z
        //     Duration: ...
        //     Chapters:
        //       Chapter #0:0: start 0.000000, end 1.000000
        //         Metadata:
        //           title           : One
        //     Stream #0:0[0x1](und): Video: h264 ...
        //         Metadata:
        //           handler_name    : VideoHandler
        MediaTags out;
        MediaTag::Scope scope = MediaTag::Scope::Global;
        bool inMetadata = false;
        size_t metadataIndent = 0;
        bool started = false;
        for (const auto& raw : lines)
        {
            const std::string_view line(raw);
            const size_t indent = Indent(line);
            const std::string_view text = line.substr(indent);
            if (text.starts_with("Input #"))
            {
                started = true;
                scope = MediaTag::Scope::Global;
                inMetadata = false;
                continue;
            }
            if (!started || text.starts_with("Output #"))
            {
                if (text.starts_with("Output #"))
                    break;
                continue;
            }
            if (inMetadata && indent > metadataIndent)
            {
                const auto colon = text.find(" : ");
                const auto bare = text.find(':');
                const auto at = colon != std::string_view::npos ? colon : bare;
                if (at != std::string_view::npos)
                {
                    std::string key(text.substr(0, at));
                    while (!key.empty() && key.back() == ' ')
                        key.pop_back();
                    std::string value(text.substr(std::min(text.size(), at + (colon != std::string_view::npos ? 3 : 1))));
                    if (!key.empty())
                        out.tags.push_back(MediaTag{ scope, std::move(key), std::move(value) });
                }
                continue;
            }
            inMetadata = false;
            if (text == "Metadata:")
            {
                inMetadata = true;
                metadataIndent = indent;
                continue;
            }
            if (text.starts_with("Chapter #"))
            {
                ++out.chapters;
                scope = MediaTag::Scope::Chapter;
                continue;
            }
            if (text.starts_with("Stream #"))
            {
                scope = MediaTag::Scope::Stream;
                if (text.find(": Data:") != std::string_view::npos)
                    ++out.dataStreams;
                else if (text.find(": Attachment:") != std::string_view::npos)
                    ++out.attachments;
                else if (text.find("(attached pic)") != std::string_view::npos)
                    ++out.coverArt;
                continue;
            }
            if (text.starts_with("Duration:") || text.starts_with("Chapters:"))
                scope = MediaTag::Scope::Global;
        }
        return out;
    }

    std::wstring MuxerFor(const std::wstring& ext)
    {
        static const std::pair<const wchar_t*, const wchar_t*> kMuxers[] = {
            { L".mp4", L"mp4" },   { L".m4v", L"mp4" },   { L".mov", L"mov" },   { L".m4a", L"ipod" },  { L".3gp", L"3gp" },
            { L".mkv", L"matroska" }, { L".mka", L"matroska" }, { L".webm", L"webm" }, { L".avi", L"avi" },   { L".mp3", L"mp3" },
            { L".flac", L"flac" }, { L".ogg", L"ogg" },   { L".oga", L"ogg" },   { L".opus", L"opus" }, { L".wav", L"wav" },
            { L".aif", L"aiff" },  { L".aiff", L"aiff" }, { L".wmv", L"asf" },   { L".wma", L"asf" },   { L".asf", L"asf" },
            { L".ts", L"mpegts" }, { L".mts", L"mpegts" }, { L".m2ts", L"mpegts" }, { L".mpg", L"mpeg" }, { L".mpeg", L"mpeg" },
        };
        for (const auto& [e, m] : kMuxers)
            if (ext == e)
                return m;
        return {};
    }

    std::vector<std::wstring> StripMediaArgs(const std::wstring& ext)
    {
        // 0:V is video that is not an attached picture, so cover art is left out; data tracks
        // (0:d) and attachments (0:t) are never mapped. "?" lets a song have no video, and so on.
        std::vector<std::wstring> a{ L"-map", L"0:V?", L"-map", L"0:a?", L"-map", L"0:s?", L"-c", L"copy",
                                     // -1 disables every automatic metadata copy: global, per stream and per chapter.
                                     L"-map_metadata", L"-1", L"-map_chapters", L"-1",
                                     // No "encoder: Lavf..." tag or other build details written by the muxer.
                                     L"-fflags", L"+bitexact" };
        if (ext == L".mp3")
            a.insert(a.end(), { L"-write_xing", L"0" });
        a.insert(a.end(), { L"-f", MuxerFor(ext) });
        return a;
    }
}
