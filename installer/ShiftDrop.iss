; ShiftDrop installer (Inno Setup 6.3 or later). CI builds it on every push; to build it by hand after
; a Release build (see docs/BUILDING.md, "Build the installer"):
;   ./scripts/get-ffmpeg.ps1
;   ./scripts/make-notices.ps1
;   & "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.6.0 installer\ShiftDrop.iss
;
; Per-user install: no admin rights, no UAC prompt. Files go to %LOCALAPPDATA%\Programs\ShiftDrop.
; The app is linked with the static CRT, so nothing else (no VC++ redistributable) is installed.
; ffmpeg.exe and ShiftDropWorker.exe (engines\) are the only other programs; ffmpeg runs when a video or
; audio file is converted, the worker when an archive is read.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppExe
  #define AppExe "..\build\x64-release\Release\ShiftDrop.exe"
#endif
#ifndef WorkerExe
  #define WorkerExe "..\build\x64-release\Release\ShiftDropWorker.exe"
#endif
#ifndef EnginesDir
  #define EnginesDir "..\build\engines"
#endif
#ifndef NoticesFile
  #define NoticesFile "..\build\installer\THIRD-PARTY-NOTICES.txt"
#endif
; Must match SD_NAME in src/Brand.h: the exe, the settings folder and the Run value are all named after it.
#define AppName "ShiftDrop"

[Setup]
; AppId identifies the install for upgrades and uninstall; never change it.
AppId={{55068C09-84E0-491F-941F-E15021376900}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=CoPrime69
AppPublisherURL=https://github.com/CoPrime69/ShiftDrop
VersionInfoVersion={#AppVersion}
; lowest = install for the current user only; {autopf} then means %LOCALAPPDATA%\Programs.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableDirPage=auto
; Inno Setup 6 skips the welcome page unless asked; ours says what ShiftDrop is ([Messages] below).
DisableWelcomePage=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 22H2 (build 19045) or later.
MinVersion=10.0.19045
OutputDir=..\build\installer
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
SetupIconFile=..\src\ShiftDrop.ico
UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
; The wizard's art at 100% and 200% (Setup picks the best match for the screen's DPI), rendered from
; the brand kit's drawings as 24-bit BMP, which every Inno Setup 6 reads.
WizardImageFile=images\wizard-side-164x314.bmp,images\wizard-side-328x628.bmp
WizardSmallImageFile=images\wizard-small-55x55.bmp,images\wizard-small-110x110.bmp
; ultra64: the strongest LZMA2 setting (a 64 MB dictionary); the installer is built once per CI run.
Compression=lzma2/ultra64
SolidCompression=yes
; Fallback only: PrepareToInstall below asks a running copy to quit first. ShiftDrop has no visible
; top-level window for Restart Manager to close, so it is never restarted by Setup.
CloseApplications=yes
RestartApplications=no

[Messages]
; The welcome and finish pages. The name and version come from the defines above, like everywhere
; else in this script.
WelcomeLabel2=Setup will install {#AppName} {#AppVersion} on this PC. It runs offline and needs no account. Works on Windows 10 and 11.
FinishedLabel=Drag any file and press Shift to open the wheel. {#AppName} lives in the tray.
FinishedLabelNoIcons=Drag any file and press Shift to open the wheel. {#AppName} lives in the tray.

[Tasks]
; Shown on a fresh install only. On an upgrade the choice already saved by the app (the tray menu's
; "Start with Windows") wins, so reinstalling never turns it back on.
Name: startup; Description: "Start {#AppName} when I sign in to Windows"; Check: not HasSavedChoice
; The right-click entry for files and folders (per user, HKCU\Software\Classes). Same rule: an upgrade
; keeps whatever the tray menu last chose.
Name: shellmenu; Description: "Add ""{#AppName}..."" to the right-click menu of files and folders"; Check: not HasSavedChoice

[Dirs]
; Settings, history, recipes and logs. Kept on uninstall: they are the user's data.
Name: "{localappdata}\{#AppName}"; Flags: uninsneveruninstall

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#NoticesFile}"; DestDir: "{app}"; Flags: ignoreversion
; Video and audio (scripts/get-ffmpeg.ps1): the LGPL ffmpeg.exe (our own build from unmodified FFmpeg
; sources, scripts/build-ffmpeg.sh), with its licences and where its exact source is.
Source: "{#EnginesDir}\ffmpeg.exe"; DestDir: "{app}\engines"; Flags: ignoreversion
Source: "{#EnginesDir}\FFMPEG-LICENSE.txt"; DestDir: "{app}\engines"; Flags: ignoreversion
Source: "{#EnginesDir}\FFMPEG-SOURCE.txt"; DestDir: "{app}\engines"; Flags: ignoreversion
; The sandboxed worker: reads archives inside an AppContainer with no file or network access.
Source: "{#WorkerExe}"; DestDir: "{app}\engines"; Flags: ignoreversion

[Icons]
; The ID and the activator let the app show real toasts with buttons (src/toast/ToastXml.h has both).
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; AppUserModelID: "CoPrime69.ShiftDrop"; AppUserModelToastActivatorCLSID: "AF377B89-066C-4AD3-8387-8B77C419B8EE"

[INI]
; Same key the app reads (startup::Choice). Written only when there is no saved choice yet.
Filename: "{localappdata}\{#AppName}\settings.ini"; Section: "Startup"; Key: "StartWithWindows"; String: "1"; Tasks: startup; Check: not HasSavedChoice
Filename: "{localappdata}\{#AppName}\settings.ini"; Section: "Startup"; Key: "StartWithWindows"; String: "0"; Tasks: not startup; Check: not HasSavedChoice

[Registry]
; The exact value the app writes itself (startup::Enable), so the app sees it as already correct.
; Written here as well so login starts the installed copy even if it is not launched after Setup.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppName}.exe"" --autostart"; Flags: uninsdeletevalue; Check: StartupWanted
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "{#AppName}"; Flags: deletevalue dontcreatekey; Check: not StartupWanted

[Run]
; The app writes (or removes) the right-click entry itself, pointing at the installed exe.
Filename: "{app}\{#AppName}.exe"; Parameters: "--register-shell"; Flags: runhidden waituntilterminated; Check: ShellMenuWanted
Filename: "{app}\{#AppName}.exe"; Parameters: "--unregister-shell"; Flags: runhidden waituntilterminated; Check: not ShellMenuWanted
; The finish page's "Start" box, ticked by default: a postinstall entry is, unless flagged unchecked.
Filename: "{app}\{#AppName}.exe"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Quits the running copy and removes the Run value before the files are deleted.
Filename: "{app}\{#AppName}.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "QuitAndRemoveStartup"
; --uninstall removes the right-click entry too; this covers an exe that predates that.
Filename: "{app}\{#AppName}.exe"; Parameters: "--unregister-shell"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveShellMenu"

[Code]
var
  SavedChoice: String;
  HadShellMenu: Boolean;

function InitializeSetup: Boolean;
begin
  // Read once, before [INI] runs, so every Check below sees the state from before this install.
  SavedChoice := GetIniString('Startup', 'StartWithWindows', '', ExpandConstant('{localappdata}\{#AppName}\settings.ini'));
  HadShellMenu := RegKeyExists(HKCU, 'Software\Classes\Directory\shell\{#AppName}');
  Result := True;
end;

function HasSavedChoice: Boolean;
begin
  Result := SavedChoice <> '';
end;

function ShellMenuWanted: Boolean;
begin
  if HasSavedChoice then
    Result := HadShellMenu   // an upgrade: keep the entry if it was there (it is re-pointed at the new exe)
  else
    Result := WizardIsTaskSelected('shellmenu');
end;

function StartupWanted: Boolean;
begin
  if HasSavedChoice then
    Result := StrToIntDef(SavedChoice, 1) <> 0   // the app treats any non-zero value as on
  else
    Result := WizardIsTaskSelected('startup');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  // A copy may be running from the install folder or from anywhere else (a portable copy). Ask it to
  // exit so its exe can be replaced and the new one can start. The new exe does the asking because
  // an older one may predate --quit.
  ExtractTemporaryFile('{#AppName}.exe');
  if not Exec(ExpandConstant('{tmp}\{#AppName}.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Log('Could not run --quit; Restart Manager will handle a running copy.')
  else if Code <> 0 then
    Log('The running copy did not exit in time; Restart Manager will handle it.');
end;
