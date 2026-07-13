; Clipster installer (Inno Setup 6). Built by the release workflow, which
; passes /DAppVersion=<x.y.z> and /DStageDir=<staged app folder>.
; Local build: iscc installer\clipster.iss after staging files manually.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\stage\Clipster"
#endif

[Setup]
; Never change AppId: it is how upgrades find the existing install.
AppId={{8F4E9A21-6C37-4B5D-9E12-D3A47C10BE55}
AppName=Clipster
AppVersion={#AppVersion}
AppPublisher=Plezo
AppPublisherURL=https://github.com/Plezo/Clipster
AppSupportURL=https://github.com/Plezo/Clipster/issues
WizardStyle=modern
; Per-user install ({autopf} -> %LOCALAPPDATA%\Programs), no admin prompt.
PrivilegesRequired=lowest
DefaultDirName={autopf}\Clipster
DefaultGroupName=Clipster
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Clipster.exe
SetupIconFile=..\assets\clipster.ico
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\Clipster"; Filename: "{app}\Clipster.exe"
Name: "{autodesktop}\Clipster"; Filename: "{app}\Clipster.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Clipster.exe"; Description: "{cm:LaunchProgram,Clipster}"; Flags: nowait postinstall skipifsilent
