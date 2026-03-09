; Hunter Dashboard Setup Script for NSIS
; Requires NSIS 3.0 or later

!define APPNAME "Hunter Dashboard"
!define VERSION "1.0.0.0"
!define PUBLISHER "Hunter Project"
!define WEBSITE "https://github.com/bahmany/censorship_hunter"
!define LICENSE "license.txt"

; Basic configuration
Name "${APPNAME}"
OutFile "hunter_dashboard_setup.exe"
InstallDir "$PROGRAMFILES64\Hunter"
InstallDirRegKey HKLM "Software\Hunter\Dashboard" "InstallPath"
RequestExecutionLevel admin

; Interface settings
!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_ICON "hunter_icon.ico"
!define MUI_UNICON "hunter_icon.ico"

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

; Languages
!insertmacro MUI_LANGUAGE "English"

; Version information
VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "© 2026 ${PUBLISHER}"
VIAddVersionKey "FileDescription" "Hunter Anti-Censorship Dashboard Installer"
VIAddVersionKey "FileVersion" "${VERSION}"

; Installation sections
Section "Hunter Dashboard" SecDashboard

    SectionIn RO
    
    ; Set installation directory
    SetOutPath "$INSTDIR"
    
    ; Create runtime directory
    CreateDirectory "$INSTDIR\runtime"
    
    ; Main dashboard executable
    File "..\hunter_flutter_ui\build\windows\x64\runner\Release\hunter_dashboard.exe"
    
    ; C++ CLI and dependencies
    File "..\bin\hunter_cli.exe"
    File "..\bin\libcrypto-3-x64.dll"
    File "..\bin\libssl-3-x64.dll"
    File "..\bin\libcurl-4.dll"
    File "..\bin\libzstd.dll"
    File "..\bin\zlib1.dll"
    File "..\bin\libssh2-1.dll"
    File "..\bin\libnghttp2-14.dll"
    File "..\bin\libnghttp3-9.dll"
    File "..\bin\libpsl-5.dll"
    File "..\bin\libidn2-0.dll"
    File "..\bin\libbrotlidec.dll"
    
    ; Proxy engines
    File "..\bin\xray.exe"
    File "..\bin\sing-box.exe"
    File "..\bin\mihomo-windows-amd64-compatible.exe"
    File "..\bin\tor.exe"
    
    ; Configuration files
    File "..\config\All_Configs_Sub.txt"
    File "..\config\all_extracted_configs.txt"
    File "..\config\sub.txt"
    
    ; Create uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    
    ; Registry entries
    WriteRegStr HKLM "Software\Hunter\Dashboard" "InstallPath" "$INSTDIR"
    WriteRegStr HKLM "Software\Hunter\Dashboard" "Version" "${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "DisplayName" "${APPNAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "Publisher" "${PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "URLInfoAbout" "${WEBSITE}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard" "NoRepair" 1
    
SectionEnd

Section "Visual C++ Redistributable" SecVCRedist
    
    ; Check if VC++ Redistributable is already installed
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{DA5E371C-6333-3D8A-A1CE-1C3B849911EF}" "Version"
    
    ${If} $0 == ""
        ; Download and install VC++ Redistributable
        DetailPrint "Downloading Visual C++ Redistributable..."
        NSISdl::download "https://aka.ms/vs/17/release/vc_redist.x64.exe" "$TEMP\vc_redist.x64.exe"
        Pop $0 ; Get return value
        ${If} $0 == "success"
            DetailPrint "Installing Visual C++ Redistributable..."
            ExecWait '"$TEMP\vc_redist.x64.exe" /quiet' $0
            DetailPrint "VC++ Redistributable installation completed with return code: $0"
        ${Else}
            DetailPrint "Failed to download VC++ Redistributable: $0"
        ${EndIf}
        Delete "$TEMP\vc_redist.x64.exe"
    ${Else}
        DetailPrint "Visual C++ Redistributable already installed"
    ${EndIf}
    
SectionEnd

Section "Start Menu Shortcuts" SecShortcuts
    
    CreateDirectory "$SMPROGRAMS\Hunter"
    CreateShortCut "$SMPROGRAMS\Hunter\Hunter Dashboard.lnk" "$INSTDIR\hunter_dashboard.exe" "" "$INSTDIR\hunter_dashboard.exe" 0 "" SW_SHOWNORMAL
    CreateShortCut "$SMPROGRAMS\Hunter\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    
SectionEnd

Section "Desktop Shortcut" SecDesktop
    
    CreateShortCut "$DESKTOP\Hunter Dashboard.lnk" "$INSTDIR\hunter_dashboard.exe" "" "$INSTDIR\hunter_dashboard.exe" 0 "" SW_SHOWNORMAL
    
SectionEnd

; Uninstaller section
Section "Uninstall"

    ; Remove files and directories
    Delete "$INSTDIR\hunter_dashboard.exe"
    Delete "$INSTDIR\hunter_cli.exe"
    Delete "$INSTDIR\libcrypto-3-x64.dll"
    Delete "$INSTDIR\libssl-3-x64.dll"
    Delete "$INSTDIR\libcurl-4.dll"
    Delete "$INSTDIR\libzstd.dll"
    Delete "$INSTDIR\zlib1.dll"
    Delete "$INSTDIR\libssh2-1.dll"
    Delete "$INSTDIR\libnghttp2-14.dll"
    Delete "$INSTDIR\libnghttp3-9.dll"
    Delete "$INSTDIR\libpsl-5.dll"
    Delete "$INSTDIR\libidn2-0.dll"
    Delete "$INSTdir\libbrotlidec.dll"
    Delete "$INSTDIR\xray.exe"
    Delete "$INSTDIR\sing-box.exe"
    Delete "$INSTDIR\mihomo-windows-amd64-compatible.exe"
    Delete "$INSTDIR\tor.exe"
    Delete "$INSTDIR\All_Configs_Sub.txt"
    Delete "$INSTDIR\all_extracted_configs.txt"
    Delete "$INSTDIR\sub.txt"
    Delete "$INSTDIR\Uninstall.exe"
    
    ; Remove directories
    RMDir /r "$INSTDIR\runtime"
    RMDir "$INSTDIR"
    
    ; Remove shortcuts
    Delete "$DESKTOP\Hunter Dashboard.lnk"
    Delete "$SMPROGRAMS\Hunter\Hunter Dashboard.lnk"
    Delete "$SMPROGRAMS\Hunter\Uninstall.lnk"
    RMDir "$SMPROGRAMS\Hunter"
    
    ; Remove registry entries
    DeleteRegKey HKLM "Software\Hunter\Dashboard"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\HunterDashboard"
    
SectionEnd

; Section descriptions
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDashboard} "Install Hunter Dashboard application with all required components"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecVCRedist} "Install Visual C++ Redistributable (required for C++ components)"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Create Start Menu shortcuts"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create Desktop shortcut"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; Callback functions
Function .onInit
    ; Check if already installed
    ReadRegStr $0 HKLM "Software\Hunter\Dashboard" "InstallPath"
    ${If} $0 != ""
        MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "${APPNAME} is already installed. $\n$\nClick OK to remove the previous version or Cancel to cancel this upgrade." IDOK uninst
        Abort
    ${EndIf}
    
    ; Uninstall previous version
    uninst:
        ClearErrors
        ExecWait '$0\Uninstall.exe _?=$INSTDIR'
    ${Unless} ${Errors}
        Delete '$0\Uninstall.exe'
        RMDir '$0'
    ${EndUnless}
FunctionEnd

Function un.onInit
    MessageBox MB_YESNO "Are you sure you want to completely remove ${APPNAME}?" IDYES NoAbort
    Abort
    NoAbort:
FunctionEnd
