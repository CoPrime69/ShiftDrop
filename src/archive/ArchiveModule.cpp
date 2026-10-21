#include "archive/ArchiveModule.h"
#include "PathUtil.h"
#include "archive/ArchiveSource.h"
#include "archive/ArchiveWriter.h"
#include "archive/Sinks.h"
#include "Ask.h"
#include "Log.h"

#include <windows.h>

#include <filesystem>
#include <format>
#include <optional>

namespace sd::archive
{
    namespace
    {
        void RegisterActions(ActionRegistry& registry)
        {
            const ParamSpec format{ "format", ParamType::Enum, 0, 0, { "zip", "tar.gz", "7z" }, "zip" };
            registry.Add(ActionDescriptor{ "archive.compress", L"Archive", StepKind::Engine, { format } });
            registry.Add(ActionDescriptor{ "archive.extract", L"Extract", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "archive.convert", L"Convert archive", StepKind::Engine, { format } });
        }

        const char* BuiltInRecipes()
        {
            // Icons (Segoe Fluent Icons / MDL2): Package for archives made, OpenFolderHorizontal for Extract.
            return R"json([
  { "id": "archive-zip", "label": "Zip", "description": "One ZIP of everything", "ring": "files", "suffix": "zipped", "icon": "E7B8",
    "steps": [ { "action": "archive.compress", "params": { "format": "zip" } } ] },

  { "id": "archive-extract", "label": "Extract", "description": "Unpack into a folder next to it", "suffix": "extracted", "icon": "E8B7",
    "steps": [ { "action": "archive.extract" } ] },
  { "id": "archive-to-zip", "label": "ZIP", "description": "Repack as ZIP", "icon": "E7B8",
    "steps": [ { "action": "archive.convert", "params": { "format": "zip" } } ] },
  { "id": "archive-to-targz", "label": "TAR.GZ", "description": "Repack as TAR.GZ", "icon": "E7B8",
    "steps": [ { "action": "archive.convert", "params": { "format": "tar.gz" } } ] },
  { "id": "archive-to-7z", "label": "7z", "description": "Repack as 7z (LZMA2)", "icon": "E7B8",
    "steps": [ { "action": "archive.convert", "params": { "format": "7z" } } ] },

  { "id": "archive-folder-zip", "label": "ZIP", "description": "Pack into one ZIP", "suffix": "zipped", "icon": "E7B8",
    "steps": [ { "action": "archive.compress", "params": { "format": "zip" } } ] },
  { "id": "archive-folder-targz", "label": "TAR.GZ", "description": "Pack into one TAR.GZ", "suffix": "archived", "icon": "E7B8",
    "steps": [ { "action": "archive.compress", "params": { "format": "tar.gz" } } ] },
  { "id": "archive-folder-7z", "label": "7z", "description": "Pack into one 7z (LZMA2)", "suffix": "archived", "icon": "E7B8",
    "steps": [ { "action": "archive.compress", "params": { "format": "7z" } } ] }
])json";
        }

        const wchar_t* ModeText(ReadMode mode)
        {
            switch (mode)
            {
            case ReadMode::Sandboxed: return L"sandboxed worker";
            case ReadMode::Worker: return L"worker without AppContainer";
            case ReadMode::InProcess: return L"in process";
            }
            return L"";
        }

        // What the top panel says for a failure.
        std::wstring UserMessage(const Result& r)
        {
            switch (r.status)
            {
            case Status::Unsafe: return L"Blocked: " + r.message;
            case Status::Limit: return L"Stopped: " + r.message;
            case Status::Corrupt: return L"Can't read the archive: " + r.message;
            default: return r.message;
            }
        }

        std::wstring Plural(uint64_t n, const wchar_t* one, const wchar_t* many)
        {
            return std::format(L"{} {}", n, n == 1 ? one : many);
        }

        class ArchiveEngine final : public IEngine
        {
        public:
            explicit ArchiveEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action.starts_with("archive.");
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                const auto& action = recipe.steps[0].action;
                if (action == "archive.compress")
                    return recipe.ring == "files" || IsFolder(path);   // Convert-wheel packing is for folders
                return IsArchive(path) && !IsFolder(path);
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const DWORD a = GetFileAttributesW(path.c_str());
                if (a == INVALID_FILE_ATTRIBUTES)
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if (recipe.steps[0].action == "archive.compress")
                    return true;
                if ((a & FILE_ATTRIBUTE_DIRECTORY) || !IsArchive(path))
                {
                    if (why)
                        *why = L"not an archive";
                    return false;
                }
                if (recipe.steps[0].action == "archive.convert" && !FormatOf(recipe))
                {
                    if (why)
                        *why = L"unknown archive format";
                    return false;
                }
                return true;
            }

            bool IsNoOp(const Recipe& recipe, const std::wstring& path) const override
            {
                const auto* f = FormatOf(recipe);
                return recipe.steps[0].action == "archive.convert" && f && IsSameFormat(KindOfPath(path), f->format);
            }

            bool IsBatch(const Recipe& recipe) const override { return recipe.steps[0].action == "archive.compress"; }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                           const ProgressFn& progress) const override
            {
                return recipe.steps[0].action == "archive.extract" ? Extract(recipe, runId, source, stop, progress)
                                                                    : Convert(recipe, runId, source, stop, progress);
            }

            ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const override
            {
                const auto* format = FormatOf(recipe);
                if (sources.empty() || !format)
                    return ItemResult{ false, false, {}, L"nothing to pack", 0, {} };
                for (const auto& s : sources)
                    if (!PathExists(s))
                        return ItemResult{ false, false, {}, std::format(L"{} is gone", FileName(s)), 0, {} };

                // One item: named after it ("Photos (zipped).zip", "report (zipped).zip"). Several
                // (loose files, or items from more than one folder): "Archive.zip", in the first one's
                // folder, numbered when the name is taken ("Archive (2).zip").
                const std::filesystem::path first(sources.front());
                const bool single = sources.size() == 1;
                OutputPlace place;
                std::wstring historySource = sources.front();
                if (single)
                    place.stem = IsFolder(sources.front()) ? first.filename().wstring() : first.stem().wstring();
                else
                {
                    historySource = first.parent_path().wstring();
                    place.folder = historySource;
                    place.stem = kSeveralItems;
                }
                if (place.stem.empty())
                    place.stem = kSeveralItems;   // a drive's root has no name
                StagedOutput out(m_history, runId, historySource, false, place);
                if (!out.Ready())
                    return out.Fail(out.Error());
                const Result r = Compress(sources, format->format, out.Path(), stop, progress);
                if (r.status == Status::Cancelled || stop.stop_requested())
                    return out.Fail(L"cancelled", true);
                if (!r.ok())
                    return out.Fail(UserMessage(r));
                return out.Commit(single ? std::wstring_view(recipe.suffix) : std::wstring_view(), format->extension);
            }

        private:
            static constexpr const wchar_t* kSeveralItems = L"Archive";

            // The password box for `source`, when the worker finds it encrypted (src/Ask.h). The
            // password goes from the box to the worker and is wiped (Secret) as soon as it is sent.
            // `declined` is set when the user cancels it, and then it is never shown again for this job
            // (whatever the worker asks).
            static PasswordFn AskPassword(const std::wstring& source, const wchar_t* title, std::stop_token stop, bool& declined)
            {
                return [name = FileName(source), title, stop, &declined](bool wrong, Secret& password) {
                    if (!declined && ask::Password(ask::PasswordRequest{ title, name, wrong }, password, stop))
                        return true;
                    declined = true;
                    return false;
                };
            }

            static const WriteFormatInfo* FormatOf(const Recipe& recipe)
            {
                const auto& params = recipe.steps[0].params;
                return FindWriteFormat(params.is_object() ? params.value("format", std::string("zip")) : std::string("zip"));
            }

            ItemResult Finish(StagedOutput& out, const Result& r, std::stop_token stop, std::wstring_view suffix, std::wstring_view extension,
                              std::wstring message = {}, const wchar_t* declined = nullptr) const
            {
                if (r.status == Status::Cancelled || stop.stop_requested())
                    return out.Fail(L"cancelled", true);
                if (declined && r.status == Status::Password)
                    return out.Fail(declined, true);   // the user cancelled the password box
                if (!r.ok())
                    return out.Fail(UserMessage(r));
                auto result = out.Commit(suffix, extension);
                if (result.ok)
                    result.message = std::move(message);
                return result;
            }

            ItemResult Extract(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const
            {
                const Kind kind = KindOfPath(source);
                const Limits limits;
                ReadMode mode = ReadMode::InProcess;
                if (IsRaw(kind))
                {
                    // "notes.txt.gz" -> "notes (extracted).txt": the one file inside, named after the archive.
                    const std::filesystem::path inner(ArchiveStem(source));
                    StagedOutput out(m_history, runId, source, false, OutputPlace{ {}, inner.stem().wstring() });
                    if (!out.Ready())
                        return out.Fail(out.Error());
                    FileSink file(out.Path());
                    GuardSink guard(file, limits, FileSize(source), inner.wstring());
                    const Result r = ReadEntries(source, kind, limits, guard, stop, progress, &mode);   // one compressed file: never encrypted
                    file.End();   // closes the output if a failure left it open, so it can be deleted
                    log::Info(L"archive: extract {} ({}): {}", source, ModeText(mode), r.ok() ? L"ok" : r.message);
                    return Finish(out, r, stop, recipe.suffix, inner.extension().wstring());
                }

                StagedOutput out(m_history, runId, source, true, OutputPlace{ {}, ArchiveStem(source) });
                if (!out.Ready())
                    return out.Fail(out.Error());
                Result r;
                GuardSink* guardPtr = nullptr;
                ExtractSink sink(out.Path(), [&](uint64_t bytes) { return guardPtr ? guardPtr->Charge(bytes) : Result{}; });
                GuardSink guard(sink, limits, FileSize(source));
                guardPtr = &guard;
                bool declined = false;
                r = ReadEntries(source, kind, limits, guard, stop, progress, &mode, AskPassword(source, L"Extract", stop, declined));
                sink.End();   // closes a file left open by a failure mid-entry, so the staging folder can be removed
                log::Info(L"archive: extract {} ({}): {} files, {} folders, {} links skipped: {}", source, ModeText(mode), guard.files, guard.folders,
                          guard.skippedLinks, r.ok() ? L"ok" : r.message);
                if (r.ok() && guard.files + guard.folders == 0)
                    r = Fail(Status::Corrupt, L"it is empty");
                if (r.ok())
                {
                    sink.Finish();
                    sink.FlattenSingleFolder();
                }
                std::wstring message = Plural(guard.files, L"file", L"files");
                if (guard.skippedLinks)
                    message += std::format(L", {} skipped", Plural(guard.skippedLinks, L"link", L"links"));
                return Finish(out, r, stop, recipe.suffix, L"", L"Extracted " + message, declined ? L"no password given, so nothing was extracted" : nullptr);
            }

            ItemResult Convert(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const
            {
                const auto* format = FormatOf(recipe);
                const Kind kind = KindOfPath(source);
                const Limits limits;
                StagedOutput out(m_history, runId, source, false, OutputPlace{ {}, ArchiveStem(source) });
                if (!out.Ready())
                    return out.Fail(out.Error());
                ReadMode mode = ReadMode::InProcess;
                Result r;
                bool declined = false;
                {
                    // The writer must be closed before the staging file is renamed or deleted.
                    ArchiveWriter writer;
                    r = writer.Open(out.Path(), format->format);
                    if (r.ok())
                    {
                        WriteSink sink(writer, format->format, std::filesystem::path(out.Path()).parent_path().wstring());
                        GuardSink guard(sink, limits, FileSize(source), IsRaw(kind) ? ArchiveStem(source) : std::wstring());
                        r = ReadEntries(source, kind, limits, guard, stop, progress, &mode, AskPassword(source, L"Convert archive", stop, declined));
                    }
                    if (r.ok())
                        r = writer.Close();
                }
                log::Info(L"archive: convert {} to {} ({}): {}", source, format->label, ModeText(mode), r.ok() ? L"ok" : r.message);
                return Finish(out, r, stop, recipe.suffix, format->extension, {}, declined ? L"no password given, so nothing was converted" : nullptr);
            }

            HistoryStore* m_history;
        };
    }

    std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
    {
        return std::make_unique<ArchiveEngine>(services);
    }

    FeatureModule Module()
    {
        return FeatureModule{ "archive", &RegisterActions, &BuiltInRecipes, &MakeEngine };
    }
}
