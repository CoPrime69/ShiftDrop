#include "mediaeditor/EditorApi.h"
#include "Log.h"
#include "Utf.h"

#include <atomic>

namespace sd::mediatools
{
    namespace
    {
        std::atomic<EditorOpener>& Opener()
        {
            static std::atomic<EditorOpener> opener{ nullptr };
            return opener;
        }
    }

    bool IsVideoTool(EditorTool tool)
    {
        switch (tool)
        {
        case EditorTool::TrimAudio:
        case EditorTool::AudioChannels:
        case EditorTool::BleepAudio:
        case EditorTool::AudioVisualizer:
            return false;
        default:
            return true;
        }
    }

    bool IsPerFileTool(EditorTool tool)
    {
        switch (tool)
        {
        case EditorTool::SpeedVideo:
        case EditorTool::JoinVideos:
        case EditorTool::AudioChannels:
        case EditorTool::AudioVisualizer:
            return false;
        default:
            return true;
        }
    }

    nlohmann::json ParamsForFile(const nlohmann::json& params, const std::wstring& path)
    {
        if (!params.is_object() || !params.contains("perFile"))
            return params;
        nlohmann::json out = params;
        out.erase("perFile");
        const auto& perFile = params["perFile"];
        if (const auto it = perFile.is_object() ? perFile.find(Narrow(path)) : perFile.end(); it != perFile.end() && it->is_object())
            out.update(*it);
        return out;
    }

    void SetEditorOpener(EditorOpener opener) { Opener() = opener; }

    bool OpenEditor(HWND owner, EditorRequest request, EditorDone done)
    {
        const EditorOpener opener = Opener();
        if (!opener)
        {
            log::Warn(L"media tools: no editor window in this build; '{}' needs one", request.title);
            return false;
        }
        opener(owner, std::move(request), std::move(done));
        return true;
    }
}
