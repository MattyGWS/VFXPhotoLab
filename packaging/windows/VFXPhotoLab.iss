#define MyAppName "VFX Photo Lab"
#define MyAppPublisher "Matty Wyett-Simmonds and contributors"
#define MyAppURL "https://github.com/MattyGWS/VFXPhotoLab"
#define MyAppExeName "VFXPhotoLab.exe"
#define MyAppVersion GetEnv("VFXPL_VERSION")
#define MyWindowsVersion GetEnv("VFXPL_WINDOWS_VERSION")
#define MySourceDir GetEnv("VFXPL_SOURCE_DIR")
#define MyOutputDir GetEnv("VFXPL_OUTPUT_DIR")
#define MyOutputBaseFilename GetEnv("VFXPL_INSTALLER_BASENAME")

#if MyAppVersion == ""
  #error VFXPL_VERSION was not provided.
#endif
#if MyWindowsVersion == ""
  #error VFXPL_WINDOWS_VERSION was not provided.
#endif
#if MySourceDir == ""
  #error VFXPL_SOURCE_DIR was not provided.
#endif
#if MyOutputDir == ""
  #error VFXPL_OUTPUT_DIR was not provided.
#endif
#if MyOutputBaseFilename == ""
  #error VFXPL_INSTALLER_BASENAME was not provided.
#endif

[Setup]
AppId={{DA1A888C-8AF8-4C76-8B47-0FB0B1B4AB75}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
SetupIconFile=VFXPhotoLab.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=..\..\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyWindowsVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.vfxphoto"; ValueType: string; ValueName: ""; ValueData: "VFXPhotoLab.Project"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\VFXPhotoLab.Project"; ValueType: string; ValueName: ""; ValueData: "VFX Photo Lab Project"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\VFXPhotoLab.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCU; Subkey: "Software\Classes\VFXPhotoLab.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
