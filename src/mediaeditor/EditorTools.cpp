// The per-tool half of the editor window: default choices, the options panel, what the timeline and
// the picture do under the mouse and keys, how a choice sounds while playing, and the parameters
// handed back to the engine.
#include "mediaeditor/EditorWindow.h"
#include "FormatText.h"
#include "Log.h"
#include "Media.h"
#include "PathUtil.h"
#include "Utf.h"
#include "Wic.h"
#include "audiotools/AudioTools.h"
#include "videotools/VideoTools.h"

#include <shobjidl.h>
#include <wincodec.h>
#include <wil/resource.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

using Microsoft::WRL::ComPtr;

namespace sd::mediatools
{
    using theme::WithAlpha;

    namespace
    {
        // Tool control ids (shared ones are below 100).
        enum : int
        {
            kIdStart = 100, kIdEnd, kIdStartHere, kIdEndHere, kIdPlaySel, kIdMode = 110, kIdSilence = 120, kIdAspect = 130, kIdOutW = 140, kIdOutH,
            kIdOutReset, kIdSpeed = 150, kIdSpeedCustom = 170, kIdUp = 180, kIdDown, kIdRemove, kIdAdd = 190, kIdSplitKind = 200, kIdParts = 210,
            kIdStyle = 220, kIdWhole = 230, kIdTone = 240, kIdListen = 250, kIdMono = 260, kIdSwap = 270, kIdLeft, kIdRight, kIdResetVol,
            kIdPicStyle = 280, kIdChoose = 290, kIdClear, kIdShape = 300, kIdJoinFit = 310, kIdTrimPart = 320, kIdFade = 330, kIdPitch,
            kIdFramePick = 340, kIdEvery = 350, kIdRate, kIdFormat = 360, kIdSplitEvery = 370, kIdAddClips = 380, kIdDuck = 385,
            kIdRows = 400, kIdRowDelete = 600,
        };

        constexpr float kGrip = 7.f;        // how near an edge or marker the mouse must be (DIPs)
        constexpr float kJoinRowH = 76.f;
        const std::vector<double> kSpeeds{ 0.25, 0.5, 0.75, 1.25, 1.5, 2, 3, 4 };
        const char* const kAspects[] = { "free", "original", "16:9", "9:16", "1:1", "4:5" };
        const wchar_t* const kAspectLabels[] = { L"Free", L"Original", L"16:9", L"9:16", L"1:1", L"4:5" };
        const char* const kShapes[] = { "landscape", "portrait", "square" };
        const char* const kFramePicks[] = { "marks", "every", "rate" };   // FramePick's order
        const char* const kFrameFormats[] = { "png", "jpg", "webp" };
        const char* const kBleepSounds[] = { "tone", "silence", "duck" };  // BleepSound's order

        nlohmann::json SecondsList(const std::vector<int64_t>& times)
        {
            nlohmann::json a = nlohmann::json::array();
            for (const auto t : times)
                a.push_back(ToSeconds(t));
            return a;
        }

        LPCWSTR HandleCursor(int handle)
        {
            switch (handle)
            {
            case 0:
            case 2: return IDC_SIZENWSE;
            case 1:
            case 3: return IDC_SIZENESW;
            case 4:
            case 6: return IDC_SIZENS;
            case 5:
            case 7: return IDC_SIZEWE;
            default: return IDC_SIZEALL;
            }
        }

        // "1:02.500" rather than "00:01:02.500" where the hours are zero (list rows are narrow).
        std::wstring Short(int64_t us)
        {
            std::wstring t = FormatTimecode(us);
            if (t.starts_with(L"00:"))
                t.erase(0, t[3] == L'0' ? 4 : 3);
            return t;
        }

        std::wstring SpanText(const TimeRange& r) { return Short(r.start) + L" \u2013 " + Short(r.end); }

        // "1.5", "1.5x", "150%" -> 1.5
        bool ParseSpeed(std::wstring text, double& out)
        {
            std::erase_if(text, [](wchar_t c) { return c == L' ' || c == L'x' || c == L'X' || c == L'\u00D7'; });
            std::replace(text.begin(), text.end(), L',', L'.');
            bool percent = false;
            if (!text.empty() && text.back() == L'%')
            {
                percent = true;
                text.pop_back();
            }
            if (text.empty())
                return false;
            wchar_t* end = nullptr;
            const double v = std::wcstod(text.c_str(), &end);
            if (!end || *end != 0 || !std::isfinite(v))
                return false;
            out = percent ? v / 100 : v;
            return true;
        }

        bool ParseInt(const std::wstring& text, int lo, int hi, int& out)
        {
            if (text.empty() || text.size() > 6 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                return false;
            const int v = std::stoi(text);
            if (v < lo || v > hi)
                return false;
            out = v;
            return true;
        }

        // A number typed with or without its unit: "30", "30 frames", "2.5 a second".
        bool ParseNumber(std::wstring text, double& out)
        {
            while (!text.empty() && (std::iswalpha(text.back()) || text.back() == L' '))
                text.pop_back();
            std::replace(text.begin(), text.end(), L',', L'.');
            while (!text.empty() && text.front() == L' ')
                text.erase(0, 1);
            if (text.empty())
                return false;
            wchar_t* end = nullptr;
            const double v = std::wcstod(text.c_str(), &end);
            if (!end || *end != 0 || !std::isfinite(v))
                return false;
            out = v;
            return true;
        }

        // Files from the open dialog: one, or several when `many`. Empty when cancelled.
        std::vector<std::wstring> PickFiles(HWND owner, const wchar_t* title, const wchar_t* kind, const std::wstring& patterns, bool many)
        {
            std::vector<std::wstring> paths;
            ComPtr<IFileOpenDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                return paths;
            const COMDLG_FILTERSPEC types[] = { { kind, patterns.c_str() } };
            dialog->SetFileTypes(1, types);
            dialog->SetTitle(title);
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (many ? FOS_ALLOWMULTISELECT : 0));
            if (FAILED(dialog->Show(owner)))
                return paths;
            const auto add = [&](IShellItem* item) {
                wil::unique_cotaskmem_string path;
                if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    paths.emplace_back(path.get());
            };
            if (!many)
            {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(dialog->GetResult(&item)))
                    add(item.Get());
                return paths;
            }
            ComPtr<IShellItemArray> items;
            DWORD count = 0;
            if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count)))
                return paths;
            for (DWORD i = 0; i < count; ++i)
            {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(items->GetItemAt(i, &item)))
                    add(item.Get());
            }
            return paths;
        }

        // A picture for the visualizer's preview, flattened onto black, at most `maxSide` across.
        bool LoadPicture(const std::wstring& path, int maxSide, Frame& out)
        {
            const auto wic = wic::Factory();
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (!wic ||
                FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)))
                return false;
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            if (w == 0 || h == 0)
                return false;
            const double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(w, h));
            const UINT sw = std::max(1u, static_cast<UINT>(w * scale)), sh = std::max(1u, static_cast<UINT>(h * scale));
            ComPtr<IWICBitmapScaler> scaler;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant)) ||
                FAILED(wic->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                return false;
            out.w = static_cast<int>(sw);
            out.h = static_cast<int>(sh);
            out.bgra.resize(static_cast<size_t>(sw) * sh);
            if (FAILED(converter->CopyPixels(nullptr, sw * 4, static_cast<UINT>(out.bgra.size() * 4), reinterpret_cast<BYTE*>(out.bgra.data()))))
                return false;
            for (auto& p : out.bgra)
                p |= 0xFF000000u;   // premultiplied over black = the colour channels as they are
            return true;
        }
    }

    // ---- Defaults ------------------------------------------------------------------------------------------

    void EditorWindow::InitChoices(FileState& f)
    {
        const auto& p = m_req.params;
        const int64_t dur = f.info.DurationUs();
        f.keep = { 0, dur };
        const double start = p.value("start", 0.0), end = p.value("end", 0.0);
        if (start > 0)
            f.keep.start = std::clamp<int64_t>(FromSeconds(start), 0, dur);
        if (end > 0)
            f.keep.end = std::clamp<int64_t>(FromSeconds(end), f.keep.start, dur);
        if (f.keep.Length() < kMinRangeUs)
            f.keep = { 0, dur };
        if (const auto it = p.find("box"); it != p.end() && it->is_array() && it->size() == 4 && (*it)[0].is_number())
            f.crop = ClampCrop({ (*it)[0].get<int>(), (*it)[1].get<int>(), (*it)[2].get<int>(), (*it)[3].get<int>() }, f.info.width, f.info.height);
        else
            f.crop = CenteredCrop(f.info.width, f.info.height, AspectByName(m_aspect, f.info.width, f.info.height));
        f.outW = std::clamp(p.value("width", 0), 0, 8192);
        f.outH = std::clamp(p.value("height", 0), 0, 8192);
        if (m_req.tool == EditorTool::SaveFrames)
            f.marks = videotools::NormalizeFrameTimes(TimesFrom(p, "frames"), f.info.media.fps, dur);
        else if (m_req.tool == EditorTool::SplitVideo)
            f.marks = NormalizePoints(TimesFrom(p, "points"), dur);
        if (const auto it = p.find("ranges"); it != p.end() && it->is_array())
        {
            for (const auto& r : *it)
                if (r.is_array() && r.size() == 2 && r[0].is_number() && r[1].is_number())
                    f.ranges.push_back({ FromSeconds(r[0].get<double>()), FromSeconds(r[1].get<double>()) });
            f.ranges = NormalizeRanges(f.ranges, dur);
        }
    }

    // ---- Ranges on the timeline ----------------------------------------------------------------------------

    std::vector<TimeRange> EditorWindow::EditableRanges() const
    {
        const auto& f = Cur();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
            return { f.keep };
        case EditorTool::BleepAudio:
            return f.ranges;
        case EditorTool::RedactVideo:
            if (m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()))
                return { f.areas[static_cast<size_t>(m_selected)].range };
            return {};
        default:
            return {};
        }
    }

    int EditorWindow::SelectedRange() const
    {
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
            return 0;
        case EditorTool::BleepAudio:
            return m_selected;
        case EditorTool::RedactVideo:
            return m_selected >= 0 ? 0 : -1;
        default:
            return -1;
        }
    }

    void EditorWindow::SetEditableRange(size_t index, TimeRange r)
    {
        auto& f = Cur();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
            f.keep = r;
            break;
        case EditorTool::BleepAudio:
            if (index < f.ranges.size())
                f.ranges[index] = r;
            break;
        case EditorTool::RedactVideo:
            if (m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()))
                f.areas[static_cast<size_t>(m_selected)].range = r;
            break;
        default:
            break;
        }
    }

    // ---- The options panel ------------------------------------------------------------------------------------

    void EditorWindow::BuildPanel()
    {
        using K = ui::Control::Kind;
        auto& f = Cur();
        const bool ready = f.ready;
        const int64_t dur = Duration();
        const float x = m_rcPanel.x + 20, w = m_rcPanel.w - 40;
        float y = m_rcPanel.y + 20;
        const float bottom = m_rcPanel.Bottom() - 12;

        const auto caption = [&](const std::wstring& text) {
            m_texts.push_back({ { x, y, w, 18 }, text, ui::Font::Caption, 1 });
            y += 26;
        };
        const auto note = [&](const std::wstring& text, int tone = 1) {
            if (text.empty())
                return;
            const float h = m_painter.ParagraphHeight(text, w, ui::Font::Small);
            m_texts.push_back({ { x, y, w, h + 2 }, text, ui::Font::Small, tone, 0, true });
            y += h + 12;
        };
        const auto info = [&](const std::wstring& label, const std::wstring& value) {
            m_texts.push_back({ { x, y, 110, 22 }, label, ui::Font::Body, 1 });
            m_texts.push_back({ { x + 110, y, w - 110, 22 }, value, ui::Font::Number, 0, 2 });
            y += 26;
        };
        const auto add = [&](ui::Control c) { m_controls.push_back(std::move(c)); };
        // Choices in rows of `perRow`; arrows move between them.
        const auto segments = [&](int baseId, const std::vector<std::wstring>& labels, int chosen, std::function<void(int)> pick, int perRow = 4,
                                  const std::wstring& tip = {}, bool enabled = true) {
            const int n = static_cast<int>(labels.size());
            const int cols = std::min(n, perRow);
            const float gap = 4, sw = (w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            for (int i = 0; i < n; ++i)
            {
                ui::Control c;
                c.id = baseId + i;
                c.kind = K::Segment;
                c.rect = { x + static_cast<float>(i % cols) * (sw + gap), y + static_cast<float>(i / cols) * 36, sw, 32 };
                c.text = labels[static_cast<size_t>(i)];
                c.on = i == chosen;
                c.enabled = enabled && ready;
                c.tip = tip;
                c.click = [pick, i] { pick(i); };
                c.step = [pick, i, n](int d) { pick(std::clamp(i + d, 0, n - 1)); };
                add(std::move(c));
            }
            y += static_cast<float>((n + cols - 1) / cols) * 36 + 6;
        };
        const auto field = [&](int id, const std::wstring& label, const std::wstring& value, std::function<bool(const std::wstring&)> commit,
                               const std::wstring& tip, bool enabled = true) {
            ui::Control c;
            c.id = id;
            c.kind = K::Field;
            c.rect = { x, y, w, 32 };
            c.detail = label;
            c.text = value;
            c.tip = tip;
            c.enabled = enabled && ready;
            c.commit = std::move(commit);
            add(std::move(c));
            y += 40;
        };
        const auto button = [&](int id, const std::wstring& text, const wchar_t* glyph, std::function<void()> click, bool enabled = true,
                                const std::wstring& tip = {}, float bx = -1, float bw = -1) {
            ui::Control c;
            c.id = id;
            c.kind = K::Button;
            c.rect = { bx < 0 ? x : bx, y, bw < 0 ? w : bw, 32 };
            c.text = text;
            c.glyph = glyph;
            c.tip = tip;
            c.enabled = enabled && ready;
            c.click = std::move(click);
            add(std::move(c));
            if (bx < 0 || bx + bw >= x + w - 1)
                y += 40;
        };
        const auto toggle = [&](int id, const std::wstring& text, bool on, std::function<void()> flip, bool enabled = true, const std::wstring& tip = {}) {
            ui::Control c;
            c.id = id;
            c.kind = K::Toggle;
            c.rect = { x, y, w, 36 };
            c.text = text;
            c.on = on;
            c.tip = tip;
            c.enabled = enabled && ready;
            c.click = flip;
            c.step = [flip, on](int d) {
                if ((d > 0) != on)
                    flip();
            };
            add(std::move(c));
            y += 44;
        };
        const auto slider = [&](int id, const std::wstring& label, double value, double lo, double hi, std::function<std::wstring(double)> format,
                                std::function<void(double)> set, double stepBy, bool enabled = true) {
            ui::Control c;
            c.id = id;
            c.kind = K::Slider;
            c.rect = { x, y, w, 44 };
            c.text = label;
            c.detail = format(value);
            c.value = (value - lo) / (hi - lo);
            c.enabled = enabled && ready;
            c.slide = [set, lo, hi](double pos) { set(lo + pos * (hi - lo)); };
            c.step = [set, value, lo, hi, stepBy](int d) { set(std::clamp(value + d * stepBy, lo, hi)); };
            add(std::move(c));
            y += 52;
        };
        // A list of items, as many as fit above `reserve`; each row selects, its x button removes.
        const auto rows = [&](const std::vector<std::pair<std::wstring, std::wstring>>& items, int selected, std::function<void(int)> select,
                              std::function<void(int)> remove, float reserve) {
            const int fit = std::max(1, static_cast<int>((bottom - reserve - y) / 36));
            const int n = static_cast<int>(items.size());
            // Keep the selected row in view.
            const int first = selected >= fit ? std::min(selected - fit + 1, n - fit) : 0;
            for (int i = first; i < n && i < first + fit; ++i)
            {
                ui::Control c;
                c.id = kIdRows + i;
                c.kind = K::Row;
                c.rect = { x - 4, y, w + 8, 34 };
                c.text = items[static_cast<size_t>(i)].first;
                c.detail = items[static_cast<size_t>(i)].second;
                c.on = i == selected;
                c.click = [select, i] { select(i); };
                add(std::move(c));
                ui::Control del;
                del.id = kIdRowDelete + i;
                del.kind = K::Icon;
                del.rect = { x + w - 24, y + 5, 24, 24 };
                del.glyph = ui::icon::Close;
                del.tip = L"Remove (Delete)";
                del.click = [remove, i] { remove(i); };
                add(std::move(del));
                // The detail text makes room for the remove button.
                m_controls[m_controls.size() - 2].rect.w -= 28;
                y += 36;
            }
            if (n > fit)
                note(std::format(L"{} more \u2013 use \u2191 \u2193 on the timeline to go through them.", n - fit));
            y += 4;
        };
        const auto rangeFields = [&](TimeRange r, std::function<void(TimeRange)> set, const std::wstring& startTip, const std::wstring& endTip) {
            field(kIdStart, L"From", FormatTimecode(r.start),
                  [r, set, dur](const std::wstring& text) {
                      int64_t us = 0;
                      if (!ParseTimecode(text, us) || us > r.end - kMinRangeUs || us > dur)
                          return false;
                      set({ us, r.end });
                      return true;
                  },
                  startTip);
            field(kIdEnd, L"To", FormatTimecode(r.end),
                  [r, set, dur](const std::wstring& text) {
                      int64_t us = 0;
                      if (!ParseTimecode(text, us) || us < r.start + kMinRangeUs || us > dur + 500)
                          return false;
                      set({ r.start, std::min(us, dur) });
                      return true;
                  },
                  endTip);
        };
        const float half = (w - 8) / 2;
        const auto restart = [this] {
            if (m_playing && m_playback)
                StartPlay(m_playback->PositionUs(), m_playEnd);
        };

        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
        {
            const bool video = m_req.tool == EditorTool::TrimVideo;
            const bool remove = video && m_remove;
            if (video)
                segments(kIdTrimPart, { L"Keep selection", L"Remove selection" }, m_remove ? 1 : 0, [this](int i) { m_remove = i == 1; Rebuild(); }, 2,
                         L"Keep the selected part, or cut it out and join what is on either side");
            caption(remove ? L"PART TO REMOVE" : L"PART TO KEEP");
            const std::wstring part = remove ? L"removed" : L"kept";
            rangeFields(
                f.keep, [this](TimeRange r) { Cur().keep = r; Rebuild(); }, std::format(L"Where the {} part starts (I sets it at the playhead)", part),
                std::format(L"Where the {} part ends (O sets it at the playhead)", part));
            if (remove)
            {
                info(L"Removed", FormatTimecode(f.keep.Length()));
                info(L"Result", FormatTimecode(videotools::TrimmedLength(f.keep, dur, true)));
            }
            else
                info(L"Length", FormatTimecode(f.keep.Length()));
            button(kIdStartHere, L"Start here", nullptr, [this] { ToolKey('I', false, false); }, true, std::format(L"Start the {} part at the playhead (I)", part), x, half);
            button(kIdEndHere, L"End here", nullptr, [this] { ToolKey('O', false, false); }, true, std::format(L"End the {} part at the playhead (O)", part), x + half + 8, half);
            button(kIdPlaySel, remove ? L"Play the selection" : L"Play the kept part", ui::icon::Play, [this] { StopPlay(); TogglePlay(true); }, true, L"Shift+Space");
            y += 6;
            if (video)
            {
                // Copying the streams only keeps one part as it is: removing a part or fading re-encodes.
                const bool copies = videotools::TrimCopies(true, { m_remove, m_fade });
                caption(L"CUT");
                segments(kIdMode, { L"Exact", L"Fast" }, m_fast && copies ? 1 : 0, [this](int i) { m_fast = i == 1; Rebuild(); }, 4,
                         copies ? std::wstring() : L"Removing a part or fading always re-encodes", copies);
                note(!copies  ? L"Removing a part or fading re-encodes, so the cuts land on the exact frames."
                     : m_fast ? L"Copies the video as it is: instant and lossless, but the cut lands on a keyframe, so the start can move a little earlier."
                              : L"Re-encodes so the cut lands on the exact frame. Slower than Fast.");
                toggle(kIdFade, L"Fade in and out", m_fade, [this] { m_fade = !m_fade; Rebuild(); }, true,
                       L"Half a second of fade at each cut, picture and sound");
                if (m_fade)
                    note(remove ? L"The picture fades to black and back, and the sound out and in, where the two parts meet."
                                : L"The picture fades from and to black, and the sound in and out, where the kept part was cut.");
            }
            else
            {
                caption(L"SILENCE");
                toggle(kIdSilence, L"Also trim silence at both ends", m_silence, [this] { m_silence = !m_silence; Rebuild(); });
                note(L"Quiet below \u221250 dB for at least 0.1 s is cut from the start and the end of the kept part.");
            }
            break;
        }
        case EditorTool::CropVideo:
        {
            caption(L"SHAPE");
            int chosen = 0;
            for (int i = 0; i < 6; ++i)
                if (m_aspect == kAspects[i])
                    chosen = i;
            segments(kIdAspect, std::vector<std::wstring>(kAspectLabels, kAspectLabels + 6), chosen,
                     [this](int i) {
                         m_aspect = kAspects[i];
                         auto& file = Cur();
                         if (file.ready)
                         {
                             file.crop = CenteredCrop(file.info.width, file.info.height, AspectByName(m_aspect, file.info.width, file.info.height));
                             file.outW = file.outH = 0;
                         }
                         Rebuild();
                     },
                     3);
            caption(L"AREA");
            info(L"Size", std::format(L"{} \u00D7 {}", f.crop.w, f.crop.h));
            info(L"Position", std::format(L"{}, {}", f.crop.x, f.crop.y));
            y += 4;
            caption(L"OUTPUT SIZE");
            int ow = 0, oh = 0;
            CropOutputSize(f.crop, f.outW, f.outH, ow, oh);
            field(kIdOutW, L"Width", std::to_wstring(ow),
                  [this](const std::wstring& text) {
                      int v = 0;
                      if (!ParseInt(text, 16, 8192, v))
                          return false;
                      Cur().outW = v;
                      Cur().outH = 0;
                      return true;
                  },
                  L"The video's width in pixels; the height follows the area's shape");
            field(kIdOutH, L"Height", std::to_wstring(oh),
                  [this](const std::wstring& text) {
                      int v = 0;
                      if (!ParseInt(text, 16, 8192, v))
                          return false;
                      Cur().outH = v;
                      Cur().outW = 0;
                      return true;
                  },
                  L"The video's height in pixels; the width follows the area's shape");
            button(kIdOutReset, L"Same as the area", nullptr, [this] { Cur().outW = Cur().outH = 0; Rebuild(); }, f.outW > 0 || f.outH > 0,
                   L"Output the cropped area at its own size");
            note(L"Drag on the picture to draw the area, or drag its edges and corners. The sound is kept as it is.");
            break;
        }
        case EditorTool::SpeedVideo:
        {
            caption(L"SPEED");
            int chosen = -1;
            std::vector<std::wstring> labels;
            for (size_t i = 0; i < kSpeeds.size(); ++i)
            {
                labels.push_back(videotools::SpeedLabel(kSpeeds[i]));
                if (std::abs(kSpeeds[i] - m_factor) < 1e-6)
                    chosen = static_cast<int>(i);
            }
            segments(kIdSpeed, labels, chosen, [this, restart](int i) { m_factor = kSpeeds[static_cast<size_t>(i)]; restart(); Rebuild(); });
            field(kIdSpeedCustom, L"Custom", Num(m_factor) + L"\u00D7",
                  [this, restart](const std::wstring& text) {
                      double v = 0;
                      if (!ParseSpeed(text, v) || v < videotools::kMinSpeed - 1e-9 || v > videotools::kMaxSpeed + 1e-9 || std::abs(v - 1) < 1e-3)
                          return false;
                      m_factor = std::round(v * 1000) / 1000;
                      restart();
                      return true;
                  },
                  L"Any speed from 0.25\u00D7 to 4\u00D7, like 1.8 or 180%");
            const bool sound = std::any_of(m_files.begin(), m_files.end(), [](const FileState& file) { return file.ready && file.info.HasAudio(); });
            toggle(kIdPitch, L"Keep pitch", m_keepPitch && sound, [this, restart] { m_keepPitch = !m_keepPitch; restart(); Rebuild(); }, sound,
                   L"Off: the sound plays faster or slower like a tape, its pitch going up or down with it");
            caption(L"RESULT");
            info(L"Length", FormatTimecode(static_cast<int64_t>(static_cast<double>(dur) / m_factor)));
            info(L"Was", FormatTimecode(dur));
            y += 4;
            note(!ready          ? L""
                 : !sound        ? L"This video has no sound; it plays silent at the new speed."
                 : m_keepPitch   ? L"The sound keeps its pitch. Space plays at the new speed."
                                 : L"The pitch follows the speed: higher when faster, lower when slower. Space plays at the new speed.");
            if (m_files.size() > 1)
                note(std::format(L"Applies to all {} videos.", m_files.size()));
            break;
        }
        case EditorTool::JoinVideos:
        {
            caption(L"ORDER");
            note(L"Drag the clips to change their order, or select one and press Alt+\u2191 or Alt+\u2193.");
            const bool sel = m_selected >= 0 && m_selected < static_cast<int>(m_order.size());
            const float third = (w - 8) / 3;
            button(kIdUp, L"Up", nullptr, [this] { ToolKey(VK_UP, false, true); }, sel && m_selected > 0, L"Move the clip earlier (Alt+\u2191)", x, third);
            button(kIdDown, L"Down", nullptr, [this] { ToolKey(VK_DOWN, false, true); }, sel && m_selected + 1 < static_cast<int>(m_order.size()),
                   L"Move the clip later (Alt+\u2193)", x + third + 4, third);
            button(kIdRemove, L"Remove", nullptr, [this] { ToolKey(VK_DELETE, false, false); }, sel && m_order.size() > 1,
                   L"Leave this clip out (Delete)", x + 2 * third + 8, third);
            button(kIdAddClips, L"Add clips\u2026", ui::icon::Add, [this] { AddClips(); }, true, L"Add more videos to the end of the list");
            y += 6;
            // Clips of another shape than the first: whole with black bars, or cropped to fill the frame.
            caption(L"OTHER SHAPES");
            segments(kIdJoinFit, { L"Fit", L"Fill" }, m_joinFill ? 1 : 0, [this](int i) { m_joinFill = i == 1; Rebuild(); }, 2);
            y += 6;
            caption(L"RESULT");
            std::vector<ProbeInfo> clips;
            bool allProbed = true;
            int64_t total = 0;
            for (const int i : m_order)
            {
                const auto& clip = m_files[static_cast<size_t>(i)];
                allProbed = allProbed && clip.probed;
                if (clip.ready)
                {
                    clips.push_back(clip.info);
                    total += clip.info.DurationUs();
                }
            }
            if (!clips.empty())
            {
                int cw = 0, chh = 0;
                double fps = 0;
                videotools::JoinCanvas(clips.front(), cw, chh, fps);
                info(L"Size", std::format(L"{} \u00D7 {}", cw, chh));
                info(L"Frame rate", Num(std::round(fps * 100) / 100) + L" fps");
                info(L"Length", FormatTimecode(total));
                info(L"Clips", std::to_wstring(clips.size()));
                y += 4;
            }
            if (!allProbed)
                note(L"Checking the clips\u2026");
            else if (clips.size() < 2)
                note(L"Join needs at least two videos.", 3);
            else if (videotools::CanJoinByCopy(clips))
                note(L"The clips match, so they are joined without re-encoding: quick, and nothing is lost.");
            else
                note(m_joinFill ? L"The clips differ, so they are re-encoded to the first clip's size and frame rate. Other shapes are cropped to fill the frame."
                                : L"The clips differ, so they are re-encoded to the first clip's size and frame rate. Other shapes get black bars.");
            break;
        }
        case EditorTool::SaveFrames:
        {
            caption(L"FRAMES TO SAVE");
            segments(kIdFramePick, { L"Marked", L"Every N", L"Per second" }, static_cast<int>(m_frames.pick),
                     [this](int i) {
                         m_frames.pick = static_cast<videotools::FramePick>(i);
                         m_selected = -1;
                         Rebuild();
                     },
                     3);
            const double fps = ready ? f.info.media.fps : 0;
            if (m_frames.pick == videotools::FramePick::Marks)
            {
                button(kIdAdd, L"Add this frame", ui::icon::Add, [this] { ToolKey('M', false, false); }, true, L"M, or double-click the timeline");
                std::vector<std::pair<std::wstring, std::wstring>> items;
                for (size_t i = 0; i < f.marks.size(); ++i)
                    items.push_back({ std::format(L"Frame {}", i + 1), FormatTimecode(f.marks[i]) });
                rows(items, m_selected, [this](int i) { m_selected = i; SetPlayhead(Cur().marks[static_cast<size_t>(i)]); Rebuild(); },
                     [this](int i) { m_selected = i; ToolKey(VK_DELETE, false, false); }, 250);
                if (f.marks.empty())
                    note(L"Nothing marked yet: the frame at the playhead is saved.");
            }
            else if (m_frames.pick == videotools::FramePick::Every)
            {
                field(kIdEvery, L"Every", std::format(L"{} frames", m_frames.every),
                      [this](const std::wstring& text) {
                          double v = 0;
                          if (!ParseNumber(text, v) || v != std::floor(v) || v < 1 || v > videotools::kMaxEvery)
                              return false;
                          m_frames.every = static_cast<int>(v);
                          return true;
                      },
                      L"Save the first frame, then one in every N: 1 saves them all");
                if (fps > 0)
                    info(L"That is", Num(std::round(fps / m_frames.every * 100) / 100) + L" a second");
                else if (ready)
                    note(L"This video's frame rate is unknown, so every Nth frame cannot be counted. Use Per second instead.", 3);
            }
            else
            {
                field(kIdRate, L"Per second", std::format(L"{} a second", Num(m_frames.perSecond)),
                      [this](const std::wstring& text) {
                          double v = 0;
                          if (!ParseNumber(text, v) || v < videotools::kMinPerSecond - 1e-9 || v > videotools::kMaxPerSecond + 1e-9)
                              return false;
                          m_frames.perSecond = std::round(v * 100) / 100;
                          return true;
                      },
                      L"How many frames to save for each second of video, 0.01 to 60, like 2 or 0.5");
                info(L"One every", FormatRulerLabel(static_cast<int64_t>(std::llround(1e6 / m_frames.perSecond)), 100'000));
            }
            y += 4;
            caption(L"FORMAT");
            int format = 0;
            for (int i = 0; i < 3; ++i)
                if (m_frameFormat == kFrameFormats[i])
                    format = i;
            segments(kIdFormat, { L"PNG", L"JPG", L"WebP" }, format, [this](int i) { m_frameFormat = kFrameFormats[i]; Rebuild(); }, 3);
            caption(L"RESULT");
            const int64_t count = FrameCount(f);
            const uint64_t each = ready ? videotools::FrameFileBytes(f.info.width, f.info.height, m_frameFormat) : 0;
            info(L"Frames", count > 0 ? std::to_wstring(count) : L"\u2013");
            info(L"Each", each > 0 ? L"about " + SizeText(each) : L"\u2013");
            info(L"In all", each > 0 && count > 0 ? L"about " + SizeText(each * static_cast<uint64_t>(count)) : L"\u2013");
            note(format == 0   ? L"Full size, lossless, together in one folder next to the video."
                 : format == 1 ? std::format(L"Full size at quality {}, together in one folder next to the video.", videotools::FrameQuality("jpg"))
                               : std::format(L"Full size at quality {}, the smallest files, together in one folder next to the video.",
                                             videotools::FrameQuality("webp")));
            break;
        }
        case EditorTool::SplitVideo:
        {
            caption(L"SPLIT");
            segments(kIdSplitKind, { L"At marks", L"Equal parts", L"Every" }, static_cast<int>(m_split),
                     [this](int i) {
                         m_split = static_cast<SplitKind>(i);
                         m_selected = -1;
                         Rebuild();
                     },
                     3);
            if (m_split == SplitKind::Equal)
            {
                field(kIdParts, L"Parts", std::to_wstring(m_parts),
                      [this](const std::wstring& text) {
                          int v = 0;
                          if (!ParseInt(text, 2, 100, v))
                              return false;
                          m_parts = v;
                          return true;
                      },
                      L"How many equal parts, 2 to 100");
                info(L"Each", FormatTimecode(dur / std::max(1, m_parts)));
            }
            else if (m_split == SplitKind::Every)
            {
                field(kIdSplitEvery, L"Every", FormatRulerLabel(m_splitEvery, m_splitEvery % 1'000'000 != 0 ? 100'000 : 1'000'000),
                      [this, dur](const std::wstring& text) {
                          int64_t us = 0;
                          if (!ParseTimecode(text, us) || us < 1'000'000 || EveryPoints(dur, us).size() + 1 > static_cast<size_t>(videotools::kMaxSplitParts))
                              return false;
                          m_splitEvery = us;
                          return true;
                      },
                      L"How long each part is, like 0:30, 1:30 or 90 (seconds); the last part is what is left");
                const auto parts = PartsFromPoints(SplitPoints(f), dur);
                info(L"Parts", std::to_wstring(parts.size()));
                info(L"Last part", parts.empty() ? std::wstring(L"\u2013") : FormatTimecode(parts.back().Length()));
            }
            else
            {
                button(kIdAdd, L"Cut here", ui::icon::Add, [this] { ToolKey('M', false, false); }, true, L"M, or double-click the timeline");
                std::vector<std::pair<std::wstring, std::wstring>> items;
                for (size_t i = 0; i < f.marks.size(); ++i)
                    items.push_back({ std::format(L"Cut {}", i + 1), FormatTimecode(f.marks[i]) });
                rows(items, m_selected, [this](int i) { m_selected = i; SetPlayhead(Cur().marks[static_cast<size_t>(i)]); Rebuild(); },
                     [this](int i) { m_selected = i; ToolKey(VK_DELETE, false, false); }, 170);
                info(L"Parts", std::to_wstring(f.marks.size() + 1));
            }
            y += 4;
            caption(L"CUT");
            segments(kIdMode, { L"Exact", L"Fast" }, m_fast ? 1 : 0, [this](int i) { m_fast = i == 1; Rebuild(); });
            note(m_fast ? L"Copies the video as it is: instant and lossless, but each cut moves to the nearest keyframe."
                        : L"Re-encodes each part so the cuts land on the exact frames.");
            break;
        }
        case EditorTool::RedactVideo:
        {
            caption(L"AREAS");
            if (f.areas.empty())
                note(L"Drag on the picture to cover a face, a name or a number plate. Each area can show for part of the video.");
            std::vector<std::pair<std::wstring, std::wstring>> items;
            for (size_t i = 0; i < f.areas.size(); ++i)
                items.push_back({ std::format(L"Area {}{}", i + 1, f.areas[i].blur ? L"" : L" (solid)"), SpanText(f.areas[i].range) });
            rows(items, m_selected,
                 [this](int i) {
                     m_selected = i;
                     const auto& range = Cur().areas[static_cast<size_t>(i)].range;
                     if (!range.Contains(m_playhead))
                         SetPlayhead(range.start);
                     Rebuild();
                 },
                 [this](int i) { m_selected = i; ToolKey(VK_DELETE, false, false); }, m_selected >= 0 ? 260 : 90);
            if (m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()))
            {
                auto& area = f.areas[static_cast<size_t>(m_selected)];
                caption(std::format(L"AREA {}", m_selected + 1));
                segments(kIdStyle, { L"Blur", L"Solid" }, area.blur ? 0 : 1,
                         [this](int i) {
                             auto& a = Cur().areas[static_cast<size_t>(m_selected)];
                             a.blur = i == 0;
                             m_blurStyle = a.blur;
                             Rebuild();
                         },
                         2);
                rangeFields(
                    area.range, [this](TimeRange r) { Cur().areas[static_cast<size_t>(m_selected)].range = r; Rebuild(); },
                    L"When the area starts to show (I sets it at the playhead)", L"When it stops (O)");
                button(kIdStartHere, L"Start here", nullptr, [this] { ToolKey('I', false, false); }, true, L"I", x, half);
                button(kIdEndHere, L"End here", nullptr, [this] { ToolKey('O', false, false); }, true, L"O", x + half + 8, half);
                button(kIdWhole, L"Whole video", nullptr,
                       [this, dur] {
                           Cur().areas[static_cast<size_t>(m_selected)].range = { 0, dur };
                           Rebuild();
                       },
                       area.range != TimeRange{ 0, dur });
            }
            else
            {
                caption(L"NEW AREAS");
                segments(kIdStyle, { L"Blur", L"Solid" }, m_blurStyle ? 0 : 1, [this](int i) { m_blurStyle = i == 0; Rebuild(); }, 2);
            }
            break;
        }
        case EditorTool::AudioChannels:
        {
            caption(L"OUTPUT");
            segments(kIdMono, { L"Stereo", L"Mono" }, m_mono ? 1 : 0, [this, restart](int i) { m_mono = i == 1; restart(); Rebuild(); }, 2);
            toggle(kIdSwap, L"Swap left and right", m_swap && !m_mono, [this, restart] { m_swap = !m_swap; restart(); Rebuild(); }, !m_mono);
            caption(L"VOLUME");
            const auto pct = [](double v) { return std::format(L"{}%", static_cast<int>(std::lround(v * 100))); };
            const auto set = [this, restart](double& target) {
                return [this, restart, &target](double v) {
                    target = std::clamp(std::round(v * 20) / 20, 0.0, 2.0);   // 5% steps
                    if (m_drag != Drag::Slider)
                        restart();
                    Rebuild();
                };
            };
            slider(kIdLeft, m_mono ? L"Left into the mix" : L"Left", m_left, 0, 2, pct, set(m_left), 0.05);
            slider(kIdRight, m_mono ? L"Right into the mix" : L"Right", m_right, 0, 2, pct, set(m_right), 0.05);
            button(kIdResetVol, L"Reset volumes", nullptr, [this, restart] { m_left = m_right = 1; restart(); Rebuild(); },
                   std::abs(m_left - 1) > 1e-6 || std::abs(m_right - 1) > 1e-6);
            caption(L"LISTEN TO");
            segments(kIdListen, { L"Original", L"Result" }, m_listenProcessed ? 1 : 0, [this](int i) { m_listenProcessed = i == 1; if (m_playing) StartPlay(m_playback->PositionUs(), m_playEnd); Rebuild(); }, 2,
                     L"A switches while playing");
            if (ready && f.info.channels > 2)
                note(L"This file has more than two channels; they are mixed down to stereo first.");
            if (m_files.size() > 1)
                note(std::format(L"Applies to all {} files.", m_files.size()));
            break;
        }
        case EditorTool::BleepAudio:
        {
            caption(L"SOUND");
            segments(kIdTone, { L"Beep", L"Silence", L"Duck" }, static_cast<int>(m_bleep),
                     [this, restart](int i) {
                         m_bleep = static_cast<audiotools::BleepSound>(i);
                         restart();
                         Rebuild();
                     },
                     3, L"A 1 kHz tone, nothing, or the sound turned down");
            if (m_bleep == audiotools::BleepSound::Duck)
                slider(kIdDuck, L"Lower by", m_duckDb, audiotools::kMinDuckDb, audiotools::kMaxDuckDb,
                       [](double v) { return std::format(L"{} dB", std::lround(v)); },
                       [this, restart](double v) {
                           m_duckDb = std::clamp(std::round(v), audiotools::kMinDuckDb, audiotools::kMaxDuckDb);
                           if (m_drag != Drag::Slider)
                               restart();
                           Rebuild();
                       },
                       1);
            caption(L"PARTS TO BLEEP");
            button(kIdAdd, L"Add at the playhead", ui::icon::Add, [this] { ToolKey('M', false, false); }, true, L"M, or drag across the waveform");
            std::vector<std::pair<std::wstring, std::wstring>> items;
            for (size_t i = 0; i < f.ranges.size(); ++i)
                items.push_back({ std::format(L"Bleep {}", i + 1), SpanText(f.ranges[i]) });
            rows(items, m_selected,
                 [this](int i) {
                     m_selected = i;
                     SetPlayhead(Cur().ranges[static_cast<size_t>(i)].start);
                     Rebuild();
                 },
                 [this](int i) { m_selected = i; ToolKey(VK_DELETE, false, false); }, m_selected >= 0 ? 200 : 120);
            if (m_selected >= 0 && m_selected < static_cast<int>(f.ranges.size()))
                rangeFields(
                    f.ranges[static_cast<size_t>(m_selected)],
                    [this](TimeRange r) {
                        auto& file = Cur();
                        file.ranges[static_cast<size_t>(m_selected)] = r;
                        file.ranges = NormalizeRanges(file.ranges, file.info.DurationUs());
                        m_selected = std::min<int>(m_selected, static_cast<int>(file.ranges.size()) - 1);
                        Rebuild();
                    },
                    L"Where the bleep starts (I)", L"Where it ends (O)");
            caption(L"LISTEN TO");
            segments(kIdListen, { L"Original", L"Bleeped" }, m_listenProcessed ? 1 : 0, [this](int i) { m_listenProcessed = i == 1; if (m_playing) StartPlay(m_playback->PositionUs(), m_playEnd); Rebuild(); }, 2,
                     L"A switches while playing");
            button(kIdPlaySel, L"Play around the selected bleep", ui::icon::Play, [this] { StopPlay(); TogglePlay(true); }, m_selected >= 0, L"Shift+Space");
            break;
        }
        case EditorTool::AudioVisualizer:
        {
            caption(L"STYLE");
            segments(kIdPicStyle, { L"Bars", L"Wave", L"Circle", L"Square", L"Vertical", L"Picture" }, static_cast<int>(m_style),
                     [this](int i) {
                         m_style = static_cast<audiotools::VisualStyle>(i);
                         Rebuild();
                     },
                     3);
            const bool still = m_style == audiotools::VisualStyle::Picture;
            caption(still ? L"PICTURE" : L"BACKGROUND");
            button(kIdChoose, m_image.empty() ? L"Choose a picture\u2026" : L"Choose another\u2026", ui::icon::Picture, [this] { ChoosePicture(); });
            if (!m_image.empty())
            {
                info(L"Picture", FileName(m_image));
                button(kIdClear, still ? L"Use the cover art instead" : L"No picture", nullptr, [this] { m_image.clear(); m_imagePicture = {}; Rebuild(); });
            }
            else
                note(still ? L"Without a picture, each song's own cover art is used; a song without one gets the waveform."
                           : L"Without a picture the background is plain dark. A picture is fitted to fill the video and darkened a little.");
            caption(L"SHAPE");
            int chosen = 0;
            for (int i = 0; i < 3; ++i)
                if (m_shape == kShapes[i])
                    chosen = i;
            segments(kIdShape, { L"Landscape", L"Portrait", L"Square" }, chosen, [this](int i) { m_shape = kShapes[i]; Rebuild(); }, 3);
            int vw = 0, vh = 0;
            audiotools::ShapeSize(audiotools::ShapeByName(m_shape), vw, vh);
            info(L"Video", std::format(L"{} \u00D7 {}, 30 fps", vw, vh));
            y += 4;
            note(L"The video gets the song's sound and length, as an MP4.");
            if (m_files.size() > 1)
                note(std::format(L"Makes one video for each of the {} songs.", m_files.size()));
            break;
        }
        }
    }

    std::wstring EditorWindow::PrimaryLabel() const
    {
        if (PerFile())
        {
            int open = 0;
            for (const auto& file : m_files)
                open += file.status == Status::Open ? 1 : 0;
            if (open > 1)
                return L"Next file";
        }
        const auto& f = Cur();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio: return m_req.tool == EditorTool::TrimVideo && m_remove ? L"Remove part" : L"Trim";
        case EditorTool::CropVideo: return L"Crop";
        case EditorTool::SpeedVideo: return L"Make it " + videotools::SpeedLabel(m_factor);
        case EditorTool::JoinVideos: return std::format(L"Join {} clips", m_order.size());
        case EditorTool::SaveFrames:
        {
            if (m_frames.pick == videotools::FramePick::Marks && f.marks.empty())
                return L"Save this frame";
            const int64_t count = FrameCount(f);
            return count <= 0 ? L"Save frames" : count == 1 ? L"Save 1 frame" : std::format(L"Save {} frames", count);
        }
        case EditorTool::SplitVideo: return std::format(L"Split into {} parts", SplitPoints(f).size() + 1);
        case EditorTool::RedactVideo: return L"Redact";
        case EditorTool::AudioChannels: return m_mono ? L"Make mono" : L"Convert";
        case EditorTool::BleepAudio: return L"Bleep";
        case EditorTool::AudioVisualizer: return m_files.size() > 1 ? std::format(L"Create {} videos", m_files.size()) : L"Create video";
        }
        return L"OK";
    }

    bool EditorWindow::PrimaryEnabled() const
    {
        const auto& f = Cur();
        if (m_req.tool == EditorTool::JoinVideos)
        {
            int usable = 0;
            for (const int i : m_order)
                usable += m_files[static_cast<size_t>(i)].ready ? 1 : 0;
            return usable >= 2;
        }
        if (!PerFile())
            return std::any_of(m_files.begin(), m_files.end(), [](const FileState& file) { return file.ready; }) &&
                   (m_req.tool != EditorTool::SpeedVideo || std::abs(m_factor - 1) > 1e-3) &&
                   (m_req.tool != EditorTool::AudioChannels || m_mono || m_swap || std::abs(m_left - 1) > 1e-6 || std::abs(m_right - 1) > 1e-6 ||
                    !f.ready || f.info.channels != 2);
        if (!f.ready)
            return false;
        const int64_t dur = f.info.DurationUs();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
            return m_remove ? videotools::TrimmedLength(f.keep, dur, true) >= kMinRangeUs && f.keep.Length() > 0 : f.keep.start > 0 || f.keep.end < dur;
        case EditorTool::TrimAudio: return f.keep.start > 0 || f.keep.end < dur || m_silence;
        case EditorTool::CropVideo:
        {
            int ow = 0, oh = 0;
            CropOutputSize(f.crop, f.outW, f.outH, ow, oh);
            return f.crop != PixelRect{ 0, 0, f.info.width, f.info.height } || ow != f.info.width || oh != f.info.height;
        }
        case EditorTool::SplitVideo: return !SplitPoints(f).empty();
        case EditorTool::SaveFrames: return FrameCount(f) > 0;
        case EditorTool::RedactVideo: return !f.areas.empty();
        case EditorTool::BleepAudio: return !f.ranges.empty();
        default: return true;
        }
    }

    std::wstring EditorWindow::Hint() const
    {
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
            return m_remove ? L"I and O set the start and end of the part to remove  \u00B7  \u2190 \u2192 step one frame  \u00B7  Shift+Space plays it  \u00B7  The wheel zooms"
                            : L"I and O set the start and end at the playhead  \u00B7  \u2190 \u2192 step one frame  \u00B7  Shift+Space plays the kept part  \u00B7  The wheel zooms";
        case EditorTool::TrimAudio: return L"I and O set the start and end  \u00B7  Space plays  \u00B7  Shift+Space plays the kept part  \u00B7  The wheel zooms";
        case EditorTool::CropVideo: return L"Drag on the picture to draw the area  \u00B7  Drag its edges or corners to adjust  \u00B7  \u2190 \u2192 step one frame";
        case EditorTool::SpeedVideo: return L"Space plays at the new speed  \u00B7  \u2190 \u2192 step one frame";
        case EditorTool::JoinVideos: return L"Drag to reorder  \u00B7  Alt+\u2191 Alt+\u2193 move the selected clip  \u00B7  Delete leaves it out";
        case EditorTool::SaveFrames:
            return m_frames.pick == videotools::FramePick::Marks
                       ? L"M or a double-click adds the frame at the playhead  \u00B7  Drag a mark to move it  \u00B7  Delete removes it"
                       : L"The ticks on the timeline are the frames saved  \u00B7  M marks frames by hand instead";
        case EditorTool::SplitVideo:
            return m_split == SplitKind::Marks ? L"M or a double-click cuts at the playhead  \u00B7  Drag a cut to move it  \u00B7  Delete removes it"
                                               : L"The dashed lines are where it is cut  \u00B7  M marks cuts by hand instead";
        case EditorTool::RedactVideo: return L"Drag on the picture to add an area  \u00B7  I and O set when the selected area shows  \u00B7  Delete removes it";
        case EditorTool::AudioChannels: return L"Space plays  \u00B7  A switches between the original and the result";
        case EditorTool::BleepAudio: return L"Drag across the waveform to mark a part  \u00B7  M adds one at the playhead  \u00B7  A compares with the original";
        case EditorTool::AudioVisualizer: return L"Every song gets its own video, named after it.";
        }
        return {};
    }

    // ---- Mouse on the timeline and the picture ------------------------------------------------------------

    bool EditorWindow::TrackMouseDown(float x, float y, bool doubleClick)
    {
        auto& f = Cur();
        const int64_t t = TT(x);
        m_downTime = t;
        const auto tool = m_req.tool;
        if (MarksOnTrack())
        {
            if (doubleClick)
            {
                SetPlayhead(t, false);
                ToolKey('M', false, false);
                return true;
            }
            const int hit = HitMarker(m_view, f.marks, x - m_rcTrack.x, kGrip);
            if (hit >= 0)
            {
                m_selected = hit;
                m_drag = Drag::Marker;
                m_dragIndex = hit;
                SetPlayhead(f.marks[static_cast<size_t>(hit)], false);
                Rebuild();
                return true;
            }
        }
        if (tool == EditorTool::RedactVideo)
        {
            // The other areas' bars along the bottom of the track select them.
            for (size_t i = 0; i < f.areas.size() && i < 6; ++i)
            {
                const float by = m_rcTrack.Bottom() - 7 - static_cast<float>(i) * 6;
                if (y >= by - 2 && y <= by + 5 && x >= TX(f.areas[i].range.start) && x <= TX(f.areas[i].range.end) && static_cast<int>(i) != m_selected)
                {
                    m_selected = static_cast<int>(i);
                    Rebuild();
                    return true;
                }
            }
        }
        const auto ranges = EditableRanges();
        if (!ranges.empty())
        {
            const RangeHit hit = HitRange(m_view, ranges, x - m_rcTrack.x, kGrip);
            if (hit.index >= 0)
            {
                if (tool == EditorTool::BleepAudio)
                    m_selected = hit.index;
                m_dragIndex = hit.index;
                m_dragRange = ranges[static_cast<size_t>(hit.index)];
                if (hit.part == RangeHit::Part::Body)
                    m_drag = Drag::Body;
                else
                {
                    m_drag = Drag::Edge;
                    m_dragStartEdge = hit.part == RangeHit::Part::Start;
                }
                Rebuild();
                return true;
            }
        }
        if (tool == EditorTool::BleepAudio)
        {
            // A drag across empty waveform marks a new part; a click just moves the playhead.
            m_drag = Drag::NewRange;
            m_dragIndex = -1;
            int64_t lo = 0, hi = Duration();
            for (const auto& r : f.ranges)
            {
                if (r.end <= t)
                    lo = std::max(lo, r.end);
                if (r.start >= t)
                    hi = std::min(hi, r.start);
            }
            m_dragRange = { lo, hi };
            return true;
        }
        StopPlay();
        m_drag = Drag::Scrub;
        SetPlayhead(t, false);
        return true;
    }

    bool EditorWindow::StageMouseDown(float x, float y)
    {
        auto& f = Cur();
        if (m_req.tool == EditorTool::JoinVideos)
        {
            const int pos = static_cast<int>(std::floor((y - m_rcList.y + m_listScroll) / kJoinRowH));
            if (pos < 0 || pos >= static_cast<int>(m_order.size()))
                return false;
            m_selected = pos;
            m_drag = Drag::JoinRow;
            m_dragIndex = pos;
            m_joinDropAt = -1;
            Rebuild();
            return true;
        }
        if (!f.ready)
            return false;
        const auto map = StageMap();
        if (m_req.tool == EditorTool::CropVideo)
        {
            const int handle = HitBoxHandle(map.ToView(f.crop), x, y, 8);
            m_dragBox = f.crop;
            if (handle >= 0)
            {
                m_drag = Drag::CropHandle;
                m_dragHandle = handle;
                return true;
            }
            if (map.shown.Contains(x, y))
            {
                m_drag = Drag::CropNew;
                return true;
            }
            return false;
        }
        if (m_req.tool == EditorTool::RedactVideo)
        {
            if (m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()))
            {
                const int handle = HitBoxHandle(map.ToView(f.areas[static_cast<size_t>(m_selected)].box), x, y, 8);
                if (handle >= 0)
                {
                    m_drag = Drag::AreaHandle;
                    m_dragIndex = m_selected;
                    m_dragHandle = handle;
                    m_dragBox = f.areas[static_cast<size_t>(m_selected)].box;
                    return true;
                }
            }
            // The topmost area showing now (or the selected one) under the mouse.
            for (int i = static_cast<int>(f.areas.size()) - 1; i >= 0; --i)
            {
                const auto& a = f.areas[static_cast<size_t>(i)];
                if ((a.range.Contains(m_playhead) || i == m_selected) && map.ToView(a.box).Contains(x, y))
                {
                    m_selected = i;
                    m_drag = Drag::AreaHandle;
                    m_dragIndex = i;
                    m_dragHandle = 8;
                    m_dragBox = a.box;
                    Rebuild();
                    return true;
                }
            }
            if (map.shown.Contains(x, y))
            {
                m_drag = Drag::AreaNew;
                m_dragIndex = -1;
                return true;
            }
            m_selected = -1;
            Rebuild();
            return true;
        }
        return false;
    }

    void EditorWindow::DragTo(float x, float y)
    {
        auto& f = Cur();
        const int64_t dur = Duration();
        const auto snap = [this](int64_t t) { return Fps() > 0 ? SnapToFrame(t, Fps()) : t; };
        const auto map = StageMap();
        const auto toFrame = [&](float px, float py, int& fx, int& fy) {
            fx = std::clamp(map.ToFrameX(px), 0, map.frameW);
            fy = std::clamp(map.ToFrameY(py), 0, map.frameH);
        };
        switch (m_drag)
        {
        case Drag::Edge:
        {
            auto ranges = EditableRanges();
            if (m_dragIndex < 0 || m_dragIndex >= static_cast<int>(ranges.size()))
                break;
            const int64_t t = ClampEdge(ranges, static_cast<size_t>(m_dragIndex), m_dragStartEdge, snap(TT(x)), dur);
            TimeRange r = ranges[static_cast<size_t>(m_dragIndex)];
            (m_dragStartEdge ? r.start : r.end) = t;
            SetEditableRange(static_cast<size_t>(m_dragIndex), r);
            // The picture follows the edge being dragged: the first frame kept, or the last.
            if (Video())
                SetPlayhead(m_dragStartEdge ? t : t - FrameDurationUs(Fps()), false);
            Rebuild();
            break;
        }
        case Drag::Body:
        {
            if (!m_dragMoved)
                break;
            const auto ranges = EditableRanges();
            if (m_dragIndex < 0 || m_dragIndex >= static_cast<int>(ranges.size()))
                break;
            const int64_t lo = m_dragIndex > 0 ? ranges[static_cast<size_t>(m_dragIndex - 1)].end : 0;
            const int64_t hi = m_dragIndex + 1 < static_cast<int>(ranges.size()) ? ranges[static_cast<size_t>(m_dragIndex + 1)].start : dur;
            const int64_t len = m_dragRange.Length();
            const int64_t shift = static_cast<int64_t>((x - m_downX) * m_view.usPerPx);
            const int64_t start = snap(std::clamp(m_dragRange.start + shift, lo, std::max(lo, hi - len)));
            SetEditableRange(static_cast<size_t>(m_dragIndex), { start, std::min(hi, start + len) });
            Rebuild();
            break;
        }
        case Drag::NewRange:
        {
            if (!m_dragMoved)
                break;
            const int64_t t = std::clamp(TT(x), m_dragRange.start, m_dragRange.end);
            const TimeRange r{ std::min(t, m_downTime), std::max(t, m_downTime) };
            if (m_dragIndex < 0)
            {
                const auto at = std::lower_bound(f.ranges.begin(), f.ranges.end(), r, [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
                m_dragIndex = static_cast<int>(at - f.ranges.begin());
                f.ranges.insert(at, r);
                m_selected = m_dragIndex;
            }
            else
                f.ranges[static_cast<size_t>(m_dragIndex)] = r;
            Rebuild();
            break;
        }
        case Drag::Marker:
            if (m_dragIndex >= 0 && m_dragIndex < static_cast<int>(f.marks.size()))
            {
                const int64_t t = std::clamp<int64_t>(snap(TT(x)), 0, m_req.tool == EditorTool::SaveFrames ? LastFrameTime(Fps(), dur) : dur);
                f.marks[static_cast<size_t>(m_dragIndex)] = t;
                SetPlayhead(t, false);
                Rebuild();
            }
            break;
        case Drag::CropHandle:
        case Drag::AreaHandle:
        {
            if (map.shown.w <= 0)
                break;
            const int dx = static_cast<int>(std::lround((x - m_downX) * map.frameW / map.shown.w));
            const int dy = static_cast<int>(std::lround((y - m_downY) * map.frameH / map.shown.h));
            if (m_drag == Drag::CropHandle)
            {
                f.crop = DragCrop(m_dragBox, m_dragHandle, dx, dy, map.frameW, map.frameH, AspectByName(m_aspect, map.frameW, map.frameH));
                f.outW = f.outH = 0;
            }
            else if (m_dragIndex >= 0 && m_dragIndex < static_cast<int>(f.areas.size()))
                f.areas[static_cast<size_t>(m_dragIndex)].box = DragCrop(m_dragBox, m_dragHandle, dx, dy, map.frameW, map.frameH, {});
            Rebuild();
            break;
        }
        case Drag::CropNew:
        case Drag::AreaNew:
        {
            if (!m_dragMoved || map.shown.w <= 0)
                break;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            toFrame(m_downX, m_downY, x0, y0);
            toFrame(x, y, x1, y1);
            PixelRect box = RectFromCorners(x0, y0, x1, y1, map.frameW, map.frameH);
            if (m_drag == Drag::CropNew)
            {
                const Aspect aspect = AspectByName(m_aspect, map.frameW, map.frameH);
                if (!aspect.Free())
                {
                    // Keep the corner where the drag started; fit the shape inside the dragged box.
                    const double a = static_cast<double>(aspect.num) / aspect.den;
                    int w = box.w, h = box.h;
                    if (w > h * a)
                        w = static_cast<int>(h * a);
                    else
                        h = static_cast<int>(w / a);
                    box = { x1 < x0 ? x0 - w : x0, y1 < y0 ? y0 - h : y0, w, h };
                }
                f.crop = ClampCrop(box, map.frameW, map.frameH);
                f.outW = f.outH = 0;
            }
            else
            {
                box = ClampCrop(box, map.frameW, map.frameH);
                if (m_dragIndex < 0)
                {
                    f.areas.push_back({ box, { 0, dur }, m_blurStyle });
                    m_dragIndex = static_cast<int>(f.areas.size()) - 1;
                    m_selected = m_dragIndex;
                }
                else
                    f.areas[static_cast<size_t>(m_dragIndex)].box = box;
            }
            Rebuild();
            break;
        }
        case Drag::JoinRow:
            if (m_dragMoved)
                m_joinDropAt = std::clamp(static_cast<int>(std::lround((y - m_rcList.y + m_listScroll) / kJoinRowH)), 0, static_cast<int>(m_order.size()));
            break;
        default:
            break;
        }
    }

    void EditorWindow::ToolDragEnd(Drag was)
    {
        auto& f = Cur();
        const int64_t dur = Duration();
        switch (was)
        {
        case Drag::Body:
        case Drag::NewRange:
            if (!m_dragMoved)
                SetPlayhead(TT(m_downX), false);
            if (m_req.tool == EditorTool::BleepAudio)
            {
                // Merge what now overlaps; keep the dragged part selected.
                const int64_t keepAt = m_selected >= 0 && m_selected < static_cast<int>(f.ranges.size()) ? f.ranges[static_cast<size_t>(m_selected)].start : -1;
                f.ranges = NormalizeRanges(f.ranges, dur);
                m_selected = -1;
                for (size_t i = 0; i < f.ranges.size(); ++i)
                    if (keepAt >= 0 && f.ranges[i].start <= keepAt && keepAt <= f.ranges[i].end)
                        m_selected = static_cast<int>(i);
            }
            break;
        case Drag::Edge:
            if (m_req.tool == EditorTool::BleepAudio)
                f.ranges = NormalizeRanges(f.ranges, dur);
            break;
        case Drag::Marker:
            if (m_dragIndex >= 0 && m_dragIndex < static_cast<int>(f.marks.size()))
            {
                const int64_t moved = f.marks[static_cast<size_t>(m_dragIndex)];
                f.marks = m_req.tool == EditorTool::SaveFrames ? videotools::NormalizeFrameTimes(f.marks, Fps(), dur) : NormalizePoints(f.marks, dur);
                const auto it = std::lower_bound(f.marks.begin(), f.marks.end(), moved);
                m_selected = it == f.marks.end() ? static_cast<int>(f.marks.size()) - 1 : static_cast<int>(it - f.marks.begin());
            }
            break;
        case Drag::AreaNew:
            if (!m_dragMoved)
            {
                m_selected = -1;
                break;
            }
            break;
        case Drag::JoinRow:
            if (m_dragMoved && m_joinDropAt >= 0 && m_dragIndex >= 0 && m_joinDropAt != m_dragIndex && m_joinDropAt != m_dragIndex + 1)
            {
                const int item = m_order[static_cast<size_t>(m_dragIndex)];
                m_order.erase(m_order.begin() + m_dragIndex);
                const int to = m_joinDropAt > m_dragIndex ? m_joinDropAt - 1 : m_joinDropAt;
                m_order.insert(m_order.begin() + to, item);
                m_selected = to;
            }
            break;
        default:
            break;
        }
    }

    LPCWSTR EditorWindow::CursorAt(float x, float y) const
    {
        switch (m_drag)
        {
        case Drag::Edge:
        case Drag::Marker:
        case Drag::Body:
            return IDC_SIZEWE;
        case Drag::CropHandle:
        case Drag::AreaHandle:
            return HandleCursor(m_dragHandle);
        case Drag::CropNew:
        case Drag::AreaNew:
            return IDC_CROSS;
        case Drag::JoinRow:
            return IDC_SIZENS;
        default:
            break;
        }
        if (const int id = ControlAt(x, y))
        {
            const auto* c = ControlById(id);
            return c && c->kind == ui::Control::Kind::Field && c->enabled ? IDC_IBEAM : IDC_ARROW;
        }
        const auto& f = Cur();
        if (!f.ready)
            return IDC_ARROW;
        if (HasTimeline() && m_rcTrack.Contains(x, y))
        {
            if (MarksOnTrack() && HitMarker(m_view, f.marks, x - m_rcTrack.x, kGrip) >= 0)
                return IDC_SIZEWE;
            const auto hit = HitRange(m_view, EditableRanges(), x - m_rcTrack.x, kGrip);
            if (hit.part == RangeHit::Part::Start || hit.part == RangeHit::Part::End)
                return IDC_SIZEWE;
            return IDC_ARROW;
        }
        if (m_rcStage.Contains(x, y) && (m_req.tool == EditorTool::CropVideo || m_req.tool == EditorTool::RedactVideo))
        {
            const auto map = StageMap();
            const PixelRect* box = m_req.tool == EditorTool::CropVideo ? &f.crop
                                   : m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()) ? &f.areas[static_cast<size_t>(m_selected)].box
                                                                                                     : nullptr;
            if (box)
                if (const int handle = HitBoxHandle(map.ToView(*box), x, y, 8); handle >= 0)
                    return HandleCursor(handle);
            if (map.shown.Contains(x, y))
                return IDC_CROSS;
        }
        return IDC_ARROW;
    }

    // ---- Keys -----------------------------------------------------------------------------------------------

    bool EditorWindow::ToolKey(UINT vk, bool shift, bool ctrl)
    {
        auto& f = Cur();
        const auto tool = m_req.tool;
        const bool alt = GetKeyState(VK_MENU) < 0;
        if (tool == EditorTool::JoinVideos)
        {
            const int n = static_cast<int>(m_order.size());
            const bool sel = m_selected >= 0 && m_selected < n;
            if ((vk == VK_UP || vk == VK_DOWN) && (alt || ctrl) && sel)
            {
                const int to = m_selected + (vk == VK_UP ? -1 : 1);
                if (to >= 0 && to < n)
                {
                    std::swap(m_order[static_cast<size_t>(m_selected)], m_order[static_cast<size_t>(to)]);
                    m_selected = to;
                }
                Rebuild();
                return true;
            }
            if (vk == VK_UP || vk == VK_DOWN)
            {
                m_selected = std::clamp(sel ? m_selected + (vk == VK_UP ? -1 : 1) : 0, 0, n - 1);
                // Keep it in view.
                const float top = static_cast<float>(m_selected) * kJoinRowH;
                if (top < m_listScroll)
                    m_listScroll = top;
                else if (top + kJoinRowH > m_listScroll + m_rcList.h)
                    m_listScroll = top + kJoinRowH - m_rcList.h;
                Rebuild();
                return true;
            }
            if ((vk == VK_DELETE || vk == VK_BACK) && sel && n > 1)
            {
                m_order.erase(m_order.begin() + m_selected);
                m_selected = std::min(m_selected, n - 2);
                Rebuild();
                return true;
            }
            return false;
        }
        if (!f.ready)
            return false;
        const int64_t dur = f.info.DurationUs();
        const int64_t frame = Video() ? FrameDurationUs(Fps()) : 0;
        switch (vk)
        {
        case 'I':
        case 'O':
        {
            const bool start = vk == 'I';
            auto ranges = EditableRanges();
            const int sel = SelectedRange();
            if (sel < 0 || sel >= static_cast<int>(ranges.size()))
                return false;
            // The out point keeps the frame at the playhead.
            const int64_t t = start ? m_playhead : std::min(dur, m_playhead + frame);
            TimeRange r = ranges[static_cast<size_t>(sel)];
            (start ? r.start : r.end) = ClampEdge(ranges, static_cast<size_t>(sel), start, t, dur);
            SetEditableRange(static_cast<size_t>(sel), r);
            Rebuild();
            return true;
        }
        case 'M':
        case VK_INSERT:
            if (tool == EditorTool::SaveFrames || tool == EditorTool::SplitVideo)
            {
                // Marking a frame or a cut goes back to marks.
                if (tool == EditorTool::SplitVideo)
                    m_split = SplitKind::Marks;
                else
                    m_frames.pick = videotools::FramePick::Marks;
                auto marks = f.marks;
                marks.push_back(m_playhead);
                marks = tool == EditorTool::SaveFrames ? videotools::NormalizeFrameTimes(marks, Fps(), dur) : NormalizePoints(marks, dur);
                if (marks.size() == f.marks.size())
                {
                    MessageBeep(MB_OK);   // already marked, or too near an end or another cut
                    return true;
                }
                f.marks = std::move(marks);
                m_selected = static_cast<int>(std::lower_bound(f.marks.begin(), f.marks.end(), SnapToFrame(m_playhead, Fps())) - f.marks.begin());
                Rebuild();
                return true;
            }
            if (tool == EditorTool::BleepAudio)
            {
                TimeRange r;
                if (!FitNewRange(f.ranges, m_playhead, 1'000'000, dur, r))
                {
                    MessageBeep(MB_OK);
                    return true;
                }
                f.ranges.push_back(r);
                f.ranges = NormalizeRanges(f.ranges, dur);
                for (size_t i = 0; i < f.ranges.size(); ++i)
                    if (f.ranges[i].start <= r.start && r.start < f.ranges[i].end)
                        m_selected = static_cast<int>(i);
                Rebuild();
                return true;
            }
            if (tool == EditorTool::RedactVideo)
            {
                const int w = std::max(16, f.info.width / 4), h = std::max(16, f.info.height / 4);
                f.areas.push_back({ ClampCrop({ (f.info.width - w) / 2, (f.info.height - h) / 2, w, h }, f.info.width, f.info.height), { 0, dur }, m_blurStyle });
                m_selected = static_cast<int>(f.areas.size()) - 1;
                Rebuild();
                return true;
            }
            return false;
        case VK_DELETE:
        case VK_BACK:
        {
            const auto drop = [this](auto& list) {
                if (m_selected < 0 || m_selected >= static_cast<int>(list.size()))
                    return false;
                list.erase(list.begin() + m_selected);
                m_selected = list.empty() ? -1 : std::min(m_selected, static_cast<int>(list.size()) - 1);
                Rebuild();
                return true;
            };
            if (MarksOnTrack())
                return drop(f.marks);
            if (tool == EditorTool::BleepAudio)
                return drop(f.ranges);
            if (tool == EditorTool::RedactVideo)
                return drop(f.areas);
            return false;
        }
        case 'A':
            if (tool == EditorTool::BleepAudio || tool == EditorTool::AudioChannels)
            {
                m_listenProcessed = !m_listenProcessed;
                if (m_playing)
                    StartPlay(m_playback->PositionUs(), m_playEnd);
                Rebuild();
                return true;
            }
            return false;
        case VK_UP:
        case VK_DOWN:
        {
            // Through the marked items, the playhead following.
            const int dir = vk == VK_UP ? -1 : 1;
            std::vector<int64_t> starts;
            if (MarksOnTrack())
                starts = f.marks;
            else if (tool == EditorTool::BleepAudio)
                for (const auto& r : f.ranges)
                    starts.push_back(r.start);
            else if (tool == EditorTool::RedactVideo)
                for (const auto& a : f.areas)
                    starts.push_back(a.range.start);
            if (starts.empty())
                return false;
            m_selected = std::clamp(m_selected < 0 ? (dir > 0 ? 0 : static_cast<int>(starts.size()) - 1) : m_selected + dir, 0, static_cast<int>(starts.size()) - 1);
            SetPlayhead(starts[static_cast<size_t>(m_selected)]);
            Rebuild();
            return true;
        }
        default:
            (void)shift;
            return false;
        }
    }

    // ---- Listening --------------------------------------------------------------------------------------------

    Playback::FilterFn EditorWindow::PlayFilter() const
    {
        const auto& f = Cur();
        switch (m_req.tool)
        {
        case EditorTool::SpeedVideo:
        {
            const std::wstring chain = videotools::SpeedAudioFilter(m_factor, m_keepPitch, f.info.sampleRate);
            return [chain](TimeRange) { return Playback::Filter{ chain, false }; };
        }
        case EditorTool::AudioChannels:
        {
            if (!m_listenProcessed)
                return {};
            audiotools::ChannelMix mix;
            mix.mono = m_mono;
            mix.swap = m_swap;
            mix.left = m_left;
            mix.right = m_right;
            const std::wstring pan = audiotools::PanFilter(mix, f.info.channels);
            return [pan](TimeRange) { return Playback::Filter{ pan, false }; };
        }
        case EditorTool::BleepAudio:
        {
            if (!m_listenProcessed || f.ranges.empty())
                return {};
            const auto ranges = f.ranges;
            const auto sound = m_bleep;
            const double duck = m_duckDb;
            const int rate = f.info.sampleRate > 0 ? f.info.sampleRate : 48000;
            return [ranges, sound, duck, rate](TimeRange chunk) {
                return Playback::Filter{ audiotools::BleepGraph(ranges, sound, duck, rate, chunk.start), true };
            };
        }
        default:
            return {};
        }
    }

    double EditorWindow::PlayRate() const { return m_req.tool == EditorTool::SpeedVideo ? m_factor : 1.0; }

    // ---- What goes back to the engine ---------------------------------------------------------------------

    nlohmann::json EditorWindow::SharedParams() const
    {
        nlohmann::json p = nlohmann::json::object();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
            p["mode"] = m_fast && videotools::TrimCopies(true, { m_remove, m_fade }) ? "fast" : "exact";
            p["part"] = m_remove ? "remove" : "keep";
            p["fade"] = m_fade;
            break;
        case EditorTool::CropVideo: p["aspect"] = m_aspect; break;
        case EditorTool::SpeedVideo:
            p["factor"] = m_factor;
            p["pitch"] = m_keepPitch;
            break;
        case EditorTool::JoinVideos: p["fit"] = m_joinFill ? "fill" : "fit"; break;
        case EditorTool::SplitVideo:
            p["mode"] = m_fast ? "fast" : "exact";
            p["parts"] = m_split == SplitKind::Equal ? m_parts : 0;
            p["every"] = m_split == SplitKind::Every ? ToSeconds(m_splitEvery) : 0.0;
            break;
        case EditorTool::SaveFrames:
            p["pick"] = kFramePicks[static_cast<int>(m_frames.pick)];
            p["every"] = m_frames.every;
            p["rate"] = m_frames.perSecond;
            p["format"] = m_frameFormat;
            break;
        case EditorTool::RedactVideo: p["style"] = m_blurStyle ? "blur" : "solid"; break;
        case EditorTool::TrimAudio: p["silence"] = m_silence; break;
        case EditorTool::AudioChannels:
            p["mode"] = m_mono ? "mono" : "stereo";
            p["swap"] = m_swap && !m_mono;
            p["left"] = m_left;
            p["right"] = m_right;
            break;
        case EditorTool::BleepAudio:
            p["tone"] = kBleepSounds[static_cast<int>(m_bleep)];
            p["duck"] = m_duckDb;
            break;
        case EditorTool::AudioVisualizer:
            p["style"] = audiotools::VisualStyleName(m_style);
            p["shape"] = m_shape;
            p["image"] = Narrow(m_image);   // the background of a moving style, or the still picture
            break;
        default: break;
        }
        return p;
    }

    nlohmann::json EditorWindow::FileParams(const FileState& f) const
    {
        nlohmann::json p = nlohmann::json::object();
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
            p["start"] = ToSeconds(f.keep.start);
            p["end"] = ToSeconds(f.keep.end);
            break;
        case EditorTool::CropVideo:
            p["box"] = { f.crop.x, f.crop.y, f.crop.w, f.crop.h };
            p["width"] = f.outW;
            p["height"] = f.outH;
            break;
        case EditorTool::SaveFrames:
            if (m_frames.pick == videotools::FramePick::Marks)
                p["frames"] = SecondsList(f.marks.empty() ? std::vector<int64_t>{ f.playhead } : f.marks);
            break;
        case EditorTool::SplitVideo: p["points"] = SecondsList(m_split == SplitKind::Marks ? f.marks : std::vector<int64_t>{}); break;
        case EditorTool::RedactVideo:
        {
            auto boxes = nlohmann::json::array();
            for (const auto& a : f.areas)
                boxes.push_back({ { "x", a.box.x }, { "y", a.box.y }, { "w", a.box.w }, { "h", a.box.h }, { "start", ToSeconds(a.range.start) },
                                  { "end", ToSeconds(a.range.end) }, { "style", a.blur ? "blur" : "solid" } });
            p["boxes"] = std::move(boxes);
            break;
        }
        case EditorTool::BleepAudio:
        {
            auto ranges = nlohmann::json::array();
            for (const auto& r : f.ranges)
                ranges.push_back({ ToSeconds(r.start), ToSeconds(r.end) });
            p["ranges"] = std::move(ranges);
            break;
        }
        default: break;
        }
        return p;
    }

    void EditorWindow::ChoosePicture()
    {
        const auto paths = PickFiles(Hwnd(), L"Picture for the video", L"Pictures", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff", false);
        if (paths.empty())
            return;
        Frame picture;
        if (!LoadPicture(paths.front(), 1600, picture))
        {
            MessageBoxW(Hwnd(), L"This picture could not be opened.", m_req.title.c_str(), MB_ICONWARNING);
            return;
        }
        m_image = paths.front();
        m_imagePicture = { std::move(picture), nullptr, 0 };
        Rebuild();
    }

    void EditorWindow::AddClips()
    {
        const auto paths = PickFiles(Hwnd(), L"Videos to add", L"Videos", MediaFilePatterns(MediaKind::Video), true);
        if (paths.empty() || !m_loader)
            return;
        for (const auto& path : paths)
        {
            FileState f;
            f.path = path;
            f.name = FileName(path);
            m_files.push_back(std::move(f));
            const int index = static_cast<int>(m_files.size()) - 1;
            m_order.push_back(index);
            m_loader->Probe(index, path);   // its thumbnail follows once it is probed (OnLoaded)
        }
        m_selected = static_cast<int>(m_order.size()) - 1;
        Rebuild();
    }

    std::vector<int64_t> EditorWindow::SplitPoints(const FileState& f) const
    {
        const int64_t dur = f.ready ? f.info.DurationUs() : 0;
        switch (m_split)
        {
        case SplitKind::Equal: return EqualPartPoints(dur, m_parts);
        case SplitKind::Every: return EveryPoints(dur, m_splitEvery);
        default: return f.marks;
        }
    }

    int64_t EditorWindow::FrameCount(const FileState& f) const
    {
        if (!f.ready)
            return 0;
        if (m_frames.pick == videotools::FramePick::Marks)
            return f.marks.empty() ? 1 : static_cast<int64_t>(f.marks.size());
        return videotools::FrameSequenceCount(m_frames, f.info.media.fps, f.info.DurationUs());
    }

    bool EditorWindow::MarksOnTrack() const
    {
        return (m_req.tool == EditorTool::SaveFrames && m_frames.pick == videotools::FramePick::Marks) ||
               (m_req.tool == EditorTool::SplitVideo && m_split == SplitKind::Marks);
    }

    // ---- Tool drawing -------------------------------------------------------------------------------------------

    void EditorWindow::PaintTrackOverlay(ui::Painter& p)
    {
        const auto& f = Cur();
        const RectF& track = m_rcTrack;
        const auto& k = Theme();
        const auto rangeHandles = [&](float x0, float x1, const D2D1_COLOR_F& color, float width) {
            p.Fill({ x0, track.y, width, track.h }, color, 3);
            p.Fill({ x1 - width, track.y, width, track.h }, color, 3);
            const D2D1_COLOR_F grip = k.onAccent;
            for (const float gx : { x0 + width / 2, x1 - width / 2 })
                p.Line(gx, track.y + track.h / 2 - 6, gx, track.y + track.h / 2 + 6, WithAlpha(grip, 0.8f), 1.f);
        };
        switch (m_req.tool)
        {
        case EditorTool::TrimVideo:
        case EditorTool::TrimAudio:
        {
            const float x0 = TX(f.keep.start), x1 = TX(f.keep.end);
            if (m_req.tool == EditorTool::TrimVideo && m_remove)
            {
                // The part that goes: shaded and edged in the warning colour; the rest stays clear.
                p.Fill({ x0, track.y, x1 - x0, track.h }, D2D1::ColorF(0, 0, 0, 0.62f));
                p.Fill({ x0, track.y, x1 - x0, track.h }, WithAlpha(k.danger, 0.28f));
                p.Fill({ x0, track.y, x1 - x0, 2 }, k.danger);
                p.Fill({ x0, track.Bottom() - 2, x1 - x0, 2 }, k.danger);
                rangeHandles(x0, x1, k.danger, 10);
                break;
            }
            const D2D1_COLOR_F shade = D2D1::ColorF(0, 0, 0, Video() ? 0.62f : (k.dark ? 0.35f : 0.12f));
            p.Fill({ track.x, track.y, x0 - track.x, track.h }, shade);
            p.Fill({ x1, track.y, track.Right() - x1, track.h }, shade);
            p.Fill({ x0, track.y, x1 - x0, 2 }, k.accent);
            p.Fill({ x0, track.Bottom() - 2, x1 - x0, 2 }, k.accent);
            rangeHandles(x0, x1, k.accent, 10);
            break;
        }
        case EditorTool::BleepAudio:
            for (size_t i = 0; i < f.ranges.size(); ++i)
            {
                const float x0 = TX(f.ranges[i].start), x1 = TX(f.ranges[i].end);
                const bool sel = static_cast<int>(i) == m_selected;
                p.Fill({ x0, track.y, x1 - x0, track.h }, WithAlpha(k.danger, sel ? 0.30f : 0.18f));
                if (sel)
                {
                    p.Stroke({ x0, track.y, x1 - x0, track.h }, k.danger, 1.5f, 3);
                    rangeHandles(x0, x1, k.danger, 6);
                }
                else
                {
                    p.Line(x0, track.y, x0, track.Bottom(), WithAlpha(k.danger, 0.8f));
                    p.Line(x1, track.y, x1, track.Bottom(), WithAlpha(k.danger, 0.8f));
                }
                if (x1 - x0 > 18)
                    p.Text(std::to_wstring(i + 1), { x0 + (sel ? 8.f : 4.f), track.y + 4, 24, 16 }, ui::Font::Caption, k.text);
            }
            break;
        case EditorTool::RedactVideo:
            for (size_t i = 0; i < f.areas.size() && i < 6; ++i)
            {
                const float by = track.Bottom() - 7 - static_cast<float>(i) * 6;
                p.Fill({ TX(f.areas[i].range.start), by, TX(f.areas[i].range.end) - TX(f.areas[i].range.start), 4 },
                       static_cast<int>(i) == m_selected ? k.accent : WithAlpha(D2D1::ColorF(1, 1, 1, 1), 0.55f), 2);
            }
            if (m_selected >= 0 && m_selected < static_cast<int>(f.areas.size()))
            {
                const auto& r = f.areas[static_cast<size_t>(m_selected)].range;
                const float x0 = TX(r.start), x1 = TX(r.end);
                p.Fill({ x0, track.y, x1 - x0, track.h }, k.accentSoft);
                p.Fill({ x0, track.y, x1 - x0, 2 }, k.accent);
                rangeHandles(x0, x1, k.accent, 8);
            }
            break;
        case EditorTool::SaveFrames:
            if (m_frames.pick != videotools::FramePick::Marks)
            {
                // A tick at each frame saved; where they crowd, every so many of them.
                const int64_t count = FrameCount(f);
                const double fps = f.info.media.fps;
                if (count <= 0)
                    break;
                const double spacing = static_cast<double>(f.info.DurationUs()) / static_cast<double>(count) / std::max(1e-9, m_view.usPerPx);
                const int64_t stride = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(4 / std::max(1e-9, spacing))));
                for (int64_t i = 0; i < count; i += stride)
                {
                    const float x = TX(videotools::FrameSequenceTime(m_frames, fps, i));
                    if (x < track.x - 1)
                        continue;
                    if (x > track.Right() + 1)
                        break;
                    p.Line(x, track.y, x, track.y + track.h * 0.35f, WithAlpha(k.accent, 0.9f), 1.5f);
                }
                break;
            }
            for (size_t i = 0; i < f.marks.size(); ++i)
            {
                const float x = TX(f.marks[i]);
                const bool sel = static_cast<int>(i) == m_selected;
                p.Line(x, track.y, x, track.Bottom(), sel ? k.text : k.accent, 2.f);
                const RectF flag{ x - 9, track.y + 3, 18, 18 };
                p.Fill(flag, sel ? k.text : k.accent, 9);
                p.Text(std::to_wstring(i + 1), flag, ui::Font::Caption, sel ? k.window : k.onAccent, 1);
            }
            break;
        case EditorTool::SplitVideo:
        {
            const bool marks = m_split == SplitKind::Marks;
            const auto points = SplitPoints(f);
            const auto parts = PartsFromPoints(points, f.info.DurationUs());
            for (size_t i = 0; i < parts.size(); ++i)
            {
                const float x0 = TX(parts[i].start), x1 = TX(parts[i].end);
                if (x1 - x0 < 26)
                    continue;
                const RectF chip{ (x0 + x1) / 2 - 11, track.Bottom() - 24, 22, 18 };
                p.Fill(chip, D2D1::ColorF(0, 0, 0, 0.6f), 9);
                p.Text(std::to_wstring(i + 1), chip, ui::Font::Caption, D2D1::ColorF(1, 1, 1, 1), 1);
            }
            for (size_t i = 0; i < points.size(); ++i)
            {
                const float x = TX(points[i]);
                const bool sel = marks && static_cast<int>(i) == m_selected;
                if (!marks)
                    Target()->DrawLine({ x, track.y }, { x, track.Bottom() }, p.Brush(k.text), 2.f, m_dash.Get());
                else
                {
                    p.Line(x, track.y, x, track.Bottom(), sel ? k.accent : k.text, 2.f);
                    const RectF tab{ x - 6, track.y, 12, 10 };
                    p.Fill(tab, sel ? k.accent : k.text, 2);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    void EditorWindow::PaintCropOverlay(ui::Painter& p, const PreviewMap& map)
    {
        const auto& f = Cur();
        const RectF b = map.ToView(f.crop), s = map.shown;
        const D2D1_COLOR_F shade = D2D1::ColorF(0, 0, 0, 0.6f);
        p.Fill({ s.x, s.y, s.w, b.y - s.y }, shade);
        p.Fill({ s.x, b.Bottom(), s.w, s.Bottom() - b.Bottom() }, shade);
        p.Fill({ s.x, b.y, b.x - s.x, b.h }, shade);
        p.Fill({ b.Right(), b.y, s.Right() - b.Right(), b.h }, shade);
        const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1);
        // Rule of thirds while adjusting.
        if (m_drag == Drag::CropHandle || m_drag == Drag::CropNew)
            for (int i = 1; i < 3; ++i)
            {
                p.Line(b.x + b.w * i / 3, b.y, b.x + b.w * i / 3, b.Bottom(), WithAlpha(white, 0.35f));
                p.Line(b.x, b.y + b.h * i / 3, b.Right(), b.y + b.h * i / 3, WithAlpha(white, 0.35f));
            }
        p.Stroke({ b.x - 1, b.y - 1, b.w + 2, b.h + 2 }, white, 1.5f);
        Handles(p, b, true);
        int ow = 0, oh = 0;
        CropOutputSize(f.crop, f.outW, f.outH, ow, oh);
        std::wstring label = std::format(L"{} \u00D7 {}", f.crop.w, f.crop.h);
        if (ow != f.crop.w || oh != f.crop.h)
            label += std::format(L"  \u2192  {} \u00D7 {}", ow, oh);
        const float lw = p.TextWidth(label, ui::Font::Small) + 16;
        float ly = b.Bottom() + 10;
        if (ly + 22 > m_rcStage.Bottom() - 4)
            ly = b.Bottom() - 30;
        const RectF chip{ std::clamp(b.x + b.w / 2 - lw / 2, s.x, std::max(s.x, s.Right() - lw)), ly, lw, 22 };
        p.Fill(chip, D2D1::ColorF(0, 0, 0, 0.72f), 5);
        p.Text(label, chip, ui::Font::Small, white, 1);
    }

    void EditorWindow::PaintAreas(ui::Painter& p, const PreviewMap& map)
    {
        const auto& f = Cur();
        const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1);
        ID2D1Bitmap* blur = BitmapOf(m_blur);
        for (size_t i = 0; i < f.areas.size(); ++i)
        {
            const auto& a = f.areas[i];
            const bool sel = static_cast<int>(i) == m_selected;
            const bool showing = a.range.Contains(m_playhead) || (m_playhead >= Duration() - FrameDurationUs(Fps()) && a.range.end >= Duration());
            const RectF r = map.ToView(a.box);
            if (showing)
            {
                if (!a.blur)
                    p.Fill(r, D2D1::ColorF(0, 0, 0, 1));
                else if (blur && map.frameW > 0)
                {
                    const float sx = static_cast<float>(m_blur.frame.w) / map.frameW, sy = static_cast<float>(m_blur.frame.h) / map.frameH;
                    const D2D1_RECT_F src{ a.box.x * sx, a.box.y * sy, (a.box.x + a.box.w) * sx, (a.box.y + a.box.h) * sy };
                    Target()->DrawBitmap(blur, ui::ToD2D(r), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
                }
                else
                    p.Fill(r, D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.85f));
            }
            if (sel)
            {
                if (showing)
                    p.Stroke({ r.x - 1, r.y - 1, r.w + 2, r.h + 2 }, Theme().accent, 2.f);
                else
                    Target()->DrawRectangle(ui::ToD2D(r), p.Brush(WithAlpha(white, 0.8f)), 1.5f, m_dash.Get());
                Handles(p, r, true);
            }
            else if (showing)
                p.Stroke(r, WithAlpha(white, 0.45f), 1.f);
            if (showing || sel)
            {
                const RectF chip{ r.x + 4, r.y + 4, 20, 18 };
                p.Fill(chip, D2D1::ColorF(0, 0, 0, 0.65f), 4);
                p.Text(std::to_wstring(i + 1), chip, ui::Font::Caption, white, 1);
            }
        }
        if (f.areas.empty() && map.shown.w > 0)
        {
            const RectF hint{ map.shown.x + map.shown.w / 2 - 150, map.shown.Bottom() - 44, 300, 30 };
            p.Fill(hint, D2D1::ColorF(0, 0, 0, 0.6f), 15);
            p.Text(L"Drag on the picture to add an area", hint, ui::Font::Body, white, 1);
        }
    }

    void EditorWindow::PaintVisualizer(ui::Painter& p)
    {
        using audiotools::VisualStyle;
        const auto& first = m_files.front();
        p.Fill(m_rcStage, Theme().stage, 8);
        int vw = 0, vh = 0;
        audiotools::ShapeSize(audiotools::ShapeByName(m_shape), vw, vh);
        const RectF area{ m_rcStage.x + 24, m_rcStage.y + 24, m_rcStage.w - 48, m_rcStage.h - 76 };
        const RectF frame = FitInside(static_cast<float>(vw), static_cast<float>(vh), area);
        p.Fill(frame, D2D1::ColorF(0x0E0F13));
        p.Text(std::format(L"{}  \u00B7  {} \u00D7 {}", first.name, vw, vh), { m_rcStage.x, frame.Bottom() + 12, m_rcStage.w, 22 }, ui::Font::Small,
               Theme().textDim, 1);
        const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.75f);
        ID2D1Bitmap* picture = BitmapOf(m_imagePicture);
        if (m_style == VisualStyle::Picture)
        {
            if (picture)
            {
                const auto size = picture->GetPixelSize();
                const RectF fit = FitInside(static_cast<float>(size.width), static_cast<float>(size.height), frame);
                p.Fill(frame, D2D1::ColorF(0, 0, 0, 1));
                Target()->DrawBitmap(picture, ui::ToD2D(fit), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            else
            {
                p.Text(ui::icon::Music, { frame.x, frame.y + frame.h / 2 - 40, frame.w, 40 }, ui::Font::Icon, white, 1);
                p.Text(first.ready && first.info.coverArt ? L"The song's cover art" : L"Choose a picture, or each song's cover art is used",
                       { frame.x + 12, frame.y + frame.h / 2 + 4, frame.w - 24, 24 }, ui::Font::Body, white, 1);
            }
            return;
        }
        // The background: the picture filling the frame, darkened as in the video.
        if (picture)
        {
            DrawCover(p, picture, frame);
            p.Fill(frame, D2D1::ColorF(0, 0, 0, 0.45f));
        }
        // One frame of the video, drawn from the first song's loudness about a third of the way in
        // (`n` short windows over `seconds`); the spectrum the bars show is only worked out by ffmpeg.
        const bool live = first.peaks && first.peaks->SampleCount() > 0;
        const auto levels = [&](int n, double seconds) {
            std::vector<float> out(static_cast<size_t>(n));
            if (!live)
            {
                for (int c = 0; c < n; ++c)
                {
                    const float t = static_cast<float>(c) / static_cast<float>(n);
                    out[static_cast<size_t>(c)] = (0.15f + 0.35f * std::abs(std::sin(t * 23.f) * std::sin(t * 5.f + 1.f)));
                }
                return out;
            }
            const auto& peaks = *first.peaks;
            const int64_t count = std::max<int64_t>(n, static_cast<int64_t>(peaks.SampleRate() * seconds));
            const int64_t s0 = std::min(peaks.SampleCount() / 3, std::max<int64_t>(0, peaks.SampleCount() - count));
            const float gain = std::clamp(0.9f / std::max(0.01f, peaks.Peak()), 1.f, 6.f);
            for (int c = 0; c < n; ++c)
            {
                const int64_t a = s0 + count * c / n, b = std::max(a + 1, s0 + count * (c + 1) / n);
                const auto [lo, hi] = peaks.Range(a, b);
                out[static_cast<size_t>(c)] = std::sqrt(std::min(1.f, std::max(std::abs(lo), std::abs(hi)) * gain));
            }
            return out;
        };
        const D2D1_COLOR_F blue = D2D1::ColorF(0x7AB8FF);
        const float sc = frame.w / static_cast<float>(vw);
        const float cx = frame.x + frame.w / 2, cy = frame.y + frame.h / 2;
        switch (m_style)
        {
        case VisualStyle::Bars:
        case VisualStyle::Vertical:
        {
            // Bars 20 px apart across five sixths of the width, low notes on the left: rising from
            // the bottom of a band half the height, or mirrored on the middle line.
            const int cols = std::max(16, vw * 5 / 6 / 20);
            const float pitch = 20 * sc, bar = 16 * sc, x0 = cx - static_cast<float>(cols) * pitch / 2;
            const float tall = static_cast<float>(m_style == VisualStyle::Bars ? vh / 2 : vh / 4) * sc;
            const auto v = levels(cols, 0.4);
            for (int c = 0; c < cols; ++c)
            {
                const float h = std::max(1.5f * sc, v[static_cast<size_t>(c)] * (1.f - 0.55f * static_cast<float>(c) / static_cast<float>(cols)) * tall);
                const float bx = x0 + static_cast<float>(c) * pitch;
                if (m_style == VisualStyle::Bars)
                    p.Fill({ bx, cy + tall / 2 - h, bar, h }, blue);
                else
                    p.Fill({ bx, cy - h, bar, 2 * h }, blue);
            }
            break;
        }
        case VisualStyle::Square:
        {
            // The same, in 40 px blocks over two thirds of the height.
            const int cols = std::max(8, vw * 5 / 6 / 40), rows = std::max(6, vh * 2 / 3 / 40);
            const float pitch = 40 * sc, block = 34 * sc;
            const float x0 = cx - static_cast<float>(cols) * pitch / 2, bottom = cy + static_cast<float>(rows) * pitch / 2;
            const auto v = levels(cols, 0.4);
            for (int c = 0; c < cols; ++c)
            {
                const int lit = std::max(1, static_cast<int>(std::lround(v[static_cast<size_t>(c)] * (1.f - 0.55f * static_cast<float>(c) / static_cast<float>(cols)) * rows)));
                for (int r = 0; r < lit && r < rows; ++r)
                    p.Fill({ x0 + static_cast<float>(c) * pitch + (pitch - block), bottom - static_cast<float>(r + 1) * pitch + (pitch - block), block, block }, blue);
            }
            break;
        }
        case VisualStyle::Circle:
        {
            // A ring as wide as the sound is loud, a little uneven where it changes, and the fading
            // trace of an earlier one inside it.
            const int n = 120;
            const auto v = levels(n, 0.25);
            float mean = 0;
            for (const float a : v)
                mean += a / n;
            const float big = static_cast<float>(std::min(vw, vh)) * sc * 0.5f * 0.85f;
            for (const auto& [scale, alpha] : { std::pair{ 0.62f, 0.35f }, std::pair{ 1.f, 1.f } })
            {
                const float r0 = big * (0.25f + 0.75f * mean) * scale;
                float px = 0, py = 0;
                for (int i = 0; i <= n; ++i)
                {
                    const float angle = 6.2831853f * static_cast<float>(i) / n;
                    const float r = r0 * (0.85f + 0.3f * v[static_cast<size_t>(i % n)]);
                    const float qx = cx + r * std::cos(angle), qy = cy + r * std::sin(angle);
                    if (i > 0)
                        p.Line(px, py, qx, qy, WithAlpha(blue, alpha), 1.5f);
                    px = qx;
                    py = qy;
                }
            }
            break;
        }
        default:
        {
            // A frame of the waveform video: about 1/30 s of sound, each column a centred line
            // (showwaves cline, sqrt scale), in the band across the middle.
            const float band = frame.h / 4;
            const int cols = std::max(1, static_cast<int>(frame.w / 2));
            const auto v = levels(cols, 1.0 / 30);
            for (int c = 0; c < cols; ++c)
            {
                const float x = frame.x + (static_cast<float>(c) + 0.5f) * frame.w / static_cast<float>(cols);
                p.Line(x, cy - v[static_cast<size_t>(c)] * band, x, cy + v[static_cast<size_t>(c)] * band, live ? blue : WithAlpha(blue, 0.5f), 1.2f);
            }
            break;
        }
        }
    }

    void EditorWindow::PaintJoinList(ui::Painter& p)
    {
        const RectF& list = m_rcList;
        const auto& k = Theme();
        const float maxScroll = std::max(0.f, static_cast<float>(m_order.size()) * kJoinRowH - list.h);
        m_listScroll = std::clamp(m_listScroll, 0.f, maxScroll);
        Target()->PushAxisAlignedClip(ui::ToD2D(list), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t pos = 0; pos < m_order.size(); ++pos)
        {
            auto& f = m_files[static_cast<size_t>(m_order[pos])];
            const RectF row{ list.x, list.y + static_cast<float>(pos) * kJoinRowH - m_listScroll, list.w, kJoinRowH - 8 };
            if (row.Bottom() < list.y || row.y > list.Bottom())
                continue;
            const bool sel = static_cast<int>(pos) == m_selected;
            p.Fill(row, sel ? k.accentSoft : k.stage, 8);
            if (sel)
                p.Stroke(row, WithAlpha(k.accent, 0.7f), 1.f, 8);
            const RectF num{ row.x + 12, row.y + row.h / 2 - 12, 24, 24 };
            p.Fill(num, sel ? k.accent : k.control, 12);
            p.Text(std::to_wstring(pos + 1), num, ui::Font::Caption, sel ? k.onAccent : k.text, 1);
            const RectF thumb{ row.x + 48, row.y + 7, (row.h - 14) * 16 / 9, row.h - 14 };
            p.Fill(thumb, D2D1::ColorF(0, 0, 0, 1), 4);
            if (ID2D1Bitmap* bmp = BitmapOf(f.joinThumb))
            {
                const auto size = bmp->GetPixelSize();
                const RectF fit = FitInside(static_cast<float>(size.width), static_cast<float>(size.height), thumb);
                Target()->DrawBitmap(bmp, ui::ToD2D(fit), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            const float tx = thumb.Right() + 16;
            p.Text(f.name, { tx, row.y + 12, row.Right() - tx - 16, 22 }, ui::Font::BodyStrong, k.text);
            std::wstring detail;
            if (!f.error.empty())
                detail = f.error;
            else if (!f.ready)
                detail = L"Checking\u2026";
            else
            {
                detail = std::format(L"{} \u00D7 {}  \u00B7  {} fps  \u00B7  {}", f.info.width, f.info.height, Num(std::round(f.info.media.fps * 100) / 100),
                                     FormatTimecode(f.info.DurationUs()));
                if (!f.info.HasAudio())
                    detail += L"  \u00B7  no sound";
                if (pos == 0)
                    detail += L"  \u00B7  sets the size";
            }
            p.Text(detail, { tx, row.y + 36, row.Right() - tx - 16, 20 }, ui::Font::Small, f.error.empty() ? k.textDim : k.danger);
        }
        if (m_drag == Drag::JoinRow && m_joinDropAt >= 0)
        {
            const float y = list.y + static_cast<float>(m_joinDropAt) * kJoinRowH - m_listScroll - 4;
            p.Fill({ list.x, y - 1.5f, list.w, 3 }, k.accent, 1.5f);
        }
        Target()->PopAxisAlignedClip();
    }
}
