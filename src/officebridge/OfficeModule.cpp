#include "officebridge/OfficeModule.h"
#include "PathUtil.h"
#include "officebridge/OfficeBridge.h"

#include <format>

namespace sd::office
{
    namespace
    {
        class OfficeEngine final : public IEngine
        {
        public:
            OfficeEngine(const EngineServices& services, Apps apps) : m_history(services.history), m_apps(std::move(apps)) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action == "office.convert";
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                return Choose(m_apps, FamilyOf(path), recipe.FirstParam("format")) != Backend::None;
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                if (!Applies(recipe, path))
                {
                    if (why)
                        *why = FamilyOf(path) == Family::None ? L"not an office file" : L"needs PowerPoint";
                    return false;
                }
                if (!IsFile(path))
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                return true;
            }

            // COM objects of Office need a single-threaded apartment, and one app at a time. The lane is
            // their own: an export can take minutes, and Move to... must not wait behind it.
            JobLane Lane(const Recipe&) const override { return JobLane::Office; }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                const auto target = recipe.FirstParam("format");
                const auto backend = Choose(m_apps, FamilyOf(source), target);
                const bool folder = target != "pdf";
                StagedOutput out(m_history, runId, source, folder);
                if (!out.Ready())
                    return out.Fail(out.Error());
                if (backend == Backend::None)
                    return out.Fail(L"no office app can convert this file");

                Job job;
                job.source = source;
                job.target = target;
                job.output = out.Path();
                job.stop = stop;
                job.progress = progress;
                const auto outcome = backend == Backend::LibreOffice ? ConvertWithLibreOffice(m_apps.soffice, job) : ConvertWithOffice(backend, job);
                if (!outcome.ok)
                    return out.Fail(outcome.error, outcome.cancelled);
                return out.Commit(recipe.suffix, folder ? L"" : L".pdf");
            }

        private:
            HistoryStore* m_history;
            Apps m_apps;
        };

        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ "office.convert", L"Convert with the office app", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "pdf", "png", "jpg" }, "pdf" } } });
        }

        const char* Recipes()
        {
            // Icons (Segoe Fluent Icons): PDF EA90, Photo EB9F.
            return R"([
  { "id": "office-to-pdf", "label": "Exact PDF", "description": "PDF made by Office or LibreOffice, as it prints", "icon": "EA90",
    "steps": [ { "action": "office.convert", "params": { "format": "pdf" } } ] },
  { "id": "office-to-png", "label": "PNG slides", "description": "Every slide as a PNG picture, in a folder", "icon": "EB9F",
    "steps": [ { "action": "office.convert", "params": { "format": "png" } } ] },
  { "id": "office-to-jpg", "label": "JPG slides", "description": "Every slide as a JPG picture, in a folder", "icon": "EB9F",
    "steps": [ { "action": "office.convert", "params": { "format": "jpg" } } ] }
])";
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
        {
            return std::make_unique<OfficeEngine>(services, DetectApps());
        }
    }

    FeatureModule Module()
    {
        return FeatureModule{ "office", &RegisterActions, &Recipes, &MakeEngine };
    }
}
