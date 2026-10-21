#include "Log.h"
#include "Utf.h"
#include "pdf/PdfModule.h"
#include "share/Share.h"

namespace sd::share
{
    Hooks& Platform()
    {
        static Hooks hooks;   // WinShare.cpp fills it in from a static initialiser
        return hooks;
    }

    namespace
    {
        // Share (File actions wheel). The share sheet belongs to a window on the UI thread, which is exactly
        // what an engine "editor" gets, so it opens from OpenEditor and nothing runs on a worker.
        // Only when the sheet cannot open does the batch run, to put the reason in the top panel.
        class ShareEngine final : public IEngine
        {
        public:
            explicit ShareEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps.front().action == "share.files";
            }
            bool Applies(const Recipe&, const std::wstring&) const override { return true; }
            bool Accepts(const Recipe&, const std::wstring& path, std::wstring* why) const override
            {
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if (!Platform().show)
                {
                    if (why)
                        *why = L"sharing is not available in this build";
                    return false;
                }
                return true;
            }

            ItemResult Run(const Recipe&, int64_t, const std::wstring&, std::stop_token, const ProgressFn&) const override
            {
                return ItemResult{ false, false, {}, L"Share runs from the wheel", 0 };
            }

            bool IsBatch(const Recipe&) const override { return true; }
            ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token,
                                const ProgressFn&) const override
            {
                const auto& params = recipe.steps.front().params;
                const std::wstring error = params.contains("error") && params["error"].is_string()
                                               ? Widen(params["error"].get<std::string>())
                                               : std::wstring(L"the share sheet could not open");
                return pdf::FailItem(m_history, runId, sources.empty() ? std::wstring() : sources.front(), error);
            }

            bool NeedsEditor(const Recipe&) const override { return true; }
            void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                            std::function<void(Recipe, std::vector<std::wstring>)> done) const override
            {
                std::wstring error = L"sharing is not available in this build";
                if (Platform().show && Platform().show(owner, files, error))
                {
                    log::Info(L"share: sheet opened for {} item(s)", files.size());
                    return;
                }
                log::Warn(L"share: {}", error);
                Recipe failed = recipe;
                failed.steps.front().params["error"] = Narrow(error);
                done(std::move(failed), files);
            }

        private:
            HistoryStore* m_history;
        };
    }

    std::unique_ptr<IEngine> MakeShareEngine(const EngineServices& services) { return std::make_unique<ShareEngine>(services); }
}
