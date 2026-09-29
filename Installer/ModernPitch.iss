; ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
; SPDX-License-Identifier: AGPL-3.0-or-later
;
; Windows installer (Inno Setup 6). Build the plugin in Release first, then:
;   ISCC.exe Installer\ModernPitch.iss /DAppVersion=0.3.2
; Output: Installer\Output\ModernPitch-<version>-Windows-x64-Setup.exe

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\ModernPitch_artefacts\Release\VST3"
#endif

#define AppName "ModernPitch"
#define AppPublisher "Soft Studio"
#define AppUrl "https://github.com/soft-studio-prod/ModernPitch"

[Setup]
; Keep this AppId for every release so updates replace the previous install.
AppId={{E610428A-7852-4C76-8FEE-96DA14AFBA0B}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
; The plugin goes to the standard VST3 folder; this folder only holds the
; licence, the readme and the uninstaller.
DefaultDirName={autopf}\Soft Studio\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename={#AppName}-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion} (VST3)

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Files]
Source: "{#BuildDir}\ModernPitch.vst3\*"; DestDir: "{commoncf64}\VST3\ModernPitch.vst3"; \
    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\ModernPitch.vst3"

[Messages]
english.FinishedLabel=ModernPitch has been installed to your VST3 folder (Common Files\VST3). Rescan plugins in your DAW to use it.
french.FinishedLabel=ModernPitch a été installé dans votre dossier VST3 (Common Files\VST3). Relancez un scan des plugins dans votre DAW pour l'utiliser.
