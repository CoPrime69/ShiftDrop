#pragma once
#include "Ffmpeg.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// How the video and audio engines open the timeline editor. The engines live in shiftdrop_core
// (tested without a window); the editor is UI and lives in the app, which links it in and registers
// it here at startup. Without it (the tests) a tool that needs the editor says so and does nothing.
namespace sd::mediatools
{
    enum class EditorTool
    {
        TrimVideo,
        CropVideo,
        SpeedVideo,
        JoinVideos,
        SaveFrames,
        SplitVideo,
        RedactVideo,
        TrimAudio,
        AudioChannels,
        BleepAudio,
        AudioVisualizer,
    };
    bool IsVideoTool(EditorTool tool);
    // Tools whose choices belong to one file (a trim range, redaction boxes): the editor walks
    // through the files one at a time. The others take one set of choices for every file.
    bool IsPerFileTool(EditorTool tool);
    // The editor's tool bar: every video tool, or every audio tool that has a window, in bar order.
    // Any slice opens the window; its tabs switch tools without reloading the files.
    std::vector<EditorTool> SiblingTools(EditorTool tool);
    const wchar_t* ToolName(EditorTool tool);    // "Trim": its tool bar label
    const wchar_t* ToolTitle(EditorTool tool);   // "Trim Video": the window title
    const char* ToolAction(EditorTool tool);     // "video.trim": the action that runs it
    bool ToolForAction(const std::string& action, EditorTool& tool);

    struct EditorRequest
    {
        EditorTool tool = EditorTool::TrimVideo;
        std::wstring title;                          // "Trim Video"
        std::vector<std::wstring> files;
        nlohmann::json params;                       // the recipe's parameters, defaults filled in
        std::shared_ptr<const FfmpegEngine> ffmpeg;  // for thumbnails, frames, the waveform and playback
    };

    // Called once on the UI thread with the user's choices and the files to run (skipped files left
    // out); never called when the editor is closed. Per-file tools put each file's choices under
    // params["perFile"][path as UTF-8], so the whole drop is still one batch. params["tool"] is the
    // ToolAction of the tool chosen last, which can differ from the one the window opened with.
    using EditorDone = std::function<void(nlohmann::json params, std::vector<std::wstring> files)>;
    // One file's params: the shared ones with that file's "perFile" entry merged over them.
    nlohmann::json ParamsForFile(const nlohmann::json& params, const std::wstring& path);
    using EditorOpener = void (*)(HWND owner, EditorRequest request, EditorDone done);

    void SetEditorOpener(EditorOpener opener);
    // False when no editor is linked in (tests).
    bool OpenEditor(HWND owner, EditorRequest request, EditorDone done);
}
