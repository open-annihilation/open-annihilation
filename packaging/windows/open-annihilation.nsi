# -*- coding: utf-8 -*-
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One installer for the four Windows packages, which also associates launch
# files (.oalaunch) with the game. makensis (NSIS 3) on macOS,
# Debian or Nix:
#
#   makensis -WX \
#     -DOA_PACKAGE=windows-x64|windows-x86|windows-arm64|windows-95 \
#     -DOA_PACKAGE_DIR=/absolute/path/open-annihilation-<version>-<package> \
#     -DOA_VERSION=<version> \
#     [-DOA_ICON=/absolute/path/open-annihilation.ico] \
#     packaging/windows/open-annihilation.nsi
#
# The defines come before the script name. OA_PACKAGE_DIR is the package
# folder the suite lays out, with no trailing slash and no spaces. The
# installer is written beside that folder, as
# open-annihilation-<version>-<package>-setup.exe, which puts it beside the
# package zip. Do not pass -NOCD unless -DOA_ICON is an absolute path: the
# default icon is branding/open-annihilation.ico, relative to this script,
# and makensis reads relative paths from the script's directory.
#
# windows-95 is ANSI (Unicode false). The other three are Unicode. NSIS 3
# defines NSIS_CHAR_SIZE and 2.46 does not, so a 2.46 compile never sees the
# Unicode instruction (2.46 is ANSI already) and is refused for the other
# packages. Two things still differ if the Windows 95 installer has to be
# built with 2.46. The icon's images are PNG, which only NSIS 3 accepts, so
# pass -DOA_ICON with an icon of BMP images. And 2.46 reads both sides of a
# !if, so it would open the NSIS 3 headers included for the other packages
# (x64.nsh, WinVer.nsh) and the !if /FileExists checks. NSIS 3 does not read
# the side it skips, which is what this script relies on.

!ifndef OA_PACKAGE
  !error "Pass -DOA_PACKAGE=windows-x64, windows-x86, windows-arm64 or windows-95."
!endif
!ifndef OA_PACKAGE_DIR
  !error "Pass -DOA_PACKAGE_DIR as the absolute package folder, with no trailing slash."
!endif
!ifndef OA_VERSION
  !error "Pass -DOA_VERSION as the version string."
!endif
!if "${OA_PACKAGE_DIR}" == ""
  !error "OA_PACKAGE_DIR is empty."
!endif
!if "${OA_VERSION}" == ""
  !error "OA_VERSION is empty."
!endif

!if "${OA_PACKAGE}" == "windows-x64"
  !define OA_64BIT
  !define OA_REFUSE_32BIT
!endif
!if "${OA_PACKAGE}" == "windows-arm64"
  !define OA_64BIT
  !define OA_REFUSE_NOT_ARM64
!endif
!if "${OA_PACKAGE}" == "windows-x86"
  !define OA_REFUSE_BEFORE_XPSP3
!endif
!if "${OA_PACKAGE}" == "windows-95"
  !define OA_ANSI
!endif
!if "${OA_PACKAGE}" != "windows-x64"
  !if "${OA_PACKAGE}" != "windows-arm64"
    !if "${OA_PACKAGE}" != "windows-x86"
      !if "${OA_PACKAGE}" != "windows-95"
        !error "OA_PACKAGE must be windows-x64, windows-x86, windows-arm64 or windows-95."
      !endif
    !endif
  !endif
!endif

!ifdef NSIS_CHAR_SIZE
  !ifdef OA_ANSI
    Unicode false
  !else
    Unicode true
  !endif
!else
  !ifndef OA_ANSI
    !error "windows-x64, windows-x86 and windows-arm64 need NSIS 3."
  !endif
!endif

SetCompressor /SOLID lzma

!include MUI2.nsh
!include LogicLib.nsh
!include FileFunc.nsh
!include Sections.nsh
!ifdef OA_64BIT
  !include x64.nsh
!endif
!ifdef OA_REFUSE_BEFORE_XPSP3
  !include WinVer.nsh
!endif

!define OA_NAME "Open Annihilation"
!define OA_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Open Annihilation"
!ifndef OA_ICON
  !define OA_ICON "../../branding/open-annihilation.ico"
!endif
!ifdef OA_64BIT
  !define OA_DEFAULT_DIR "$PROGRAMFILES64\Open Annihilation"
!else
  !define OA_DEFAULT_DIR "$PROGRAMFILES\Open Annihilation"
!endif
!define OA_SM_GAME "$SMPROGRAMS\Open Annihilation\Open Annihilation.lnk"
!define OA_SM_UNINST "$SMPROGRAMS\Open Annihilation\Uninstall Open Annihilation.lnk"
!define OA_DESKTOP_LNK "$DESKTOP\Open Annihilation.lnk"
# A launch file (.oalaunch) opens the game in that launch's battle room, as
# open-annihilation.exe -c FILE does. The installer associates it for everyone
# who uses this computer; the game's own registration of the six types is per
# user, at run time, and a player who turns Settings' Launch files switch off
# hides this association from themselves alone. These are the names
# src/platform/file-types uses. The uninstaller removes both: the association
# for everyone, and the registration of the account that uninstalls while it
# names this install's program (un.OA_WithdrawUserType).
!define OA_USER_CLASSES_KEY "Software\Classes"
!define OA_LAUNCH_EXTENSION_KEY "Software\Classes\.oalaunch"
!define OA_LAUNCH_PROGRAM_ID "OpenAnnihilation.Launch"
!define OA_LAUNCH_PROGRAM_KEY "Software\Classes\OpenAnnihilation.Launch"
!define OA_LAUNCH_MIME_TYPE "application/x-oalaunch"
# SHChangeNotify's SHCNE_ASSOCCHANGED: Explorer shows the icon and the opener at once.
!define OA_SHELL_ASSOCIATIONS_CHANGED 0x08000000

# The named files of every package. A missing one stops the compile.
!macro OA_Require _relative
  !if /FileExists "${OA_PACKAGE_DIR}/${_relative}"
  !else
    !error "The package is missing ${_relative}."
  !endif
!macroend
!define OA_Require "!insertmacro OA_Require"
${OA_Require} "open-annihilation.exe"
${OA_Require} "oa-intro.exe"
${OA_Require} "oa-tool.exe"
${OA_Require} "LICENSE"
${OA_Require} "ATTRIBUTIONS.md"
${OA_Require} "brains/classic.oabrain"
${OA_Require} "languages/interface.tdf"
${OA_Require} "registries/coreprime.yaml"
${OA_Require} "licenses/zlib-LICENSE.txt"

Name "${OA_NAME}"
OutFile "${OA_PACKAGE_DIR}-setup.exe"
InstallDir "${OA_DEFAULT_DIR}"
!ifndef OA_64BIT
  InstallDirRegKey HKLM "${OA_UNINST_KEY}" "InstallLocation"
!endif
RequestExecutionLevel admin
Icon "${OA_ICON}"
UninstallIcon "${OA_ICON}"
Caption "${OA_NAME} ${OA_VERSION} Setup"
UninstallCaption "${OA_NAME} Uninstall"
BrandingText "${OA_NAME} ${OA_VERSION}"

!define MUI_ICON "${OA_ICON}"
!define MUI_UNICON "${OA_ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs Open Annihilation for everyone who uses this computer.$\r$\n$\r$\nYour saved games, screenshots, films, recordings and mods stay in the Open Annihilation folder in Documents. Removing the game later leaves that folder where it is."
!define MUI_COMPONENTSPAGE_TEXT_TOP "Open Annihilation is always installed. Leave the desktop shortcut ticked unless you do not want one."
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_DIRECTORYPAGE_TEXT_TOP "Open Annihilation installs into this folder. A copy already installed is replaced in this folder."
!define MUI_FINISHPAGE_TEXT "Open Annihilation is installed.$\r$\n$\r$\nYour saved games stay in the Open Annihilation folder in Documents."
!define MUI_FINISHPAGE_RUN "$INSTDIR\open-annihilation.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Open Annihilation"
!define MUI_FINISHPAGE_NOREBOOTSUPPORT

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${OA_PACKAGE_DIR}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var DesktopWasInstalled

Function .onInit
  SetShellVarContext all

  !ifdef OA_REFUSE_32BIT
    ${IfNot} ${RunningX64}
      DetailPrint "This installer is for 64-bit Windows. It does not run on 32-bit Windows."
      MessageBox MB_OK|MB_ICONSTOP "This installer is for 64-bit Windows. It does not run on 32-bit Windows." /SD IDOK
      Quit
    ${EndIf}
  !endif

  !ifdef OA_REFUSE_NOT_ARM64
    ${IfNot} ${IsNativeARM64}
      DetailPrint "This installer is for Windows on ARM. It does not run on this computer."
      MessageBox MB_OK|MB_ICONSTOP "This installer is for Windows on ARM. It does not run on this computer." /SD IDOK
      Quit
    ${EndIf}
  !endif

  !ifdef OA_REFUSE_BEFORE_XPSP3
    ${IfNot} ${AtLeastWinXP}
      DetailPrint "This installer needs Windows XP Service Pack 3 or newer."
      MessageBox MB_OK|MB_ICONSTOP "This installer needs Windows XP Service Pack 3 or newer." /SD IDOK
      Quit
    ${EndIf}
    ${If} ${IsWinXP}
    ${AndIfNot} ${AtLeastServicePack} 3
      DetailPrint "This installer needs Windows XP Service Pack 3 or newer."
      MessageBox MB_OK|MB_ICONSTOP "This installer needs Windows XP Service Pack 3 or newer." /SD IDOK
      Quit
    ${EndIf}
  !endif

  !ifdef OA_64BIT
    SetRegView 64
  !endif

  StrCpy $DesktopWasInstalled ""
  ClearErrors
  ReadRegDWORD $DesktopWasInstalled HKLM "${OA_UNINST_KEY}" "DesktopShortcut"
  ${If} ${Errors}
    StrCpy $DesktopWasInstalled ""
  ${EndIf}
  ClearErrors

  !ifdef OA_64BIT
    ReadRegStr $0 HKLM "${OA_UNINST_KEY}" "InstallLocation"
    StrCmp $0 "" oa_keep_dir
    StrCmp $INSTDIR "${OA_DEFAULT_DIR}" 0 oa_keep_dir
      StrCpy $INSTDIR $0
    oa_keep_dir:
    ClearErrors
  !endif

  Call oa_SilentShortcut
FunctionEnd

Section "Open Annihilation" SecProgram
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${OA_PACKAGE_DIR}/open-annihilation.exe"
  File "${OA_PACKAGE_DIR}/oa-intro.exe"
  File "${OA_PACKAGE_DIR}/oa-tool.exe"
  !if /FileExists "${OA_PACKAGE_DIR}/oa-update.exe"
    File "${OA_PACKAGE_DIR}/oa-update.exe"
  !endif
  File "${OA_PACKAGE_DIR}/LICENSE"
  File "${OA_PACKAGE_DIR}/ATTRIBUTIONS.md"

  SetOutPath "$INSTDIR\licenses"
  File /r "${OA_PACKAGE_DIR}/licenses/*.*"

  SetOutPath "$INSTDIR\fonts"
  File /r "${OA_PACKAGE_DIR}/fonts/*.*"

  SetOutPath "$INSTDIR\brains"
  File "${OA_PACKAGE_DIR}/brains/classic.oabrain"

  SetOutPath "$INSTDIR\languages"
  File "${OA_PACKAGE_DIR}/languages/interface.tdf"

  SetOutPath "$INSTDIR\registries"
  File /r "${OA_PACKAGE_DIR}/registries/*.*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Open Annihilation"
  CreateShortcut "${OA_SM_GAME}" "$INSTDIR\open-annihilation.exe" "" "$INSTDIR\open-annihilation.exe" 0
  CreateShortcut "${OA_SM_UNINST}" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  DeleteRegKey HKLM "${OA_UNINST_KEY}"
  WriteRegStr HKLM "${OA_UNINST_KEY}" "DisplayName" "${OA_NAME}"
  WriteRegStr HKLM "${OA_UNINST_KEY}" "DisplayVersion" "${OA_VERSION}"
  WriteRegStr HKLM "${OA_UNINST_KEY}" "Publisher" "Open Annihilation"
  WriteRegStr HKLM "${OA_UNINST_KEY}" "DisplayIcon" "$INSTDIR\open-annihilation.exe"
  WriteRegStr HKLM "${OA_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${OA_UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKLM "${OA_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${OA_UNINST_KEY}" "EstimatedSize" $0
  WriteRegDWORD HKLM "${OA_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${OA_UNINST_KEY}" "NoRepair" 1

  WriteRegStr HKLM "${OA_LAUNCH_EXTENSION_KEY}" "" "${OA_LAUNCH_PROGRAM_ID}"
  WriteRegStr HKLM "${OA_LAUNCH_EXTENSION_KEY}" "Content Type" "${OA_LAUNCH_MIME_TYPE}"
  WriteRegStr HKLM "${OA_LAUNCH_EXTENSION_KEY}\OpenWithProgids" "${OA_LAUNCH_PROGRAM_ID}" ""
  WriteRegStr HKLM "${OA_LAUNCH_PROGRAM_KEY}" "" "Open Annihilation launch file"
  WriteRegStr HKLM "${OA_LAUNCH_PROGRAM_KEY}\DefaultIcon" "" "$INSTDIR\open-annihilation.exe,0"
  WriteRegStr HKLM "${OA_LAUNCH_PROGRAM_KEY}\shell\open\command" "" '"$INSTDIR\open-annihilation.exe" -c "%1"'
  System::Call 'shell32::SHChangeNotify(i ${OA_SHELL_ASSOCIATIONS_CHANGED}, i 0, i 0, i 0)'
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "${OA_DESKTOP_LNK}" "$INSTDIR\open-annihilation.exe" "" "$INSTDIR\open-annihilation.exe" 0
  WriteRegDWORD HKLM "${OA_UNINST_KEY}" "DesktopShortcut" 1
SectionEnd

; A silent install over a copy already there keeps the recorded shortcut.
; A missing value and an explicit 0 both leave the shortcut off. A first
; install, with no InstallLocation yet, keeps the shortcut on. The section
; index exists only after SecDesktop, so this is not inside .onInit.
Function oa_SilentShortcut
  ReadRegStr $0 HKLM "${OA_UNINST_KEY}" "InstallLocation"
  IfSilent 0 oa_shortcut_choice_done
  StrCmp $0 "" oa_shortcut_choice_done
  StrCmp $DesktopWasInstalled "1" oa_shortcut_choice_done
    !insertmacro UnselectSection ${SecDesktop}
  oa_shortcut_choice_done:
  ClearErrors
FunctionEnd

Function .onInstSuccess
  ${IfNot} ${SectionIsSelected} ${SecDesktop}
    WriteRegDWORD HKLM "${OA_UNINST_KEY}" "DesktopShortcut" 0
    StrCmp $DesktopWasInstalled "1" 0 oa_keep_desktop
      Delete "${OA_DESKTOP_LNK}"
    oa_keep_desktop:
  ${EndIf}
FunctionEnd

Function un.onInit
  SetShellVarContext all
  !ifdef OA_64BIT
    SetRegView 64
  !endif
FunctionEnd

# This install's program, as the uninstaller names it and in its short form.
Var ThisProgram
Var ThisProgramShort
# The file type un.OA_WithdrawUserType withdraws.
Var UserExtension
Var UserProgramId
Var UserMimeType

# Takes a path from the stack and leaves 1 there when it is this install's
# program, else 0. Letter case never matters, and an absolute path is also
# compared in its short form, so either spelling of the same file matches.
Function un.OA_IsThisProgram
  Exch $R0
  Push $R1
  Push $R2
  Push $R3
  StrCpy $R1 0
  StrCpy $R2 $R0 2 1
  StrCpy $R3 $R0 2
  ${If} $R0 == $ThisProgram
    StrCpy $R1 1
  ${ElseIf} $R2 == ":\"
  ${OrIf} $R3 == "\\"
    ClearErrors
    GetFullPathName /SHORT $R2 $R0
    ${IfNot} ${Errors}
    ${AndIf} $R2 == $ThisProgramShort
      StrCpy $R1 1
    ${EndIf}
  ${EndIf}
  ClearErrors
  StrCpy $R0 $R1
  Pop $R3
  Pop $R2
  Pop $R1
  Exch $R0
FunctionEnd

# Takes an open command from the stack and leaves there the program it
# names: the text between its first two quotes, as the game writes it
# ("<exe>" --open "%1"). Empty when the command does not start with a quote.
Function un.OA_ProgramOfCommand
  Exch $R0
  Push $R1
  Push $R2
  StrCpy $R1 $R0 1
  ${If} $R1 != '"'
    StrCpy $R0 ""
  ${Else}
    StrCpy $R2 1
    ${Do}
      StrCpy $R1 $R0 1 $R2
      ${If} $R1 == '"'
      ${OrIf} $R1 == ""
        ${ExitDo}
      ${EndIf}
      IntOp $R2 $R2 + 1
    ${Loop}
    ${If} $R1 == '"'
      IntOp $R2 $R2 - 1
      StrCpy $R0 $R0 $R2 1
    ${Else}
      StrCpy $R0 ""
    ${EndIf}
  ${EndIf}
  Pop $R2
  Pop $R1
  Exch $R0
FunctionEnd

# Withdraws one file type of the game's own registration
# (src/platform/file-types) for the account that uninstalls: the six values
# a withdrawal in the game removes, under HKEY_CURRENT_USER\Software\Classes,
# while they still belong to this install. The program identifier's open
# command and icon go while they name this install's program. Its name, its
# keys (leaf first, and each only once empty) and the extension's three
# values (the OpenWithProgids value named for it, and the (Default) and
# Content Type while they are still the game's program identifier and MIME
# type) go only when the command or the icon named this install's program
# and neither names another: a copy of the game unzipped elsewhere, or
# another program, keeps its registration.
Function un.OA_WithdrawUserType
  StrCpy $5 "${OA_USER_CLASSES_KEY}\$UserProgramId"
  StrCpy $6 "${OA_USER_CLASSES_KEY}\$UserExtension"
  # What the open command ($7) and the icon ($8) name: 0 nothing, 1 this
  # install's program, 2 another.
  StrCpy $7 0
  ClearErrors
  ReadRegStr $0 HKCU "$5\shell\open\command" ""
  ${IfNot} ${Errors}
    Push $0
    Call un.OA_ProgramOfCommand
    Call un.OA_IsThisProgram
    Pop $1
    IntOp $7 2 - $1
  ${EndIf}
  StrCpy $8 0
  ClearErrors
  ReadRegStr $0 HKCU "$5\DefaultIcon" ""
  ${IfNot} ${Errors}
    # The game's icon is "<exe>,0".
    StrCpy $1 $0 "" -2
    StrCpy $2 ""
    ${If} $1 == ",0"
      StrCpy $2 $0 -2
    ${EndIf}
    Push $2
    Call un.OA_IsThisProgram
    Pop $1
    IntOp $8 2 - $1
  ${EndIf}
  ClearErrors
  ${If} $7 == 1
    DeleteRegValue HKCU "$5\shell\open\command" ""
  ${EndIf}
  ${If} $8 == 1
    DeleteRegValue HKCU "$5\DefaultIcon" ""
  ${EndIf}
  ${If} $7 == 1
  ${OrIf} $8 == 1
    ${If} $7 != 2
    ${AndIf} $8 != 2
      DeleteRegValue HKCU "$5" ""
      DeleteRegKey /ifempty HKCU "$5\shell\open\command"
      DeleteRegKey /ifempty HKCU "$5\shell\open"
      DeleteRegKey /ifempty HKCU "$5\shell"
      DeleteRegKey /ifempty HKCU "$5\DefaultIcon"
      DeleteRegKey /ifempty HKCU "$5"
      DeleteRegValue HKCU "$6\OpenWithProgids" "$UserProgramId"
      ClearErrors
      ReadRegStr $0 HKCU "$6" ""
      ${IfNot} ${Errors}
      ${AndIf} $0 S== $UserProgramId
        DeleteRegValue HKCU "$6" ""
      ${EndIf}
      ClearErrors
      ReadRegStr $0 HKCU "$6" "Content Type"
      ${IfNot} ${Errors}
      ${AndIf} $0 S== $UserMimeType
        DeleteRegValue HKCU "$6" "Content Type"
      ${EndIf}
    ${EndIf}
  ${EndIf}
  ClearErrors
FunctionEnd

!macro OA_WithdrawUserType _EXTENSION _PROGRAM_ID _MIME_TYPE
  StrCpy $UserExtension "${_EXTENSION}"
  StrCpy $UserProgramId "${_PROGRAM_ID}"
  StrCpy $UserMimeType "${_MIME_TYPE}"
  Call un.OA_WithdrawUserType
!macroend
!define OA_WithdrawUserType "!insertmacro OA_WithdrawUserType"

Section "Uninstall"
  # The game's own registration of the seven file types, for the account that
  # uninstalls, while it names this install's program: first, while the
  # program is still there to be named in its short form too. The extensions
  # and program identifiers are those of src/platform/file-types.
  StrCpy $ThisProgram "$INSTDIR\open-annihilation.exe"
  ClearErrors
  GetFullPathName /SHORT $ThisProgramShort $ThisProgram
  ${If} ${Errors}
  ${OrIf} $ThisProgramShort == ""
    StrCpy $ThisProgramShort $ThisProgram
  ${EndIf}
  ClearErrors
  ${OA_WithdrawUserType} ".oamod" "OpenAnnihilation.Mod" "application/x-oamod"
  ${OA_WithdrawUserType} ".oalang" "OpenAnnihilation.Language" "application/x-oalang"
  ${OA_WithdrawUserType} ".oamap" "OpenAnnihilation.MapPack" "application/x-oamap"
  ${OA_WithdrawUserType} ".oamut" "OpenAnnihilation.Mutator" "application/x-oamut"
  ${OA_WithdrawUserType} ".oabrain" "OpenAnnihilation.Brain" "application/x-oabrain"
  ${OA_WithdrawUserType} ".oakeys" "OpenAnnihilation.KeyLayout" "application/x-oakeys"
  ${OA_WithdrawUserType} ".oareg" "OpenAnnihilation.Registry" "application/x-oareg"
  ${OA_WithdrawUserType} ".oalaunch" "${OA_LAUNCH_PROGRAM_ID}" "${OA_LAUNCH_MIME_TYPE}"

  # Removes the install folder's own files, the Start Menu folder and the
  # desktop shortcut this installer added. Never Documents, and never a game
  # copy, save or mod outside the install folder.
  ClearErrors
  ReadRegDWORD $0 HKLM "${OA_UNINST_KEY}" "DesktopShortcut"
  ${IfNot} ${Errors}
    StrCmp $0 "1" 0 oa_un_no_desktop
      Delete "${OA_DESKTOP_LNK}"
    oa_un_no_desktop:
  ${EndIf}
  ClearErrors

  Delete "${OA_SM_GAME}"
  Delete "${OA_SM_UNINST}"
  RMDir "$SMPROGRAMS\Open Annihilation"

  Delete "$INSTDIR\open-annihilation.exe"
  Delete "$INSTDIR\oa-intro.exe"
  Delete "$INSTDIR\oa-tool.exe"
  Delete "$INSTDIR\oa-update.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\ATTRIBUTIONS.md"
  Delete "$INSTDIR\brains\classic.oabrain"
  RMDir "$INSTDIR\brains"
  Delete "$INSTDIR\languages\interface.tdf"
  RMDir "$INSTDIR\languages"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\fonts"
  RMDir /r "$INSTDIR\registries"
  Delete "$INSTDIR\Uninstall.exe"
  DeleteRegKey HKLM "${OA_UNINST_KEY}"
  RMDir "$INSTDIR"

  # The launch file's association: the program identifier is the game's by its
  # name; the extension's values only while they still name the game, so a
  # program that took .oalaunch over keeps it.
  DeleteRegKey HKLM "${OA_LAUNCH_PROGRAM_KEY}"
  DeleteRegValue HKLM "${OA_LAUNCH_EXTENSION_KEY}\OpenWithProgids" "${OA_LAUNCH_PROGRAM_ID}"
  DeleteRegKey /ifempty HKLM "${OA_LAUNCH_EXTENSION_KEY}\OpenWithProgids"
  ClearErrors
  ReadRegStr $0 HKLM "${OA_LAUNCH_EXTENSION_KEY}" ""
  ${If} $0 == "${OA_LAUNCH_PROGRAM_ID}"
    DeleteRegValue HKLM "${OA_LAUNCH_EXTENSION_KEY}" ""
  ${EndIf}
  ReadRegStr $0 HKLM "${OA_LAUNCH_EXTENSION_KEY}" "Content Type"
  ${If} $0 == "${OA_LAUNCH_MIME_TYPE}"
    DeleteRegValue HKLM "${OA_LAUNCH_EXTENSION_KEY}" "Content Type"
  ${EndIf}
  DeleteRegKey /ifempty HKLM "${OA_LAUNCH_EXTENSION_KEY}"
  ClearErrors

  # The override of the user who uninstalls: while Settings' Launch files
  # switch is off, the game sets .oalaunch's (Default) under this user's own
  # classes to an empty text, which names no program and hides the association
  # above from this user. With that association gone the override has nothing
  # left to hide. It is the game's only while it is that empty text: a program
  # identifier there, the player's or another program's, stays.
  ReadRegStr $0 HKCU "${OA_LAUNCH_EXTENSION_KEY}" ""
  ${IfNot} ${Errors}
  ${AndIf} $0 == ""
    DeleteRegValue HKCU "${OA_LAUNCH_EXTENSION_KEY}" ""
  ${EndIf}
  ClearErrors
  System::Call 'shell32::SHChangeNotify(i ${OA_SHELL_ASSOCIATIONS_CHANGED}, i 0, i 0, i 0)'
SectionEnd
