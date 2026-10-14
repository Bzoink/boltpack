!ifndef BUILD_DIR
  !error "BUILD_DIR is required"
!endif
!ifndef CLI_BUILD_DIR
  !error "CLI_BUILD_DIR is required"
!endif
!ifndef PACKAGE_DIR
  !error "PACKAGE_DIR is required"
!endif
!ifndef RESOURCES_DIR
  !error "RESOURCES_DIR is required"
!endif
!ifndef BP_VERSION
  !error "BP_VERSION is required"
!endif
!ifndef BP_SETUP_SUFFIX
  !error "BP_SETUP_SUFFIX is required"
!endif
!ifndef BP_ARCH_LABEL
  !error "BP_ARCH_LABEL is required"
!endif

!define MULTIUSER_MUI
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODE_INSTDIR "BoltPack"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "Software\BoltPack"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallDir"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "Software\BoltPack"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallMode"

!ifdef BP_USE_PROGRAMFILES64
  !define MULTIUSER_USE_PROGRAMFILES64
!endif

!include "MultiUser.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"
!include "x64.nsh"
!include "StrFunc.nsh"

${Using:StrFunc} StrStr
${Using:StrFunc} StrRep
${Using:StrFunc} UnStrRep

!define PRODUCT_NAME "BoltPack"
!define PRODUCT_REGKEY "Software\BoltPack"
!define UNINSTALL_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BoltPack"
!define CONTEXT_MENU_ROOT "Software\Classes\AllFilesystemObjects\shell\BoltPack"

Unicode true
Name "${PRODUCT_NAME} ${BP_VERSION} (${BP_ARCH_LABEL})"
OutFile "${BUILD_DIR}/boltpack-${BP_VERSION}-${BP_SETUP_SUFFIX}-setup.exe"
InstallDir "$LocalAppData\Programs\BoltPack"
CRCCheck on
XPStyle on
ShowInstDetails show
ShowUninstDetails show
SetCompressor /SOLID lzma
BrandingText "BoltPack ${BP_VERSION}"

!define MUI_ABORTWARNING
!define MUI_ICON "${RESOURCES_DIR}/boltpack_logo_rounded.ico"
!define MUI_UNICON "${RESOURCES_DIR}/boltpack_logo_rounded.ico"

VIProductVersion "1.0.0.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${BP_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_NAME} ${BP_ARCH_LABEL} installer"

Var StartMenuFolder

!insertmacro MUI_PAGE_WELCOME
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_LICENSE "${PACKAGE_DIR}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
!ifdef BP_REQUIRE_X64
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "This installer requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
!endif
  !insertmacro MULTIUSER_INIT
  ${If} $MultiUser.InstallMode == "AllUsers"
    !ifdef BP_USE_PROGRAMFILES64
      StrCpy $INSTDIR "$PROGRAMFILES64\BoltPack"
    !else
      StrCpy $INSTDIR "$PROGRAMFILES\BoltPack"
    !endif
  ${Else}
    StrCpy $INSTDIR "$LocalAppData\Programs\BoltPack"
  ${EndIf}
FunctionEnd

Function un.onInit
!ifdef BP_REQUIRE_X64
  ${If} ${RunningX64}
    SetRegView 64
  ${EndIf}
!endif
  !insertmacro MULTIUSER_UNINIT
FunctionEnd

Function AddInstallDirToPath
  Push $R0
  Push $R1
  Push $R2

  ${If} $MultiUser.InstallMode == "AllUsers"
    ReadRegStr $R0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  ${Else}
    ReadRegStr $R0 HKCU "Environment" "Path"
  ${EndIf}

  StrCpy $R1 ";$R0;"
  ${StrStr} $R2 "$R1" ";$INSTDIR;"
  ${If} $R2 == ""
    ${If} $R0 == ""
      StrCpy $R0 "$INSTDIR"
    ${Else}
      StrCpy $R0 "$R0;$INSTDIR"
    ${EndIf}

    ${If} $MultiUser.InstallMode == "AllUsers"
      WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$R0"
    ${Else}
      WriteRegExpandStr HKCU "Environment" "Path" "$R0"
    ${EndIf}

    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}

  Pop $R2
  Pop $R1
  Pop $R0
FunctionEnd

Function un.RemoveInstallDirFromPath
  Push $R0
  Push $R1
  Push $R2

  ${If} $MultiUser.InstallMode == "AllUsers"
    ReadRegStr $R0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  ${Else}
    ReadRegStr $R0 HKCU "Environment" "Path"
  ${EndIf}

  ${If} $R0 != ""
    StrCpy $R1 ";$R0;"
    ${UnStrRep} $R2 "$R1" ";$INSTDIR;" ";"

    ${If} $R2 == ";"
      StrCpy $R2 ""
    ${Else}
      StrCpy $R0 $R2 1
      ${If} $R0 == ";"
        StrCpy $R2 $R2 "" 1
      ${EndIf}

      StrLen $R0 $R2
      ${If} $R0 > 0
        IntOp $R0 $R0 - 1
        StrCpy $R1 $R2 1 $R0
        ${If} $R1 == ";"
          StrCpy $R2 $R2 $R0
        ${EndIf}
      ${EndIf}
    ${EndIf}

    ${If} $MultiUser.InstallMode == "AllUsers"
      ${If} $R2 == ""
        DeleteRegValue HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
      ${Else}
        WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$R2"
      ${EndIf}
    ${Else}
      ${If} $R2 == ""
        DeleteRegValue HKCU "Environment" "Path"
      ${Else}
        WriteRegExpandStr HKCU "Environment" "Path" "$R2"
      ${EndIf}
    ${EndIf}

    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}

  Pop $R2
  Pop $R1
  Pop $R0
FunctionEnd

Section "BoltPack files" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"

  File /oname=boltpack-gui.exe "${BUILD_DIR}/boltpack-gui.exe"
  File /oname=boltpack.exe "${CLI_BUILD_DIR}/boltpack.exe"
  File /oname=README.txt "${PACKAGE_DIR}/README.txt"
  File /oname=RELEASE-NOTES.txt "${PACKAGE_DIR}/RELEASE-NOTES.txt"
  File /oname=LICENSE.txt "${PACKAGE_DIR}/LICENSE"

  StrCpy $StartMenuFolder "$SMPROGRAMS\BoltPack"
  CreateDirectory "$StartMenuFolder"
  CreateShortcut "$StartMenuFolder\BoltPack GUI.lnk" "$INSTDIR\boltpack-gui.exe"
  CreateShortcut "$StartMenuFolder\BoltPack CLI.lnk" "$INSTDIR\boltpack.exe"
  CreateShortcut "$StartMenuFolder\README.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$StartMenuFolder\Release Notes.lnk" "$INSTDIR\RELEASE-NOTES.txt"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$StartMenuFolder\Uninstall BoltPack.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr SHCTX "${PRODUCT_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr SHCTX "${PRODUCT_REGKEY}" "InstallMode" "$MultiUser.InstallMode"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "DisplayName" "${PRODUCT_NAME} ${BP_VERSION}"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "DisplayVersion" "${BP_VERSION}"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "Publisher" "BoltPack"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "DisplayIcon" "$INSTDIR\boltpack-gui.exe"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr SHCTX "${UNINSTALL_REGKEY}" "QuietUninstallString" "$INSTDIR\Uninstall.exe /S"
  WriteRegDWORD SHCTX "${UNINSTALL_REGKEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${UNINSTALL_REGKEY}" "NoRepair" 1
SectionEnd

Section /o "Add BoltPack command to PATH" SecPath
  Call AddInstallDirToPath
SectionEnd

Section /o "Windows Explorer context menu" SecContextMenu
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}" "MUIVerb" "BoltPack"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}" "Icon" "$INSTDIR\boltpack-gui.exe"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}" "MultiSelectModel" "Player"

  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\001create_fluff" "MUIVerb" "Create archive (fluff)"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\001create_fluff" "Icon" "$INSTDIR\boltpack-gui.exe"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\001create_fluff\command" "" "$\"$INSTDIR\boltpack-gui.exe$\" --shell-create-fluff %*"

  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\002create_boltlz" "MUIVerb" "Create archive (boltlz)"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\002create_boltlz" "Icon" "$INSTDIR\boltpack-gui.exe"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\002create_boltlz\command" "" "$\"$INSTDIR\boltpack-gui.exe$\" --shell-create-boltlz %*"

  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\003extract" "MUIVerb" "Extract archive"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\003extract" "Icon" "$INSTDIR\boltpack-gui.exe"
  WriteRegStr SHCTX "${CONTEXT_MENU_ROOT}\shell\003extract\command" "" "$\"$INSTDIR\boltpack-gui.exe$\" --shell-extract $\"%1$\""
SectionEnd

Section "Uninstall"
  Call un.RemoveInstallDirFromPath

  Delete "$SMPROGRAMS\BoltPack\BoltPack GUI.lnk"
  Delete "$SMPROGRAMS\BoltPack\BoltPack CLI.lnk"
  Delete "$SMPROGRAMS\BoltPack\README.lnk"
  Delete "$SMPROGRAMS\BoltPack\Release Notes.lnk"
  Delete "$SMPROGRAMS\BoltPack\Uninstall BoltPack.lnk"
  RMDir "$SMPROGRAMS\BoltPack"

  Delete "$INSTDIR\boltpack-gui.exe"
  Delete "$INSTDIR\boltpack.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\RELEASE-NOTES.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey SHCTX "${UNINSTALL_REGKEY}"
  DeleteRegKey SHCTX "${PRODUCT_REGKEY}"
  DeleteRegKey SHCTX "${CONTEXT_MENU_ROOT}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "Install the Windows GUI, CLI, documentation files, Start Menu shortcuts, and the uninstaller."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} "Add the installation directory to PATH for the selected install scope."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecContextMenu} "Add Explorer context-menu entries for creating BoltPack archives and extracting archives."
!insertmacro MUI_FUNCTION_DESCRIPTION_END
