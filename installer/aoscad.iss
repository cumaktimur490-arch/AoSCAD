#define AppName "AoSCAD Studio"
#define AppVersion "0.3.0"
#define AppExe "aoscad-studio.exe"

[Setup]
AppId={{3DF9C3E3-AE56-416D-B3A9-9D6D229B305F}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=AoSCAD Contributors
AppPublisherURL=https://github.com/cumaktimur490-arch/AoSCAD
DefaultDirName={localappdata}\Programs\AoSCAD Studio
DefaultGroupName=AoSCAD Studio
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
CloseApplications=yes
RestartApplications=no
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
SetupIconFile=..\assets\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\release
OutputBaseFilename=AoSCAD-Studio-Setup-{#AppVersion}-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительные ярлыки:"; Flags: unchecked

[Files]
Source: "..\stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\AoSCAD Studio"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autoprograms}\AoSCAD Studio — Проверить обновления"; Filename: "{app}\aoscad-updater.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\AoSCAD Studio"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Запустить AoSCAD Studio"; Flags: nowait postinstall skipifsilent
