; RetroArch Windows Installer
; Built by the CI pipeline — invoke via:
;   makensis /DVERSION=<sha> /DARCH=arm64 /DBUNDLE_DIR=<path> installer.nsi

!ifndef VERSION
  !define VERSION "dev"
!endif

!ifndef ARCH
  !define ARCH "arm64"
!endif

!ifndef BUNDLE_DIR
  !define BUNDLE_DIR "..\..\retroarch-windows-${ARCH}-${VERSION}"
!endif

Name "RetroArch ${VERSION} (${ARCH})"
OutFile "retroarch-windows-${ARCH}-${VERSION}-installer.exe"
InstallDir "$PROGRAMFILES64\RetroArch"
InstallDirRegKey HKLM "Software\RetroArch" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

;--------------------------------
; Pages
Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

;--------------------------------
; Install section
Section "RetroArch (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  File /r "${BUNDLE_DIR}\*.*"

  ; Write install dir and uninstaller to registry
  WriteRegStr HKLM "Software\RetroArch" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Add Programs entry
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArch" \
    "DisplayName" "RetroArch"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArch" \
    "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArch" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArch" \
    "Publisher" "libretro"

  ; Start Menu shortcut
  CreateDirectory "$SMPROGRAMS\RetroArch"
  CreateShortcut "$SMPROGRAMS\RetroArch\RetroArch.lnk" "$INSTDIR\RetroArch.exe"
  CreateShortcut "$SMPROGRAMS\RetroArch\Uninstall.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

;--------------------------------
; Uninstall section
Section "Uninstall"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR"

  Delete "$SMPROGRAMS\RetroArch\RetroArch.lnk"
  Delete "$SMPROGRAMS\RetroArch\Uninstall.lnk"
  RMDir  "$SMPROGRAMS\RetroArch"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArch"
  DeleteRegKey HKLM "Software\RetroArch"
SectionEnd

