; Wolfe's installer (Inno Setup 6). Built by installer\build.ps1, which passes:
;   /DAppVersion=x.y.z   /DBuildDir=<folder with wolfe.exe>   /DOutputDir=<folder>
;
; It installs Wolfe, and usbip-win2 when it is missing or older than the copy
; it carries: Wolfe's own devices reach Windows through it. usbip-win2 is
; installed by its own, unmodified installer, run silently; it stays installed
; (and listed in Installed apps) when Wolfe is uninstalled.

#ifndef AppVersion
  #error Use option /DAppVersion=x.y.z
#endif
#ifndef BuildDir
  #error Use option /DBuildDir=<folder with wolfe.exe>
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define UsbipVersion "0.9.8.1"
#define UsbipSetup "USBip-" + UsbipVersion + "-x64.exe"
; usbip-win2's own AppId, as its installer registers it.
#define UsbipUninstallKey "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1"

; What Wolfe answers to (src/gui/main.cpp): its window class, and the message
; that makes it save and exit without asking.
#define WolfeWindowClass "WolfeMainWindow"
#define WolfeQuitMessage "$8003"

#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"

[Setup]
AppId={{6f77c59d-c6d3-497b-b9ad-197a0a33eafb}
AppName=Wolfe
AppVersion={#AppVersion}
AppVerName=Wolfe {#AppVersion}
AppPublisher=Wolfe
AppCopyright=Copyright (C) 2026 The Wolfe authors. GNU GPL 3.0 or later.
AppPublisherURL=https://github.com/pentecoste/wolfe
AppSupportURL=https://github.com/pentecoste/wolfe/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Wolfe
DisableProgramGroupPage=yes
; An existing folder is checked by NextButtonClick instead, more strictly.
DirExistsWarning=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903, as usbip-win2 asks.
MinVersion=10.0.18362
WizardStyle=modern
SetupIconFile=..\res\wolfe.ico
UninstallDisplayIcon={app}\wolfe.exe
UninstallDisplayName=Wolfe
OutputDir={#OutputDir}
OutputBaseFilename=Wolfe-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
; Wolfe is closed by asking it (see PrepareToInstall); this is the fallback.
CloseApplications=yes
RestartApplications=no

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nWolfe is a patchbay for the computer's sound: it makes speakers and microphones of its own, and connects them and the computer's devices through effects. Its devices reach Windows through usbip-win2, which is installed with it if needed.

[Tasks]
Name: autostart; Description: "Start Wolfe with Windows (its devices exist only while it runs)"
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\wolfe.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\wolfe-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "cache\{#UsbipSetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall nocompression; Check: UsbipNeeded

[UninstallDelete]
Type: files; Name: "{app}\usbip-setup.log"

[Icons]
Name: "{autoprograms}\Wolfe"; Filename: "{app}\wolfe.exe"
Name: "{autodesktop}\Wolfe"; Filename: "{app}\wolfe.exe"; Tasks: desktopicon

[Run]
; "Start with Windows" belongs to the user who installs, not to the
; administrator account setup may be running as: reg.exe runs as that user.
; The entry is the one Wolfe's own menu writes and recognises.
Filename: "{sys}\reg.exe"; Parameters: "add ""HKCU\{#RunKey}"" /v Wolfe /t REG_SZ /d ""\""{app}\wolfe.exe\"" --tray"" /f"; Flags: runasoriginaluser runhidden; Tasks: autostart
Filename: "{sys}\reg.exe"; Parameters: "delete ""HKCU\{#RunKey}"" /v Wolfe /f"; Flags: runasoriginaluser runhidden; Tasks: not autostart; Check: AutostartIsOurs
Filename: "{app}\wolfe.exe"; Description: "Start Wolfe"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
var
  UsbipFound: String;    // the version of usbip-win2 already installed, or ''
  UsbipIsNeeded: Boolean;
  UsbipFailed: String;   // why usbip-win2 could not be installed, or ''
  RestartWanted: Boolean;

// ---------------------------------------------------------------------------
// usbip-win2

function UsbipInstalledVersion(): String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM64, '{#UsbipUninstallKey}', 'DisplayVersion', Result) then Result := '';
  // An entry without the program (a broken uninstall) does not count.
  if not FileExists(ExpandConstant('{commonpf64}\USBip\usbip.exe')) then Result := '';
end;

function OlderThan(Found, Wanted: String): Boolean;
var
  A, B: Int64;
begin
  if (not StrToVersion(Found, A)) or (not StrToVersion(Wanted, B)) then
    Result := True
  else
    Result := ComparePackedVersion(A, B) < 0;
end;

function UsbipNeeded(): Boolean;
begin
  Result := UsbipIsNeeded;
end;

procedure InstallUsbip();
var
  Code: Integer;
  LogPath: String;
begin
  LogPath := ExpandConstant('{tmp}\usbip-setup.log');
  WizardForm.StatusLabel.Caption := 'Installing usbip-win2 {#UsbipVersion} (USB devices may stop for a moment)...';
  // Only what Wolfe needs: the client and its drivers, with the Visual C++
  // runtime it uses; no GUI, SDK or debug files. 3010: done, restart wanted.
  if not Exec(ExpandConstant('{tmp}\{#UsbipSetup}'),
              '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOICONS /COMPONENTS="main,client" /TASKS="vcredist" ' +
              '/RESTARTEXITCODE=3010 /LOG="' + LogPath + '"',
              '', SW_HIDE, ewWaitUntilTerminated, Code) then
  begin
    UsbipFailed := 'its installer could not be started: ' + SysErrorMessage(Code);
  end
  else if Code = 3010 then
  begin
    RestartWanted := True;
  end
  else if Code <> 0 then
  begin
    UsbipFailed := 'its installer stopped with code ' + IntToStr(Code);
  end;
  if (UsbipFailed = '') and (UsbipInstalledVersion() = '') then
    UsbipFailed := 'it does not appear installed afterwards';
  if UsbipFailed <> '' then
  begin
    CopyFile(LogPath, ExpandConstant('{app}\usbip-setup.log'), False);
    LogPath := ExpandConstant('{app}\usbip-setup.log');
    SuppressibleMsgBox('usbip-win2 could not be installed: ' + UsbipFailed + '.' + #13#13 +
                       'Wolfe is installed, but its own devices need usbip-win2. ' +
                       'It can be installed from https://github.com/vadimgrn/usbip-win2/releases' + #13#13 +
                       'Details: ' + LogPath, mbError, MB_OK, IDOK);
  end;
end;

// ---------------------------------------------------------------------------
// A running Wolfe

// Asks a running Wolfe to save and exit, and waits for it (10 s at most).
function CloseWolfe(): Boolean;
var
  Window: HWND;
  I: Integer;
begin
  Window := FindWindowByClassName('{#WolfeWindowClass}');
  if Window <> 0 then
  begin
    Log('Asking Wolfe to exit');
    PostMessage(Window, {#WolfeQuitMessage}, 0, 0);
    I := 0;
    while (FindWindowByClassName('{#WolfeWindowClass}') <> 0) and (I < 100) do
    begin
      Sleep(100);
      I := I + 1;
    end;
  end;
  Result := FindWindowByClassName('{#WolfeWindowClass}') = 0;
end;

// The current user's "Start with Windows" entry is this installation's.
function AutostartIsOurs(): Boolean;
var
  Command: String;
begin
  Result := RegQueryStringValue(HKCU, '{#RunKey}', 'Wolfe', Command) and
            (Pos(Lowercase(ExpandConstant('{app}\wolfe.exe')), Lowercase(Command)) > 0);
end;

// ---------------------------------------------------------------------------
// Setup

function InitializeSetup(): Boolean;
begin
  UsbipFound := UsbipInstalledVersion();
  UsbipIsNeeded := (UsbipFound = '') or OlderThan(UsbipFound, '{#UsbipVersion}');
  Log('usbip-win2 installed: "' + UsbipFound + '", to install: ' + IntToStr(Ord(UsbipIsNeeded)));
  Result := True;
end;

// A folder that already holds something other than Wolfe is refused: what
// setup puts there would replace files of the same name, and uninstalling
// would delete them.
function FolderIsUsable(Dir: String): Boolean;
var
  Found: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) then Exit;
  if FileExists(AddBackslash(Dir) + 'wolfe.exe') and FileExists(AddBackslash(Dir) + 'unins000.exe') then Exit;
  if FindFirst(AddBackslash(Dir) + '*', Found) then
  begin
    try
      repeat
        if (Found.Name <> '.') and (Found.Name <> '..') then Result := False;
      until (not Result) or (not FindNext(Found));
    finally
      FindClose(Found);
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  // (Also on a silent install, where it stops setup.)
  if (CurPageID = wpSelectDir) and not FolderIsUsable(WizardDirValue()) then
  begin
    SuppressibleMsgBox(WizardDirValue() + ' already contains other files.' + #13#13 +
                       'Choose an empty folder, or a new one: setup would replace files there, ' +
                       'and uninstalling Wolfe would delete them.', mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then Result := Result + MemoTasksInfo + NewLine + NewLine;
  if UsbipIsNeeded then
  begin
    if UsbipFound = '' then
      Result := Result + 'usbip-win2 {#UsbipVersion} will be installed as well.'
    else
      Result := Result + 'usbip-win2 will be updated from ' + UsbipFound + ' to {#UsbipVersion}.';
    Result := Result + NewLine + Space +
              'While its driver is installed, Windows restarts the USB controllers: USB devices' + NewLine + Space +
              '(a USB sound card, a mouse, a keyboard) stop for a moment. Windows may ask to restart.';
  end
  else
    Result := Result + 'usbip-win2 ' + UsbipFound + ' is already installed.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CloseWolfe() then
    Result := ''
  else
    Result := 'Wolfe is still running and did not exit when asked. Exit it from its Wolfe menu, then try again.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and UsbipIsNeeded then InstallUsbip();
end;

function NeedRestart(): Boolean;
begin
  Result := RestartWanted;
end;

// ---------------------------------------------------------------------------
// Uninstall

function InitializeUninstall(): Boolean;
begin
  Result := CloseWolfe();
  if not Result then
    SuppressibleMsgBox('Wolfe is still running and did not exit when asked. Exit it from its Wolfe menu, then try again.',
                       mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // The settings and the graph (%APPDATA%\Wolfe) stay, for a later install;
  // the entry that would start a program no longer there goes.
  if (CurUninstallStep = usPostUninstall) and AutostartIsOurs() then
    RegDeleteValue(HKCU, '{#RunKey}', 'Wolfe');
end;
