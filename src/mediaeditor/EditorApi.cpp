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

    std::vector<EditorTool> SiblingTools(EditorTool tool)
    {
        if (IsVideoTool(tool))
            return { EditorTool::TrimVideo, EditorTool::CropVideo, EditorTool::SpeedVideo, EditorTool::JoinVideos,
                     EditorTool::SaveFrames, EditorTool::SplitVideo, EditorTool::RedactVideo };
        return { EditorTool::TrimAudio, EditorTool::AudioChannels, EditorTool::BleepAudio, EditorTool::AudioVisualizer };
    }

    namespace
    {
        struct ToolNames
        {
            EditorTool tool;
            const wchar_t* name;
            const wchar_t* title;
            const char* action;
            wchar_t icon;   // Segoe Fluent Icons: the wheel's (the recipe's "icon")
        };
        constexpr ToolNames kTools[] = {
            { EditorTool::TrimVideo, L"Trim", L"Trim Video", "video.trim", 0xE78A },
            { EditorTool::CropVideo, L"Crop", L"Crop Video", "video.crop", 0xE7A8 },
            { EditorTool::SpeedVideo, L"Speed", L"Change Video Speed", "video.speed", 0xEC49 },
            { EditorTool::JoinVideos, L"Join", L"Join Videos", "video.join", 0xE71B },
            { EditorTool::SaveFrames, L"Frames", L"Save Video Frames", "video.frames", 0xE722 },
            { EditorTool::SplitVideo, L"Split", L"Split Video", "video.split", 0xE8C6 },
            { EditorTool::RedactVideo, L"Redact", L"Redact Video", "video.redact", 0xE72E },
            { EditorTool::TrimAudio, L"Trim", L"Trim Audio", "audio.trim", 0xE78A },
            { EditorTool::AudioChannels, L"Channels", L"Convert Audio Channels", "audio.channels", 0xE7F5 },
            { EditorTool::BleepAudio, L"Bleep", L"Bleep Audio", "audio.bleep", 0xE74F },
            { EditorTool::AudioVisualizer, L"Visualizer", L"Audio Visualizer", "audio.visualizer", 0xE9E9 },
        };

        const ToolNames& NamesOf(EditorTool tool)
        {
            for (const auto& t : kTools)
                if (t.tool == tool)
                    return t;
            return kTools[0];
        }
    }

    const wchar_t* ToolName(EditorTool tool) { return NamesOf(tool).name; }
    const wchar_t* ToolTitle(EditorTool tool) { return NamesOf(tool).title; }
    const char* ToolAction(EditorTool tool) { return NamesOf(tool).action; }
    wchar_t ToolIcon(EditorTool tool) { return NamesOf(tool).icon; }

    bool ToolForAction(const std::string& action, EditorTool& tool)
    {
        for (const auto& t : kTools)
            if (action == t.action)
            {
                tool = t.tool;
                return true;
            }
        return false;
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
