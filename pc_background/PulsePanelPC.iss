; PulsePanel Sphere PC Agent - one-time setup, then no desktop UI.
; Build with Inno Setup 6 on Windows after PyInstaller generates dist/*.exe.
#define MyAppVersion "0.7.0"
[Setup]
AppId={{DC480E09-2413-4027-938C-4EEC7FA13053}
AppName=PulsePanel Sphere - PC Background
AppVersion={#MyAppVersion}
DefaultDirName={localappdata}\PulsePanelSpherePC
DefaultGroupName=PulsePanel Sphere
OutputDir=..\dist\installer
OutputBaseFilename=PulsePanelPC_Installer_v0.7
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
WizardStyle=modern
UninstallDisplayName=PulsePanel Sphere - PC Background
CloseApplications=force

[Languages]
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"

[Files]
Source: "..\dist\PulsePanelPC_Background.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\PulsePanelPC_Setup.exe"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PulsePanelSpherePC"; ValueData: """{app}\PulsePanelPC_Background.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\PulsePanelPC_Setup.exe"; Parameters: "--pair"; Description: "Jednorazowo sparuj monitorek z PC (kod z ekranu)"; Flags: postinstall waituntilterminated skipifsilent
Filename: "{app}\PulsePanelPC_Background.exe"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM PulsePanelPC_Background.exe"; Flags: runhidden; RunOnceId: "StopPulsePanel"
