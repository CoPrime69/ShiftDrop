#include "SettingsWindow.h"
#include "Brand.h"
#include "Log.h"
#include "OutputPath.h"
#include "PanelLayout.h"
#include "Strings.h"
#include "Theme.h"
#include "resource.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Settings";

        // Control ids. A chip's is kIdChip + its place in m_groups' chips, a group's header kIdGroup + its place.
        constexpr int kIdFind = 100, kIdNav = 110;
        constexpr int kIdStartup = 120, kIdOcr = 121, kIdShelf = 122, kIdDrops = 123, kIdTerminal = 126, kIdCommandLine = 127;
        constexpr int kIdTheme = 130, kIdStyle = 133, kIdSize = 135, kIdPanelSize = 139;
        constexpr int kIdAccentHeader = 140, kIdSwatch = 141, kIdCustom = 141 + accent::kPresetCount, kIdWindowsAccent = 150;
        constexpr int kIdFindAction = 160, kIdTab = 161, kIdSectionOff = 166, kIdReset = 167;
        constexpr int kIdNaming = 170, kIdShellMenu = 180, kIdRecipes = 181, kIdLogs = 182, kIdSource = 183;
        constexpr int kIdGroup = 1000, kIdChip = 2000;

        // Sizes in DIPs (App Windows.dc.html), scaled by the window's DPI in Layout().
        constexpr int kNavWide = 290, kNavNarrow = 240;   // the navigation column, with its 14 DIPs either side
        constexpr int kNavItem = 38, kNavGap = 4, kCardGap = 4, kRowMin = 68, kControlH = 32, kSegmentH = 36;
        constexpr int kSwatchCell = 54, kSwatchH = 62, kPreviewH = 250, kWheelPreview = 210;

        constexpr wchar_t kGlyphOpen = 0xE8A7, kGlyphPower = 0xE7E8, kGlyphCheck = 0xE73E, kGlyphPalette = 0xE790, kGlyphAdd = 0xE710;
        constexpr wchar_t kSourceUrl[] = L"https://github.com/CoPrime69/" SD_NAME;

        // Choices, in the order of the enums in Settings.h and OutputPath.h.
        const std::wstring kTerminals[] = { L"Windows Terminal", L"PowerShell", L"Command Prompt", L"Custom command" };
        constexpr const wchar_t* kThemes[] = { L"System", L"Light", L"Dark" };
        constexpr const wchar_t* kStyles[] = { L"Solid", L"Glass" };
        constexpr const wchar_t* kSizes[] = { L"Auto", L"Small", L"Medium", L"Large" };
        constexpr const wchar_t* kDrops[] = { L"Desktop", L"Downloads", L"Ask every time" };
        constexpr const wchar_t* kNamingDetails[] = { L"Default", L"", L"", L"Copies go in a " SD_NAME L" folder next to the original" };

        // The glyph of a kind of file's card on the Wheel page.
        wchar_t GroupIcon(std::wstring_view title)
        {
            static constexpr std::pair<const wchar_t*, wchar_t> kIcons[] = {
                { L"Photos", 0xEB9F }, { L"Video", 0xE714 },      { L"Audio", 0xE8D6 },        { L"Video and audio", 0xE714 },
                { L"PDF", 0xE8A5 },    { L"Documents", 0xE8A5 }, { L"Spreadsheets", 0xE80A }, { L"Subtitles", 0xE7F0 },
                { L"Archives", 0xE7B8 }, { L"Any file", 0xE8B7 },
            };
            for (const auto& [name, glyph] : kIcons)
                if (title == name)
                    return glyph;
            return 0xE9F5;   // Processing
        }

        // The colour button's name for screen readers: "Custom colour #12A58E…".
        std::wstring CustomText(const accent::Setting& a)
        {
            if (a.custom == accent::kNoColour)
                return L"Custom colour\u2026";
            return std::format(L"Custom colour {}{}\u2026", accent::ToHex(a.custom), a.choice == accent::Choice::Custom ? L" (in use)" : L"");
        }

        std::wstring PercentText(int percent) { return std::format(L"{}%", percent); }

        ControlKit::Part PartOf(size_t i, size_t count)
        {
            using P = ControlKit::Part;
            return count == 1 ? P::Only : i == 0 ? P::First : i + 1 == count ? P::Last : P::Middle;
        }
    }

    SettingsWindow::SettingsWindow(HINSTANCE instance, FeaturesChange onFeatures, PreferencesChange onPreferences, Command command)
        : DialogWindow(instance, { .className = kClass, .width = 1000, .height = 720, .minWidth = 600, .minHeight = 440,
                                   .lightBackground = COLOR_BTNFACE, .scrolls = true, .kitFont = true }),
          m_onFeatures(std::move(onFeatures)), m_onPreferences(std::move(onPreferences)), m_command(std::move(command))
    {
    }

    SettingsWindow::~SettingsWindow()
    {
        if (m_icon)
            DestroyIcon(m_icon);
    }

    void SettingsWindow::Show(SettingsTree tree, SettingsState state)
    {
        m_tree = std::move(tree);
        m_state = std::move(state);
        if (!m_hwnd && !Create(L"" SD_NAME L" Settings"))
            return;
        BuildWheel();
        Fill();
        Layout();
        Present(m_nav[static_cast<size_t>(m_page)]);   // allowed in front: we are handling the user's tray-menu click
    }

    // ---- Controls ---------------------------------------------------------------------------------------

    SettingsWindow::Row& SettingsWindow::MakeRow(Item item, HWND control)
    {
        const auto& entry = settings::Find(item);
        Row& row = m_rows[item];
        row.title = Label(entry.title, TextRole::Title);
        row.description = entry.description ? Label(entry.description, TextRole::Caption) : nullptr;
        row.control = control;
        m_all.push_back(row.title);
        if (row.description)
            m_all.push_back(row.description);
        return row;
    }

    void SettingsWindow::CreateChildren()
    {
        // Created in reading order, which is the Tab order: the navigation, then each page's cards.
        const auto keep = [this](HWND h) {
            m_all.push_back(h);
            return h;
        };
        m_appName = keep(Label(SD_NAME, TextRole::AppName, Surface::Window));
        m_version = keep(Label(L"Version " SD_VERSION, TextRole::Caption, Surface::Window));
        m_find = keep(TextField(kIdFind, 0, L"Find a setting", true, Surface::Window));
        Describe(m_find, L"Find a setting");
        m_findHint = keep(Label(L"", TextRole::Hint, Surface::Window));
        for (int i = 0; i < settings::kPageCount; ++i)
        {
            const auto page = static_cast<Page>(i);
            m_nav[static_cast<size_t>(i)] = keep(KitButton(Look::Nav, settings::PageTitle(page), kIdNav + i, Surface::Window, settings::PageIcon(page), i == 0));
        }
        m_heading = keep(Label(L"", TextRole::Heading, Surface::Window));

        const auto toggle = [&](Item item, int id) {
            const auto& entry = settings::Find(item);
            const HWND h = keep(KitButton(Look::Toggle, entry.title, id));
            MakeRow(item, h);
            if (entry.description)
                Describe(h, entry.title, entry.description);
            return h;
        };
        const auto segments = [&](Item item, auto& buttons, const auto& texts, int id) {
            MakeRow(item, nullptr);
            for (size_t i = 0; i < buttons.size(); ++i)
            {
                buttons[i] = keep(KitButton(Look::Segment, texts[i], id + static_cast<int>(i), Surface::Card, 0, i == 0));
                StyleOf(buttons[i]).part = PartOf(i, buttons.size());
                // "Theme: Dark", so a screen reader says which choice the option belongs to.
                Describe(buttons[i], std::format(L"{}: {}", settings::Find(item).title, texts[i]));
            }
            m_rows[item].control = buttons[0];
        };

        // General
        m_filesGroup = keep(Label(L"Files", TextRole::Group, Surface::Window));
        m_startup = toggle(Item::StartWithWindows, kIdStartup);
        m_ocr = keep(DropDown(kIdOcr, std::span<const std::wstring>{}));
        MakeRow(Item::OcrLanguage, m_ocr);
        Describe(m_ocr, settings::Find(Item::OcrLanguage).title, settings::Find(Item::OcrLanguage).description);
        m_shelf = toggle(Item::FloatShelf, kIdShelf);
        segments(Item::BrowserDrops, m_drops, kDrops, kIdDrops);
        m_terminal = keep(DropDown(kIdTerminal, kTerminals));
        MakeRow(Item::Terminal, m_terminal);
        Describe(m_terminal, settings::Find(Item::Terminal).title, settings::Find(Item::Terminal).description);
        m_commandLine = keep(TextField(kIdCommandLine, 0, L"e.g. wezterm start --cwd {folder}"));
        SendMessageW(m_commandLine, EM_LIMITTEXT, 1024, 0);
        MakeRow(Item::TerminalCommand, m_commandLine);
        Describe(m_commandLine, settings::Find(Item::TerminalCommand).title, settings::Find(Item::TerminalCommand).description);

        // Appearance
        m_previewLabel = keep(Label(L"Preview", TextRole::Caption, Surface::Window));
        segments(Item::Theme, m_themes, kThemes, kIdTheme);
        segments(Item::Style, m_looks, kStyles, kIdStyle);
        segments(Item::Size, m_sizes, kSizes, kIdSize);
        m_panelSize = keep(Slider(kIdPanelSize, panel::kSizeMinPercent, panel::kSizeMaxPercent, panel::kSizeStepPercent));
        MakeRow(Item::PanelSize, m_panelSize);
        Describe(m_panelSize, settings::Find(Item::PanelSize).title, settings::Find(Item::PanelSize).description);
        m_panelValue = keep(Label(L"", TextRole::Value));
        m_panelLow = keep(Label(PercentText(panel::kSizeMinPercent).c_str(), TextRole::Small));
        m_panelHigh = keep(Label(PercentText(panel::kSizeMaxPercent).c_str(), TextRole::Small));
        const auto& accentEntry = settings::Find(Item::Accent);
        m_accentHeader = keep(KitButton(Look::Expander, accentEntry.title, kIdAccentHeader, Surface::Card, accentEntry.icon));
        StyleOf(m_accentHeader).detail = accentEntry.description;
        Describe(m_accentHeader, accentEntry.title, accentEntry.description);
        // The swatches are a radio group (arrows move between them, Tab lands on the chosen one); the
        // colour button and the switch end it.
        for (int i = 0; i < accent::kPresetCount; ++i)
            m_swatches[static_cast<size_t>(i)] = keep(KitButton(Look::Custom, accent::Name(static_cast<accent::Choice>(i)), kIdSwatch + i, Surface::Card, 0, i == 0));
        m_custom = keep(Control(WC_BUTTONW, L"", WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, kIdCustom));
        StyleOf(m_custom) = DialogWindow::Style{ .look = Look::Custom };
        m_windowsAccent = keep(KitButton(Look::Toggle, settings::Find(Item::WindowsAccent).title, kIdWindowsAccent));
        MakeRow(Item::WindowsAccent, m_windowsAccent);
        m_rows[Item::Accent].control = m_accentHeader;

        // Wheel: the search, the section tabs and the section's buttons; the cards come with the tree.
        m_findAction = keep(TextField(kIdFindAction, 0, L"Find an action, such as WebP or Copy path", true, Surface::Window));
        Describe(m_findAction, L"Find an action");
        for (int i = 0; i < kRingCount; ++i)
            m_tabs[static_cast<size_t>(i)] = keep(KitButton(Look::Tab, RingTitle(static_cast<Ring>(i)), kIdTab + i, Surface::Window, 0, i == 0));
        m_summary = keep(Label(L"", TextRole::Body, Surface::Window));
        m_sectionOff = keep(KitButton(Look::Button, L"Turn off this section", kIdSectionOff, Surface::Window, kGlyphPower));
        m_reset = keep(KitButton(Look::Button, L"Reset to default", kIdReset, Surface::Window));
        Describe(m_reset, L"Reset to default", L"Turns the section and every action in it on");
        m_noAction = keep(Label(L"", TextRole::Hint, Surface::Window));
        m_rows[Item::WheelContents].control = m_findAction;

        // Output
        MakeRow(Item::OutputNaming, nullptr);
        m_example = keep(Label(L"", TextRole::Strong));
        m_exampleNote = keep(Label(L"Live example", TextRole::Caption));
        for (size_t i = 0; i < m_namings.size(); ++i)
        {
            const auto naming = static_cast<OutputNaming>(i);
            const std::wstring text = naming == OutputNaming::Subfolder ? std::wstring(L"" SD_NAME L" folder") : OutputName(naming, L"photo", L"converted", 1, L".jpg");
            m_namings[i] = keep(KitButton(Look::Radio, text.c_str(), kIdNaming + static_cast<int>(i), Surface::Card, 0, i == 0));
            StyleOf(m_namings[i]).detail = kNamingDetails[i];
        }
        m_rows[Item::OutputNaming].control = m_namings[0];

        // Advanced
        m_shellMenu = toggle(Item::ShellMenu, kIdShellMenu);
        m_recipes = keep(KitButton(Look::Button, L"Open recipes folder", kIdRecipes, Surface::Card, kGlyphOpen));
        MakeRow(Item::RecipesFolder, m_recipes);
        m_logs = keep(KitButton(Look::Button, L"Open logs folder", kIdLogs, Surface::Card, kGlyphOpen));
        MakeRow(Item::LogsFolder, m_logs);

        // About
        m_aboutName = keep(Label(SD_NAME, TextRole::Heading));
        m_aboutVersion = keep(Label(L"Version " SD_VERSION, TextRole::Value));
        m_aboutFree = keep(Label(L"Free and open source", TextRole::Strong));
        m_source = keep(KitButton(Look::Icon, L"Open the source code on GitHub", kIdSource, Surface::Card, kGlyphOpen));
        MakeRow(Item::SourceCode, m_source);
        MakeRow(Item::Privacy, nullptr);
        CheckOne(m_nav, 0);
    }

    void SettingsWindow::BuildWheel()
    {
        // Again for each Show(): the recipes may have changed since.
        for (auto& g : m_groups)
        {
            for (HWND h : { g.header, g.hint, g.preview })
            {
                std::erase(m_all, h);
                DestroyControl(h);
            }
            for (auto& chip : g.chips)
            {
                std::erase(m_all, chip.button);
                DestroyControl(chip.button);
            }
        }
        m_groups.clear();
        int chipId = kIdChip;
        for (const auto& section : m_tree.sections)
            for (const auto& group : section.groups)
            {
                if (group.slices.empty())
                    continue;
                GroupView view;
                view.ring = section.ring;
                view.group = &group;
                view.open = m_groups.empty() || view.ring != m_groups.back().ring;   // each section's first card opens
                view.header = KitButton(Look::Expander, group.title.c_str(), kIdGroup + static_cast<int>(m_groups.size()), Surface::Card, GroupIcon(group.title));
                view.hint = Label(L"Turn an action on or off. The wheel shows exactly these tiles.", TextRole::Caption);
                bool first = true;
                for (const auto& slice : group.slices)
                {
                    const HWND chip = KitButton(Look::Chip, slice.label.c_str(), chipId++, Surface::Card, 0, first);
                    first = false;
                    if (!slice.detail.empty() && slice.detail != slice.label)
                        Describe(chip, slice.label, slice.detail);
                    view.chips.push_back({ chip, &slice });
                    m_all.push_back(chip);
                }
                view.preview = Label(L"", TextRole::Centred);
                for (HWND h : { view.header, view.hint, view.preview })
                {
                    m_all.push_back(h);
                    SendMessageW(h, WM_SETFONT, SendMessageW(m_find, WM_GETFONT, 0, 0), FALSE);
                }
                m_groups.push_back(std::move(view));
            }
    }

    void SettingsWindow::OnDpiChanged()
    {
        // The app's icon at its size for this DPI, for the header and About (GDI picks the best image).
        const int px = Px(56);
        if (px != m_iconPx)
        {
            if (m_icon)
                DestroyIcon(m_icon);
            m_icon = nullptr;
            LoadIconWithScaleDown(m_instance, MAKEINTRESOURCEW(IDI_APPICON), px, px, &m_icon);
            m_iconPx = px;
        }
    }

    // ---- Filling from the state -------------------------------------------------------------------------

    void SettingsWindow::Fill()
    {
        m_updating = true;
        const auto& p = m_state.preferences;
        Button_SetCheck(m_startup, m_state.startWithWindows ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(m_shelf, m_state.shelfOn ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(m_shelf, m_state.shelfAvailable);
        Button_SetCheck(m_shellMenu, m_state.shellMenuOn ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(m_shellMenu, m_state.shellMenuAvailable);
        // Automatic, then the languages Windows has.
        SendMessageW(m_ocr, CB_RESETCONTENT, 0, 0);
        SendMessageW(m_ocr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
        for (const auto& name : m_state.ocrLanguages)
            SendMessageW(m_ocr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(m_ocr, CB_SETCURSEL, static_cast<WPARAM>(std::clamp(m_state.ocrChoice, 0, static_cast<int>(m_state.ocrLanguages.size()))), 0);
        EnableWindow(m_ocr, !m_state.ocrLanguages.empty());
        CheckOne(m_drops, static_cast<int>(m_state.browserDrops));
        SendMessageW(m_terminal, CB_SETCURSEL, static_cast<WPARAM>(p.terminal), 0);
        SetWindowTextW(m_commandLine, p.terminalCommand.c_str());
        EnableWindow(m_commandLine, p.terminal == TerminalChoice::Custom);
        CheckOne(m_themes, static_cast<int>(p.theme));
        CheckOne(m_looks, static_cast<int>(m_state.appearance.style));
        CheckOne(m_sizes, static_cast<int>(m_state.appearance.size));
        SendMessageW(m_panelSize, TBM_SETPOS, TRUE, panel::SizePercent(p.panelSize));
        SetWindowTextW(m_panelValue, PercentText(panel::SizePercent(p.panelSize)).c_str());
        FillAccent();
        FillNaming();
        FillWheel();
        m_updating = false;
    }

    void SettingsWindow::FillAccent()
    {
        const auto& a = m_state.preferences.accent;
        const int checked = static_cast<int>(a.choice) < accent::kPresetCount ? static_cast<int>(a.choice) : -1;
        CheckOne(m_swatches, checked);
        Button_SetCheck(m_windowsAccent, a.choice == accent::Choice::Windows ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(m_custom, CustomText(a).c_str());
        auto& header = StyleOf(m_accentHeader);
        header.open = m_accentOpen;
        header.trailing = m_accentOpen ? L"" : accent::Name(a.choice);
        for (HWND h : { m_custom, m_windowsAccent, m_accentHeader })
            Redraw(h);
    }

    void SettingsWindow::FillNaming()
    {
        const auto naming = m_state.preferences.naming;
        CheckOne(m_namings, static_cast<int>(naming));
        const std::wstring out = OutputName(naming, L"photo", L"converted", 1, L".jpg");
        const std::wstring text = naming == OutputNaming::Subfolder ? std::format(L"photo.jpg  \u2192  {}\\{}", kOutputSubfolder, out)
                                                                    : std::format(L"photo.jpg  \u2192  {}", out);
        SetWindowTextW(m_example, text.c_str());
    }

    void SettingsWindow::FillWheel()
    {
        const auto& f = m_state.features;
        CheckOne(m_tabs, static_cast<int>(m_section));
        const auto* section = m_tree.Find(m_section);
        const bool on = f.SectionOn(m_section);
        SetWindowTextW(m_summary, section ? contents::Summary(f, *section).c_str() : L"");
        SetWindowTextW(m_sectionOff, on ? L"Turn off this section" : L"Turn on this section");
        // The wheel needs a section: the last one on can't be turned off.
        EnableWindow(m_sectionOff, contents::CanTurnOff(f, m_section));
        Describe(m_sectionOff, on ? L"Turn off this section" : L"Turn on this section",
                 contents::CanTurnOff(f, m_section) ? L"" : L"At least one section stays on, or the wheel would have nothing to show");
        for (auto& g : m_groups)
        {
            const auto count = contents::CountOn(f, *g.group);
            auto& header = StyleOf(g.header);
            header.open = g.open;
            header.detail = g.open ? contents::CountText(count) : contents::Listing(*g.group);
            header.trailing = g.open ? L"" : contents::CountText(count);
            Describe(g.header, g.group->title, contents::CountText(count));
            Redraw(g.header);
            const bool sectionOn = f.SectionOn(g.ring);
            for (const auto& chip : g.chips)
            {
                Button_SetCheck(chip.button, contents::SliceOn(f, chip.slice->id) ? BST_CHECKED : BST_UNCHECKED);
                EnableWindow(chip.button, sectionOn);
            }
            const auto tiles = contents::PreviewSlices(f, *g.group);
            SetWindowTextW(g.preview, std::format(L"Preview \u00B7 {} tile{}", tiles.size(), tiles.size() == 1 ? L"" : L"s").c_str());
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // ---- Changes ------------------------------------------------------------------------------------------

    void SettingsWindow::SendCommand(UINT id)
    {
        if (m_command)
            m_command(id);
    }

    void SettingsWindow::ChangePreferences(const Preferences& p)
    {
        if (p == m_state.preferences)
            return;
        const auto& before = m_state.preferences;
        const bool themeChanged = p.theme != before.theme;
        if (p.terminal != before.terminal || themeChanged || p.naming != before.naming || p.panelSize != before.panelSize)
            log::Info(L"settings: terminal {}, theme {}, output names {}, top panel size {}%", kTerminals[static_cast<int>(p.terminal)],
                      kThemes[static_cast<int>(p.theme)], static_cast<int>(p.naming), p.panelSize);
        m_state.preferences = p;
        if (m_onPreferences)
            m_onPreferences(m_state.preferences);
        if (themeChanged)
            ApplyTheme();   // the callback has set the new theme
    }

    void SettingsWindow::SetAccent(const accent::Setting& a)
    {
        if (a == m_state.preferences.accent)
        {
            FillAccent();
            return;
        }
        log::Info(L"settings: accent colour {}{}", accent::Name(a.choice), a.choice == accent::Choice::Custom ? L" " + accent::ToHex(a.custom) : L"");
        m_state.preferences.accent = a;
        if (m_onPreferences)
            m_onPreferences(m_state.preferences);
        FillAccent();
        ApplyTheme();   // the callback has set the new accent: every control redraws with it
    }

    void SettingsWindow::OnPanelSizeScroll(WORD code)
    {
        // The value beside it follows the thumb; the size is kept (and saved) when the thumb lands, on a
        // 10% step.
        const int percent = panel::SizePercent(static_cast<int>(SendMessageW(m_panelSize, TBM_GETPOS, 0, 0)));
        SetWindowTextW(m_panelValue, PercentText(percent).c_str());
        if (code == TB_THUMBTRACK)
            return;
        if (SendMessageW(m_panelSize, TBM_GETPOS, 0, 0) != percent)
            SendMessageW(m_panelSize, TBM_SETPOS, TRUE, percent);
        auto p = m_state.preferences;
        p.panelSize = percent;
        ChangePreferences(p);
    }

    void SettingsWindow::SetFeatures(const Features& features)
    {
        if (features == m_state.features)
        {
            FillWheel();
            return;
        }
        m_state.features = features;
        log::Info(L"settings: {} section(s) on, {} slice(s) off", features.SectionsOn(), features.off.size());
        if (m_onFeatures)
            m_onFeatures(m_state.features);
        FillWheel();
    }

    void SettingsWindow::ShowPage(Page page)
    {
        m_page = page;
        CheckOne(m_nav, static_cast<int>(page));
        SetWindowTextW(m_heading, page == Page::Wheel ? settings::Find(Item::WheelContents).title : settings::PageTitle(page));
        ScrollTo(0);
        Layout();
    }

    void SettingsWindow::OnFind()
    {
        const std::wstring query = Text(m_find);
        const auto found = settings::Search(query);
        m_showFound = !found.empty();
        if (Trim(query).empty())
            SetWindowTextW(m_findHint, L"");
        else if (found.empty())
            SetWindowTextW(m_findHint, std::format(L"No setting matches \u201C{}\u201D.", query).c_str());
        else
            SetWindowTextW(m_findHint, found.size() == 1 ? L"Enter goes to it." : std::format(L"{} settings match. Enter goes to the first.", found.size()).c_str());
        if (!found.empty())
        {
            m_found = found.front();
            const auto page = settings::Find(m_found).page;
            if (page != m_page)
                ShowPage(page);
            else
                Layout();
            ScrollIntoView(m_rows[m_found].card);
        }
        else
            Layout();
    }

    void SettingsWindow::OnCommand(int id, int code, HWND control)
    {
        const bool clicked = code == BN_CLICKED;
        auto p = m_state.preferences;
        if (id >= kIdNav && id < kIdNav + settings::kPageCount && clicked)
        {
            if (Checked(control) && static_cast<int>(m_page) != id - kIdNav)
                ShowPage(static_cast<Page>(id - kIdNav));
        }
        else if (id == kIdFind && code == EN_CHANGE)
            OnFind();
        else if (id == kIdStartup && clicked)
        {
            if (Checked(m_startup) != m_state.startWithWindows)
            {
                m_state.startWithWindows = Checked(m_startup);
                SendCommand(IDM_START_WITH_WINDOWS);
            }
        }
        else if (id == kIdShelf && clicked)
        {
            if (Checked(m_shelf) != m_state.shelfOn)
            {
                m_state.shelfOn = Checked(m_shelf);
                SendCommand(IDM_SHOW_SHELF);
            }
        }
        else if (id == kIdShellMenu && clicked)
        {
            if (Checked(m_shellMenu) != m_state.shellMenuOn)
            {
                m_state.shellMenuOn = Checked(m_shellMenu);
                SendCommand(IDM_SHELL_MENU);
            }
        }
        else if (id == kIdOcr && code == CBN_SELCHANGE)
        {
            const int i = static_cast<int>(SendMessageW(m_ocr, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i != m_state.ocrChoice && IDM_OCR_BASE + i <= IDM_OCR_LAST)
            {
                m_state.ocrChoice = i;
                SendCommand(IDM_OCR_BASE + static_cast<UINT>(i));
            }
        }
        else if (id >= kIdDrops && id < kIdDrops + 3 && clicked && Checked(control))
        {
            const auto choice = static_cast<VirtualOutput>(id - kIdDrops);
            CheckOne(m_drops, id - kIdDrops);
            if (choice != m_state.browserDrops)
            {
                m_state.browserDrops = choice;
                SendCommand(IDM_VIRTUAL_BASE + static_cast<UINT>(choice));
            }
        }
        else if ((id == kIdTerminal && code == CBN_SELCHANGE) || (id == kIdCommandLine && code == EN_CHANGE))
        {
            const int i = static_cast<int>(SendMessageW(m_terminal, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < static_cast<int>(std::size(kTerminals)))
                p.terminal = static_cast<TerminalChoice>(i);
            p.terminalCommand = Text(m_commandLine);
            EnableWindow(m_commandLine, p.terminal == TerminalChoice::Custom);
            InvalidateRect(m_hwnd, nullptr, FALSE);   // the command line's box dims with it
            ChangePreferences(p);
        }
        else if (id >= kIdTheme && id < kIdTheme + 3 && clicked && Checked(control))
        {
            CheckOne(m_themes, id - kIdTheme);
            p.theme = static_cast<ThemeChoice>(id - kIdTheme);
            ChangePreferences(p);
        }
        else if (id >= kIdStyle && id < kIdStyle + 2 && clicked && Checked(control))
        {
            CheckOne(m_looks, id - kIdStyle);
            const auto style = static_cast<sd::Style>(id - kIdStyle);
            if (style != m_state.appearance.style)
            {
                m_state.appearance.style = style;
                SendCommand(IDM_STYLE_BASE + static_cast<UINT>(style));
                InvalidateRect(m_hwnd, nullptr, FALSE);   // the previews
            }
        }
        else if (id >= kIdSize && id < kIdSize + 4 && clicked && Checked(control))
        {
            CheckOne(m_sizes, id - kIdSize);
            const auto size = static_cast<WheelSize>(id - kIdSize);
            if (size != m_state.appearance.size)
            {
                m_state.appearance.size = size;
                SendCommand(IDM_SIZE_BASE + static_cast<UINT>(size));
            }
        }
        else if (id == kIdAccentHeader && clicked)
        {
            m_accentOpen = !m_accentOpen;
            FillAccent();
            Layout();
        }
        else if (id >= kIdSwatch && id < kIdSwatch + accent::kPresetCount && clicked && Checked(control))
        {
            auto a = p.accent;
            a.choice = static_cast<accent::Choice>(id - kIdSwatch);
            SetAccent(a);
        }
        else if (id == kIdCustom && clicked)
        {
            // The colour dialog opens on the last colour of your own, or on the accent in use.
            auto a = p.accent;
            uint32_t rgb = a.custom != accent::kNoColour ? a.custom : accent::Current().fill;
            if (PickColour(m_hwnd, rgb))
            {
                a.custom = rgb;
                a.choice = accent::Choice::Custom;
                SetAccent(a);
            }
        }
        else if (id == kIdWindowsAccent && clicked)
        {
            auto a = p.accent;
            if (Checked(m_windowsAccent))
            {
                if (a.choice != accent::Choice::Windows)
                    m_beforeWindows = a.choice;
                a.choice = accent::Choice::Windows;
            }
            else
                a.choice = m_beforeWindows;
            SetAccent(a);
        }
        else if (id == kIdFindAction && code == EN_CHANGE)
        {
            ScrollTo(0);
            Layout();
        }
        else if (id >= kIdTab && id < kIdTab + kRingCount && clicked && Checked(control))
        {
            m_section = static_cast<Ring>(id - kIdTab);
            FillWheel();
            ScrollTo(0);
            Layout();
        }
        else if (id == kIdSectionOff && clicked)
            SetFeatures(contents::WithSection(m_state.features, m_section, !m_state.features.SectionOn(m_section)));
        else if (id == kIdReset && clicked)
        {
            if (const auto* section = m_tree.Find(m_section))
                SetFeatures(contents::Reset(m_state.features, *section));
        }
        else if (id >= kIdNaming && id < kIdNaming + 4 && clicked && Checked(control))
        {
            p.naming = static_cast<OutputNaming>(id - kIdNaming);
            ChangePreferences(p);
            FillNaming();
        }
        else if (id == kIdRecipes && clicked)
            SendCommand(IDM_OPEN_RECIPES);
        else if (id == kIdLogs && clicked)
            SendCommand(IDM_OPEN_LOGS);
        else if (id == kIdSource && clicked)
            ShellExecuteW(m_hwnd, L"open", kSourceUrl, nullptr, nullptr, SW_SHOWNORMAL);
        else if (id >= kIdGroup && id < kIdChip && clicked)
        {
            if (const auto i = static_cast<size_t>(id - kIdGroup); i < m_groups.size())
            {
                m_groups[i].open = !m_groups[i].open;
                FillWheel();
                Layout();
            }
        }
        else if (id >= kIdChip && clicked)
        {
            for (const auto& g : m_groups)
                for (const auto& chip : g.chips)
                    if (chip.button == control)
                        SetFeatures(contents::WithSlice(m_state.features, chip.slice->id, Checked(control)));
        }
    }

    // ---- Layout -------------------------------------------------------------------------------------------

    void SettingsWindow::Place(HWND control, int x, int y, int w, int h)
    {
        if (!control)
            return;
        SetWindowPos(control, nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
        m_placed.push_back(control);
    }

    int SettingsWindow::LayoutRow(Row& row, int x, int y, int width, int controlW, int controlH)
    {
        // Glyph, then the title over its description, then the control at the end (App Windows: 68 DIPs
        // at least, 12 above and below, 18 before the glyph and 14 after the control).
        const int textX = x + Px(54), textRight = x + width - Px(14) - (controlW ? controlW + Px(16) : 0);
        const int textW = std::max(Px(60), textRight - textX);
        const int titleH = LabelHeight(row.title, textW), descH = row.description ? LabelHeight(row.description, textW) : 0;
        const int textH = titleH + (descH ? Px(2) + descH : 0);
        const int h = std::max(Px(kRowMin), std::max(textH, controlH) + Px(24));
        Place(row.title, textX, y + (h - textH) / 2, textW, titleH);
        if (row.description)
            Place(row.description, textX, y + (h - textH) / 2 + titleH + Px(2), textW, descH);
        if (row.control && controlW)
            Place(row.control, x + width - Px(14) - controlW, y + (h - controlH) / 2, controlW, controlH);
        row.card = { x, y, x + width, y + h };
        return y + h;
    }

    void SettingsWindow::Layout()
    {
        if (!m_heading)
            return;
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        m_placed.clear();
        m_cards.clear();
        m_lines.clear();
        m_preview = m_exampleBox = m_aboutCard = m_freePill = {};

        // The navigation: the app, Find a setting, the pages.
        m_navWidth = Px(rc.right < Px(820) ? kNavNarrow : kNavWide);
        const int nx = Px(14), nw = m_navWidth - Px(28);
        m_header = { nx + Px(8), Px(12), nx + Px(64), Px(68) };
        Place(m_appName, nx + Px(78), Px(18), nw - Px(78), Px(22));
        Place(m_version, nx + Px(78), Px(42), nw - Px(78), Px(18));
        PlaceField(m_find, { nx, Px(86), nx + nw, Px(118) });
        m_placed.push_back(m_find);
        int y = Px(128);
        for (HWND item : m_nav)
        {
            Place(item, nx, y, nw, Px(kNavItem));
            y += Px(kNavItem + kNavGap);
        }
        if (GetWindowTextLengthW(m_findHint) > 0)
            Place(m_findHint, nx + Px(4), y + Px(8), nw - Px(8), LabelHeight(m_findHint, nw - Px(8)));

        // The page, in a column that scrolls.
        const int x = m_navWidth + Px(24), width = std::max(Px(240), static_cast<int>(rc.right) - x - Px(40));
        const int top = Px(4) - ScrollY();
        Place(m_heading, x, top, width, Px(40));
        const int end = LayoutPage(x, top + Px(62), width);
        const int before = ScrollY();
        SetScroll({ m_navWidth, 0, rc.right, rc.bottom }, end + ScrollY() + Px(40));
        if (ScrollY() != before)
        {
            Layout();   // the page got shorter than where it was scrolled to: once more, from the new place
            return;
        }
        // Everything this page doesn't show is hidden, so Tab and screen readers skip it.
        for (HWND h : m_all)
        {
            const bool placed = std::find(m_placed.begin(), m_placed.end(), h) != m_placed.end();
            if (placed != (IsWindowVisible(h) != FALSE))
                ShowWindow(h, placed ? SW_SHOWNA : SW_HIDE);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    int SettingsWindow::LayoutPage(int x, int y, int width)
    {
        const int gap = Px(kCardGap);
        const auto card = [&](Item item, int controlW, int controlH) {
            auto& row = m_rows[item];
            const int bottom = LayoutRow(row, x, y, width, controlW, controlH);
            m_cards.push_back({ row.card, settings::Find(item).icon });
            y = bottom + gap;
        };
        // A segmented choice at the end of a card.
        const auto segmented = [&](Item item, std::span<const HWND> buttons) {
            std::vector<int> widths;
            int total = 0;
            for (HWND b : buttons)
                total += widths.emplace_back(Px(static_cast<int>(Kit().SegmentWidth(Text(b)))));
            auto& row = m_rows[item];
            row.control = nullptr;
            const int top = y;
            card(item, total, Px(kSegmentH));
            int sx = x + width - Px(14) - total;
            const int sy = top + (row.card.bottom - row.card.top - Px(kSegmentH)) / 2;
            for (size_t i = 0; i < buttons.size(); ++i)
            {
                Place(buttons[i], sx, sy, widths[i], Px(kSegmentH));
                sx += widths[i];
            }
            row.control = buttons[0];
        };
        const auto buttonWidth = [&](HWND b, bool icon) { return Px(static_cast<int>(Kit().TextWidth(Text(b), ControlKit::kControlText)) + (icon ? 52 : 30)); };

        switch (m_page)
        {
        case Page::General:
        {
            card(Item::StartWithWindows, Px(96), Px(kControlH));
            card(Item::OcrLanguage, Px(230), Px(kControlH));
            card(Item::FloatShelf, Px(96), Px(kControlH));
            // The "Files" group: its heading, 22 DIPs above and 8 under.
            y += Px(22) - gap;
            Place(m_filesGroup, x + Px(2), y, width, Px(20));
            y += Px(20 + 8);
            segmented(Item::BrowserDrops, m_drops);
            // Terminal app, with the command line for a terminal of your own under it in the same card.
            card(Item::Terminal, Px(230), Px(kControlH));
            auto& terminal = m_cards.back();
            auto& command = m_rows[Item::TerminalCommand];
            // The field sits in a box of its own (PlaceField), so LayoutRow only makes room for it.
            const int fw = std::min(Px(300), width / 2);
            command.control = nullptr;
            const int sub = LayoutRow(command, x, terminal.rect.bottom, width, fw, Px(kControlH));
            command.control = m_commandLine;
            const RECT box{ x + width - Px(14) - fw, terminal.rect.bottom + (sub - terminal.rect.bottom - Px(kControlH)) / 2, x + width - Px(14),
                            terminal.rect.bottom + (sub - terminal.rect.bottom - Px(kControlH)) / 2 + Px(kControlH) };
            PlaceField(m_commandLine, box);
            m_placed.push_back(m_commandLine);
            m_lines.push_back({ x + Px(1), terminal.rect.bottom, x + width - Px(1), terminal.rect.bottom + std::max(1, Px(1)) });
            terminal.rect.bottom = sub;
            m_rows[Item::Terminal].card.bottom = sub;
            y = sub + gap;
            break;
        }
        case Page::Appearance:
        {
            m_preview = { x, y, x + width, y + Px(kPreviewH) };
            Place(m_previewLabel, x + Px(16), y + Px(12), Px(200), Px(18));
            y += Px(kPreviewH) + gap;
            segmented(Item::Theme, m_themes);
            segmented(Item::Style, m_looks);
            segmented(Item::Size, m_sizes);
            // The slider, its value at the end, and its two ends under it.
            const int sw = std::min(Px(300), width / 2);
            const int top = y;
            card(Item::PanelSize, sw, Px(48));
            const auto& row = m_rows[Item::PanelSize];
            const int sy = top + (row.card.bottom - row.card.top - Px(48)) / 2, sx = x + width - Px(14) - sw;
            Place(m_panelSize, sx - Px(10), sy, sw - Px(56) + Px(20), Px(28));
            Place(m_panelValue, sx + sw - Px(48), sy, Px(48), Px(28));
            Place(m_panelLow, sx, sy + Px(28), Px(60), Px(18));
            SetRole(m_panelHigh, TextRole::Small);
            Place(m_panelHigh, sx + sw - Px(56) - Px(60), sy + Px(28), Px(60), Px(18));
            // The accent: a card that folds open on the swatches and the Windows switch.
            const int cardTop = y;
            Place(m_accentHeader, x, y, width, Px(kRowMin));
            y += Px(kRowMin);
            if (m_accentOpen)
            {
                m_lines.push_back({ x + Px(1), y, x + width - Px(1), y + Px(1) });
                const int cells = accent::kPresetCount + 1, cell = Px(kSwatchCell);
                const int perLine = std::max(1, (width - Px(54) - Px(18)) / cell);
                for (int i = 0; i < cells; ++i)
                {
                    const HWND h = i < accent::kPresetCount ? m_swatches[static_cast<size_t>(i)] : m_custom;
                    Place(h, x + Px(46) + (i % perLine) * cell, y + Px(14) + (i / perLine) * Px(kSwatchH + 8), cell, Px(kSwatchH));
                }
                y += Px(14) + ((cells + perLine - 1) / perLine) * Px(kSwatchH + 8) + Px(6);
                m_lines.push_back({ x + Px(1), y, x + width - Px(1), y + Px(1) });
                auto& windows = m_rows[Item::WindowsAccent];
                y = LayoutRow(windows, x, y, width, Px(96), Px(kControlH));
            }
            m_cards.push_back({ { x, cardTop, x + width, y }, 0 });
            m_rows[Item::Accent].card = { x, cardTop, x + width, y };
            m_rows[Item::WindowsAccent].card = m_accentOpen ? m_rows[Item::WindowsAccent].card : m_rows[Item::Accent].card;
            y += gap;
            break;
        }
        case Page::Wheel:
            y = LayoutWheel(x, y, width);
            m_rows[Item::WheelContents].card = { x, y, x + width, y + Px(36) };
            break;
        case Page::Output:
        {
            // The naming card: its title, then a live example, then a choice per style.
            const int cardTop = y;
            auto& row = m_rows[Item::OutputNaming];
            y = LayoutRow(row, x, y, width, 0, 0);
            m_lines.push_back({ x + Px(1), y, x + width - Px(1), y + Px(1) });
            m_exampleBox = { x + Px(54), y + Px(16), x + width - Px(18), y + Px(16 + 56) };
            Place(m_example, m_exampleBox.left + Px(16), m_exampleBox.top, std::max(0, static_cast<int>(m_exampleBox.right - m_exampleBox.left) - Px(130)),
                  m_exampleBox.bottom - m_exampleBox.top);
            Place(m_exampleNote, m_exampleBox.right - Px(110), m_exampleBox.top + Px(19), Px(96), Px(18));
            SetRole(m_exampleNote, TextRole::Caption);
            y = m_exampleBox.bottom + Px(10);
            for (HWND radio : m_namings)
            {
                Place(radio, x + Px(54), y, width - Px(72), Px(34));
                y += Px(36);
            }
            y += Px(10);
            m_cards.push_back({ { x, cardTop, x + width, y }, settings::Find(Item::OutputNaming).icon, cardTop + Px(kRowMin) / 2 });
            row.card = { x, cardTop, x + width, y };
            y += gap;
            break;
        }
        case Page::Advanced:
            card(Item::ShellMenu, Px(96), Px(kControlH));
            card(Item::RecipesFolder, buttonWidth(m_recipes, true), Px(kControlH));
            card(Item::LogsFolder, buttonWidth(m_logs, true), Px(kControlH));
            break;
        case Page::About:
        {
            // The app's mark and name, its version and a pill that says it is free.
            m_aboutCard = { x, y, x + width, y + Px(120) };
            m_cards.push_back({ m_aboutCard, 0 });
            Place(m_aboutName, x + Px(92), y + Px(36), std::max(0, width - Px(320)), Px(48));
            Place(m_aboutVersion, x + width - Px(240), y + Px(26), Px(216), Px(22));
            const int pw = Px(static_cast<int>(Kit().TextWidth(L"Free and open source", 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 32);
            m_freePill = { x + width - Px(24) - pw, y + Px(58), x + width - Px(24), y + Px(92) };
            Place(m_aboutFree, m_freePill.left + Px(16), m_freePill.top + Px(1), pw - Px(32), m_freePill.bottom - m_freePill.top - Px(2));
            y += Px(120) + gap;
            m_rows[Item::Version].card = m_aboutCard;
            card(Item::SourceCode, Px(32), Px(32));
            card(Item::Privacy, 0, 0);
            break;
        }
        }
        return y;
    }

    int SettingsWindow::LayoutWheel(int x, int y, int width)
    {
        PlaceField(m_findAction, { x, y, x + width, y + Px(36) });
        m_placed.push_back(m_findAction);
        y += Px(36 + 14);
        // The section tabs, a line under them.
        int tx = x;
        for (size_t i = 0; i < m_tabs.size(); ++i)
        {
            const int w = Px(static_cast<int>(Kit().TabWidth(RingTitle(static_cast<Ring>(i)))));
            Place(m_tabs[i], tx, y, w, Px(38));
            tx += w + Px(26);
        }
        y += Px(38);
        m_lines.push_back({ x, y, x + width, y + Px(1) });
        y += Px(16);
        // The section's line, and its two buttons at the end.
        const int resetW = Px(static_cast<int>(Kit().TextWidth(Text(m_reset), ControlKit::kControlText)) + 30);
        const int offW = Px(static_cast<int>(Kit().TextWidth(L"Turn off this section", ControlKit::kControlText)) + 52);
        Place(m_reset, x + width - resetW, y, resetW, Px(kControlH));
        Place(m_sectionOff, x + width - resetW - Px(8) - offW, y, offW, Px(kControlH));
        const int sw = std::max(Px(80), width - resetW - offW - Px(24));
        Place(m_summary, x, y + (Px(kControlH) - LabelHeight(m_summary, sw)) / 2, sw, LabelHeight(m_summary, sw));
        y += Px(kControlH + 12);

        // A card per kind of file; searching opens every card with a match and shows only those chips.
        const std::wstring query = Text(m_findAction);
        const bool searching = !Trim(query).empty();
        bool any = false;
        for (auto& g : m_groups)
        {
            if (g.ring != m_section)
                continue;
            size_t matches = 0;
            for (const auto& chip : g.chips)
                matches += contents::Matches(*chip.slice, query) ? 1 : 0;
            if (searching && matches == 0)
                continue;
            any = true;
            const bool open = g.open || searching;
            if (StyleOf(g.header).open != open)
            {
                StyleOf(g.header).open = open;
                Redraw(g.header);
            }
            const int cardTop = y;
            Place(g.header, x, y, width, Px(kRowMin));
            y += Px(kRowMin);
            if (open)
            {
                m_lines.push_back({ x + Px(1), y, x + width - Px(1), y + Px(1) });
                // The chips at the start; the preview at the end, or under them when the card is narrow.
                const bool side = width >= Px(560);
                const int bx = x + Px(54), bw = width - Px(54) - Px(18) - (side ? Px(kWheelPreview + 24) : 0);
                int by = y + Px(18);
                const int hintH = LabelHeight(g.hint, bw);
                Place(g.hint, bx, by, bw, hintH);
                int cx = bx, cy = by + hintH + Px(12);
                for (const auto& chip : g.chips)
                {
                    if (!contents::Matches(*chip.slice, query))
                        continue;
                    const int w = Px(static_cast<int>(Kit().ChipWidth(chip.slice->label)));
                    if (cx > bx && cx + w > bx + bw)
                    {
                        cx = bx;
                        cy += Px(kControlH + 8);
                    }
                    Place(chip.button, cx, cy, w, Px(kControlH));
                    cx += w + Px(8);
                }
                int bottom = cy + Px(kControlH);
                const int px = side ? x + width - Px(18) - Px(kWheelPreview) : bx;
                const int py = side ? by : bottom + Px(18);
                g.radius = 88.f;
                g.wheel = D2D1::Point2F(Dip(px) + kWheelPreview / 2.f, Dip(py) + 90.f);
                Place(g.preview, px, py + Px(184), Px(kWheelPreview), Px(18));
                bottom = std::max(bottom, py + Px(184 + 18));
                g.body = { x, y, x + width, bottom + Px(18) };
                y = bottom + Px(18);
            }
            m_cards.push_back({ { x, cardTop, x + width, y }, 0 });
            y += Px(kCardGap);
        }
        if (!any)
        {
            SetWindowTextW(m_noAction, searching ? std::format(L"No action in {} matches \u201C{}\u201D.", RingTitle(m_section), query).c_str()
                                                 : L"Nothing in this section.");
            Place(m_noAction, x, y + Px(8), width, LabelHeight(m_noAction, width));
            y += Px(40);
        }
        return y;
    }

    // ---- Painting -----------------------------------------------------------------------------------------

    void SettingsWindow::PaintSurface(ControlKit& kit, const D2D1_RECT_F& client)
    {
        const auto& c = kit.Theme();
        const auto visible = [&](const RECT& r) { return r.bottom > 0 && Dip(r.top) < client.bottom && r.right > r.left; };
        for (const auto& card : m_cards)
        {
            if (!visible(card.rect))
                continue;
            const auto r = DipRect(card.rect);
            kit.DrawCard(r);
            if (card.icon)
            {
                const float cy = card.iconY ? Dip(card.iconY) : (r.top + r.bottom) / 2;
                kit.DrawGlyph(card.icon, 18.f, D2D1::RectF(r.left + 16, cy - 12, r.left + 40, cy + 12), c.text);
            }
        }
        // The setting "Find a setting" found: a ring in the accent around its card.
        if (m_showFound && settings::Find(m_found).page == m_page)
            if (const auto it = m_rows.find(m_found); it != m_rows.end() && visible(it->second.card))
                kit.StrokeRound(DipRect(it->second.card), c.accent, 2.f, ControlKit::kCardRadius);
        for (const auto& line : m_lines)
            if (line.bottom > line.top)
                kit.FillRound(DipRect(line), c.line, 0.f);
        const auto palette = theme::Current(m_state.appearance.style);
        if (m_page == Page::Appearance && visible(m_preview))
        {
            // The wheel as it opens on a photo, with its second tile under the mouse.
            const auto r = DipRect(m_preview);
            kit.FillRound(r, c.stage, ControlKit::kCardRadius);
            kit.StrokeRound(r, c.line, 1.f, ControlKit::kCardRadius);
            std::vector<ControlKit::MiniTile> tiles;
            if (const auto* convert = m_tree.Find(Ring::Convert); convert && !convert->groups.empty())
                for (const auto& slice : contents::PreviewSlices(m_state.features, convert->groups.front()))
                    tiles.push_back({ slice.label, slice.icon });
            const float radius = std::min(112.f, (r.bottom - r.top) / 2 - 14);
            kit.DrawMiniWheel(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2 + 6), radius, tiles, tiles.size() > 1 ? 1 : 0, palette,
                              L"1 file", tiles.size() > 1 ? std::wstring_view(tiles[1].label) : std::wstring_view());
        }
        if (m_page == Page::Wheel)
            for (const auto& g : m_groups)
                if (g.ring == m_section && IsWindowVisible(g.preview))
                {
                    std::vector<ControlKit::MiniTile> tiles;
                    for (const auto& slice : contents::PreviewSlices(m_state.features, *g.group))
                        tiles.push_back({ slice.label, slice.icon });
                    kit.DrawMiniWheel(g.wheel, g.radius, tiles, -1, palette, RingTitle(g.ring), L"");
                }
        if (m_page == Page::Output && visible(m_exampleBox))
            kit.FillRound(DipRect(m_exampleBox), c.window, ControlKit::kCardRadius);
        if (m_page == Page::About && visible(m_freePill))
        {
            const auto pill = DipRect(m_freePill);
            kit.FillRound(pill, c.control, (pill.bottom - pill.top) / 2);
            kit.StrokeRound(pill, c.controlLine, 1.f, (pill.bottom - pill.top) / 2);
        }
    }

    void SettingsWindow::PaintGdi(HDC dc)
    {
        if (!m_icon)
            return;
        DrawIconEx(dc, m_header.left, m_header.top, m_icon, m_header.right - m_header.left, m_header.bottom - m_header.top, 0, nullptr, DI_NORMAL);
        if (m_page == Page::About && m_aboutCard.bottom > m_aboutCard.top)
            DrawIconEx(dc, m_aboutCard.left + Px(28), m_aboutCard.top + Px(36), m_icon, Px(48), Px(48), 0, nullptr, DI_NORMAL);
    }

    void SettingsWindow::DrawControl(HWND control, ControlKit& kit, const D2D1_RECT_F& r, ControlKit::ControlState s)
    {
        // A swatch: the colour over its name.
        const auto& a = m_state.preferences.accent;
        const auto colour = [](uint32_t rgb) { return theme::Rgb(rgb); };
        const float cx = (r.left + r.right) / 2;
        const auto disc = D2D1::RectF(cx - 21, r.top, cx + 21, r.top + 42);
        bool selected = false;
        std::wstring name;
        if (control == m_custom)
        {
            // The colour of your own once there is one; ticked while in use.
            const bool picked = a.custom != accent::kNoColour;
            selected = a.choice == accent::Choice::Custom;
            kit.DrawSwatch(disc, picked ? colour(a.custom) : kit.Theme().control, selected ? kGlyphCheck : picked ? kGlyphPalette : kGlyphAdd,
                           picked ? colour(accent::TextOn(a.custom)) : kit.Theme().text, selected, s);
            name = L"Custom\u2026";
        }
        else
            for (int i = 0; i < accent::kPresetCount; ++i)
                if (control == m_swatches[static_cast<size_t>(i)])
                {
                    const auto choice = static_cast<accent::Choice>(i);
                    const uint32_t fill = accent::Preset(choice).fill;
                    selected = a.choice == choice;
                    kit.DrawSwatch(disc, colour(fill), selected ? kGlyphCheck : 0, colour(accent::TextOn(fill)), selected, s);
                    name = accent::Name(choice);
                }
        kit.DrawText(name, kit.TextFormat(ControlKit::kSmallText, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                     D2D1::RectF(r.left, r.top + 42, r.right, r.bottom), selected ? kit.Theme().text : kit.Theme().textDim);
    }

    // ---- Window -----------------------------------------------------------------------------------------

    LRESULT SettingsWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            if (id == IDCANCEL)
                ShowWindow(m_hwnd, SW_HIDE);
            else if (id == IDOK)
            {
                // Enter in Find a setting: to the setting it found.
                if (GetFocus() == m_find && m_showFound)
                    if (const HWND target = m_rows[m_found].control)
                        SetFocus(target);
            }
            else if (!m_updating)
                OnCommand(id, HIWORD(wParam), reinterpret_cast<HWND>(lParam));
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == m_panelSize)
                OnPanelSizeScroll(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            ShowWindow(m_hwnd, SW_HIDE);   // kept for next time; destroyed with the app
            return 0;
        default:
            break;
        }
        return DialogWindow::Handle(msg, wParam, lParam);
    }
}
