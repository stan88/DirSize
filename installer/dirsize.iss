; DirSize installer (Inno Setup 6.5 or later). docs/DESIGN-phase6.md.
; Build it with scripts\package-installer.ps1, which passes AppVersion and
; BinDir. This file is UTF-8 with a byte order mark.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z (scripts\package-installer.ps1 does)
#endif
#ifndef BinDir
  #define BinDir "..\build\Release"
#endif

[Setup]
AppId=DirSize
AppName=DirSize
AppVersion={#AppVersion}
AppPublisher=DirSize project
AppPublisherURL=https://github.com/stan88/DirSize
VersionInfoVersion={#AppVersion}
; The service runs as LocalSystem from a folder ordinary users cannot write
; to, and dirsize-service.exe registers itself from this fixed place: the
; folder is not selectable.
DefaultDirName={autopf}\DirSize
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 version 1809.
MinVersion=10.0.17763
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=DirSize-{#AppVersion}-setup
SetupIconFile=..\src\app\dirsize.ico
UninstallDisplayIcon={app}\dirsize.exe
UninstallDisplayName=DirSize
WizardStyle=modern
; Running copies of dirsize.exe are closed in PrepareToInstall instead.
CloseApplications=no
; The language dialog only when the Windows language matches neither.
ShowLanguageDialog=auto
; Always follow the Windows display language, as the program does. With the
; default (yes) an upgrade kept the language of the earlier installation:
; English on a Chinese Windows after the English-only first version.
UsePreviousLanguage=no

; English and Traditional Chinese. Setup picks the one matching the Windows
; display language (LanguageDetectionMethod uilanguage, the default) and
; asks only when neither matches. The Traditional Chinese messages are the
; official Inno Setup translation, kept unchanged in lang\ (lang\README.md:
; source, license); Inno Setup 6.7.3 does not ship it yet.
[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "zhtw"; MessagesFile: "lang\ChineseTraditional.isl"

[CustomMessages]
en.ContextMenuTask=Add "Open with DirSize" to the File Explorer context menu (all users)
zhtw.ContextMenuTask=在檔案總管的右鍵選單加入「用 DirSize 開啟」（所有使用者）
en.AutostartTask=Start the File Explorer panel when I sign in
zhtw.AutostartTask=登入時自動啟動檔案總管面板
en.ShareTask=Show the Share column (bar and percentage) in DirSize and the panel
zhtw.ShareTask=在 DirSize 和面板顯示比例欄（長條和百分比）
en.UnitTask=Size unit in DirSize and the panel:
zhtw.UnitTask=DirSize 和面板的顯示單位：
en.UnitAuto=Automatic (B, KB, MB, GB...)
zhtw.UnitAuto=自動（B、KB、MB、GB…）
en.UnitKB=KB
zhtw.UnitKB=KB
en.UnitGB=GB
zhtw.UnitGB=GB
en.StartPanel=Start the File Explorer panel now
zhtw.StartPanel=現在啟動檔案總管面板
en.ServiceFailed=The DirSize service could not be installed (code %1). DirSize still works, but reads folders more slowly.
zhtw.ServiceFailed=無法安裝 DirSize 服務（代碼 %1）。DirSize 仍然可以使用，但讀取資料夾會比較慢。

; Every setting the program has a switch for is offered here, checked, so a
; new user gets the whole program without looking for the switches. On a
; reinstall the previous choices are shown (UsePreviousTasks, default yes).
; All can be changed later in the program's "..." menu (the context menu
; only by reinstalling, as the program then shows "set by the installer").
[Tasks]
Name: "contextmenu"; Description: "{cm:ContextMenuTask}"
Name: "autostart"; Description: "{cm:AutostartTask}"
Name: "share"; Description: "{cm:ShareTask}"
; The size unit: one of three (exclusive child tasks, the Inno Setup [Tasks]
; help). Clearing the parent leaves the unit at automatic.
Name: "unit"; Description: "{cm:UnitTask}"
Name: "unit\auto"; Description: "{cm:UnitAuto}"; Flags: exclusive
Name: "unit\kb"; Description: "{cm:UnitKB}"; Flags: exclusive unchecked
Name: "unit\gb"; Description: "{cm:UnitGB}"; Flags: exclusive unchecked

[Files]
Source: "{#BinDir}\dirsize.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\dirsize-service.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\dirsize-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\DirSize"; Filename: "{app}\dirsize.exe"

[Registry]
; The classic verb for folders and drives, for all users. The default value
; is the plain text; MUIVerb points at string 181 (IDS_VERB_TEXT) in
; dirsize.exe, so File Explorer shows it in each user's language.
Root: HKLM; Subkey: "Software\Classes\Directory\shell\DirSize"; ValueType: string; ValueName: ""; ValueData: "Open with DirSize"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKLM; Subkey: "Software\Classes\Directory\shell\DirSize"; ValueType: string; ValueName: "MUIVerb"; ValueData: "@{app}\dirsize.exe,-181"; Tasks: contextmenu
Root: HKLM; Subkey: "Software\Classes\Directory\shell\DirSize\command"; ValueType: string; ValueName: ""; ValueData: """{app}\dirsize.exe"" ""%1"""; Tasks: contextmenu
Root: HKLM; Subkey: "Software\Classes\Drive\shell\DirSize"; ValueType: string; ValueName: ""; ValueData: "Open with DirSize"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKLM; Subkey: "Software\Classes\Drive\shell\DirSize"; ValueType: string; ValueName: "MUIVerb"; ValueData: "@{app}\dirsize.exe,-181"; Tasks: contextmenu
Root: HKLM; Subkey: "Software\Classes\Drive\shell\DirSize\command"; ValueType: string; ValueName: ""; ValueData: """{app}\dirsize.exe"" ""%1"""; Tasks: contextmenu
; Installing again with the task cleared removes the verb.
Root: HKLM; Subkey: "Software\Classes\Directory\shell\DirSize"; Flags: deletekey dontcreatekey; Tasks: not contextmenu
Root: HKLM; Subkey: "Software\Classes\Drive\shell\DirSize"; Flags: deletekey dontcreatekey; Tasks: not contextmenu

[Run]
; Per-user settings (HKCU, design 7.2): written by the program itself, run
; as the user who started Setup, not the administrator who approved it. If
; Setup was started already elevated ("Run as administrator"), the Inno
; Setup documentation says this flag has no effect, and the settings land
; in that elevated account. A cleared task turns the setting off.
Filename: "{app}\dirsize.exe"; Parameters: "--setting autostart on"; Flags: runasoriginaluser runhidden; Tasks: autostart
Filename: "{app}\dirsize.exe"; Parameters: "--setting autostart off"; Flags: runasoriginaluser runhidden; Tasks: not autostart
Filename: "{app}\dirsize.exe"; Parameters: "--setting share on"; Flags: runasoriginaluser runhidden; Tasks: share
Filename: "{app}\dirsize.exe"; Parameters: "--setting share off"; Flags: runasoriginaluser runhidden; Tasks: not share
Filename: "{app}\dirsize.exe"; Parameters: "--setting unit kb"; Flags: runasoriginaluser runhidden; Tasks: unit\kb
Filename: "{app}\dirsize.exe"; Parameters: "--setting unit gb"; Flags: runasoriginaluser runhidden; Tasks: unit\gb
Filename: "{app}\dirsize.exe"; Parameters: "--setting unit auto"; Flags: runasoriginaluser runhidden; Tasks: not unit\kb and not unit\gb
Filename: "{app}\dirsize.exe"; Description: "{cm:LaunchProgram,DirSize}"; Flags: postinstall nowait skipifsilent
Filename: "{app}\dirsize.exe"; Parameters: "--panel"; Description: "{cm:StartPanel}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
; Close running copies (all sessions), then stop and remove the service.
Filename: "{sys}\taskkill.exe"; Parameters: "/IM dirsize.exe /F"; Flags: runhidden; RunOnceId: "CloseDirSize"
Filename: "{app}\dirsize-service.exe"; Parameters: "uninstall"; Flags: runhidden; RunOnceId: "RemoveService"
; The sign-in setting of the account running the uninstaller. That is the
; user's own account when they approved the UAC prompt themselves; other
; accounts keep theirs (known limitation, DEVLOG).
Filename: "{sys}\reg.exe"; Parameters: "delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v DirSizePanel /f"; Flags: runhidden; RunOnceId: "RemoveAutostart"

[UninstallDelete]
; The cache and the service log (decision 7.4 A).
Type: filesandordirs; Name: "{commonappdata}\DirSize"

[Code]
// Before the files are replaced: close running copies of dirsize.exe (the
// window and the panel keep no unsaved data), and stop and remove the
// service of an earlier installation so its file is not in use.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  Service: String;
begin
  Result := '';
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM dirsize.exe /F', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Service := ExpandConstant('{app}\dirsize-service.exe');
  if FileExists(Service) then
    Exec(Service, 'uninstall', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// After the files are in place: register the service and start it.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;
  Code := -1;
  if Exec(ExpandConstant('{app}\dirsize-service.exe'), 'install', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start DirSize', '', SW_HIDE, ewWaitUntilTerminated, Code)
  else
    MsgBox(FmtMessage(CustomMessage('ServiceFailed'), [IntToStr(Code)]), mbError, MB_OK);
end;
