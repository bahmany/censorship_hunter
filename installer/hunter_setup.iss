; Hunter VPN Installer - Inno Setup Script
; Installs to user profile (no admin required)
; Supports update: kills running processes, replaces files
; Bundles cached configs for immediate start in censored networks

#define MyAppName "Hunter VPN"
#define MyAppVersion "1.2.1"
#define MyAppPublisher "Hunter Project"
#define MyAppExeName "hunter_dashboard.exe"
#define MyAppCLIName "hunter_cli.exe"
#define MyStagingDir "D:\projects\v2ray\pythonProject1\hunter\installer\staging"

[Setup]
AppId={{B7E2A8F1-4D3C-4E5F-9A1B-2C3D4E5F6A7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\HunterVPN
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=D:\projects\v2ray\pythonProject1\hunter\installer\output
OutputBaseFilename=HunterVPN-Setup-v{#MyAppVersion}
SetupIconFile={#MyStagingDir}\app_icon.ico
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=4
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\app_icon.ico
UninstallDisplayName={#MyAppName}
WizardStyle=modern
DisableWelcomePage=no
AllowCancelDuringInstall=yes
CloseApplications=force
RestartApplications=no
MinVersion=10.0
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Hunter VPN - Anti-Censorship Tool
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel1=Hunter VPN Setup
WelcomeLabel2=This will install Hunter VPN v{#MyAppVersion} on your computer.%n%nHunter VPN helps you bypass internet censorship with:%n%n- Dual-protocol proxy (SOCKS + HTTP)%n- TLS fragmentation anti-DPI%n- Smart routing%n- Pre-loaded configs for immediate connectivity%n%nNo administrator access required.

[Tasks]
Name: "desktopicon"; Description: "Create Desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "startupicon"; Description: "Start Hunter VPN at Windows startup"; GroupDescription: "Startup:"

[Files]
; Dashboard UI executable + Flutter runtime
Source: "{#MyStagingDir}\hunter_dashboard.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\flutter_windows.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\screen_retriever_windows_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\system_tray_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\window_manager_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

; MSVC Runtime (needed by dashboard UI)
Source: "{#MyStagingDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStagingDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion

; CLI + proxy cores in bin\ (CLI is statically linked, no DLL deps)
Source: "{#MyStagingDir}\bin\hunter_cli.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#MyStagingDir}\bin\xray.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#MyStagingDir}\bin\sing-box.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#MyStagingDir}\bin\mihomo-windows-amd64-compatible.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#MyStagingDir}\bin\tor.exe"; DestDir: "{app}\bin"; Flags: ignoreversion

; Icon
Source: "{#MyStagingDir}\app_icon.ico"; DestDir: "{app}"; Flags: ignoreversion

; Config files (bundled VPN configs for immediate use under censorship)
Source: "{#MyStagingDir}\config\All_Configs_Sub.txt"; DestDir: "{app}\config"; Flags: ignoreversion
Source: "{#MyStagingDir}\config\all_extracted_configs.txt"; DestDir: "{app}\config"; Flags: ignoreversion
Source: "{#MyStagingDir}\config\sub.txt"; DestDir: "{app}\config"; Flags: ignoreversion

; Pre-loaded runtime cache (only install if not already present - don't overwrite user data)
Source: "{#MyStagingDir}\runtime\HUNTER_gold.txt"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\HUNTER_silver.txt"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\HUNTER_balancer_cache.json"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\HUNTER_gemini_balancer_cache.json"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\HUNTER_config_db.tsv"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\hunter_config.json"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\Hunter VPN Dashboard"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"; WorkingDir: "{app}"
Name: "{group}\Hunter VPN CLI"; Filename: "{app}\bin\{#MyAppCLIName}"; IconFilename: "{app}\app_icon.ico"; WorkingDir: "{app}"
Name: "{group}\Uninstall Hunter VPN"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Hunter VPN"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Auto-start at login (user-level, no admin needed)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "HunterVPN"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startupicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Hunter VPN Dashboard"; Flags: nowait postinstall skipifsilent; WorkingDir: "{app}"

[UninstallRun]
Filename: "taskkill"; Parameters: "/F /IM hunter_dashboard.exe"; Flags: runhidden; RunOnceId: "KillDashboard"
Filename: "taskkill"; Parameters: "/F /IM hunter_cli.exe"; Flags: runhidden; RunOnceId: "KillCLI"
Filename: "taskkill"; Parameters: "/F /IM xray.exe"; Flags: runhidden; RunOnceId: "KillXray"
Filename: "taskkill"; Parameters: "/F /IM sing-box.exe"; Flags: runhidden; RunOnceId: "KillSingBox"
Filename: "taskkill"; Parameters: "/F /IM mihomo-windows-amd64-compatible.exe"; Flags: runhidden; RunOnceId: "KillMihomo"
Filename: "taskkill"; Parameters: "/F /IM tor.exe"; Flags: runhidden; RunOnceId: "KillTor"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\runtime"
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\config"

[Code]
// Kill all Hunter-related processes before installation (for updates)
procedure KillRunningProcesses();
var
  ResultCode: Integer;
begin
  Log('Killing running Hunter processes for update...');
  Exec('taskkill', '/F /IM hunter_dashboard.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM hunter_cli.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM xray.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM sing-box.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM mihomo-windows-amd64-compatible.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM tor.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Wait a moment for processes to fully terminate
  Sleep(2000);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  // Kill any running instances before starting setup
  KillRunningProcesses();
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    // Kill again right before file copy (in case user restarted them)
    KillRunningProcesses();
  end;
end;

// Clean up system proxy on uninstall
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // Reset Windows system proxy if it was set by Hunter
    RegWriteDWordValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Internet Settings', 'ProxyEnable', 0);
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Internet Settings', 'ProxyServer');
    // Refresh WinInet settings
    Exec('rundll32.exe', 'wininet.dll,InternetSetOptionW', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
