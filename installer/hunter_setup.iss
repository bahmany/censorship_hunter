; huntercensor Installer - Inno Setup Script
; Installs to user profile (no admin required)
; Supports update: kills running processes, replaces files
; Bundles cached configs for immediate start in censored networks

#define MyAppName "huntercensor"
#define MyAppVersion "1.4.0"
#define MyAppPublisher "Hunter Project"
#define MyAppExeName "huntercensor.exe"
#define MyStagingDir "D:\projects\v2ray\pythonProject1\hunter\installer\staging"

[Setup]
AppId={{B7E2A8F1-4D3C-4E5F-9A1B-2C3D4E5F6A7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\huntercensor
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=D:\projects\v2ray\pythonProject1\hunter\installer\output
OutputBaseFilename=huntercensor-Setup-v{#MyAppVersion}
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
MinVersion=6.1
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=huntercensor - Anti-Censorship Discovery Tool
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel1=huntercensor Setup
WelcomeLabel2=This will install huntercensor v{#MyAppVersion} on your computer.%n%nhuntercensor helps you discover and validate censorship-resistant proxy configs with:%n%n- Sing-box-only proxy runtime and mixed local listener (SOCKS + HTTP)%n- Redesigned home screen with scan activity, full, Telegram-only, and history sections%n- Pre-loaded database and balancer caches%n- Offline-first censorship tooling%n%nNo administrator access required.

[Tasks]
Name: "desktopicon"; Description: "Create Desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "startupicon"; Description: "Start huntercensor at Windows startup"; GroupDescription: "Startup:"

[Files]
; Native GUI application
Source: "{#MyStagingDir}\huntercensor.exe"; DestDir: "{app}"; DestName: "huntercensor.exe"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyStagingDir}\hountersansor.exe"; DestDir: "{app}"; DestName: "huntercensor.exe"; Flags: ignoreversion skipifsourcedoesntexist

; CLI + proxy cores in bin\ (CLI is statically linked, no DLL deps)
Source: "{#MyStagingDir}\bin\sing-box.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
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
Source: "{#MyStagingDir}\runtime\HUNTER_config_db_export.txt"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\sources_manager.tsv"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\source_history.tsv"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist
Source: "{#MyStagingDir}\runtime\hunter_config.json"; DestDir: "{app}\runtime"; Flags: onlyifdoesntexist

[InstallDelete]
Type: files; Name: "{app}\hountersansor.exe"

[Icons]
Name: "{group}\huntercensor"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"; WorkingDir: "{app}"
Name: "{group}\Uninstall huntercensor"; Filename: "{uninstallexe}"
Name: "{autodesktop}\huntercensor"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Auto-start at login (user-level, no admin needed)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "huntercensor"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startupicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch huntercensor"; Flags: nowait postinstall skipifsilent; WorkingDir: "{app}"

[UninstallRun]
Filename: "taskkill"; Parameters: "/F /IM huntercensor.exe"; Flags: runhidden; RunOnceId: "KillDashboard"
Filename: "taskkill"; Parameters: "/F /IM hountersansor.exe"; Flags: runhidden; RunOnceId: "KillLegacyDashboard"
Filename: "taskkill"; Parameters: "/F /IM sing-box.exe"; Flags: runhidden; RunOnceId: "KillSingBox"
Filename: "taskkill"; Parameters: "/F /IM tor.exe"; Flags: runhidden; RunOnceId: "KillTor"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\runtime"
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\config"

[Code]
// Windows version check for compatibility
function IsWindows7OrLater(): Boolean;
var
  Version: TWindowsVersion;
begin
  GetWindowsVersionEx(Version);
  // Windows 7 is version 6.1 (major=6, minor=1)
  Result := (Version.Major > 6) or ((Version.Major = 6) and (Version.Minor >= 1));
end;

// Kill all Hunter-related processes before installation (for updates)
procedure KillRunningProcesses();
var
  ResultCode: Integer;
begin
  Log('Killing running huntercensor processes for update...');
  Exec('taskkill', '/F /IM huntercensor.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM hountersansor.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM sing-box.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill', '/F /IM tor.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Wait a moment for processes to fully terminate
  Sleep(2000);
end;

function InitializeSetup(): Boolean;
var
  Version: TWindowsVersion;
begin
  Result := True;
  
  // Check Windows version and log it
  GetWindowsVersionEx(Version);
  Log('Windows version detected: ' + IntToStr(Version.Major) + '.' + IntToStr(Version.Minor) + ' (Build ' + IntToStr(Version.Build) + ')');
  
  if not IsWindows7OrLater() then
  begin
    MsgBox('This application requires Windows 7 or later.' + #13#10 + 
           'Your current Windows version is not supported.', 
           mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;
  
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
