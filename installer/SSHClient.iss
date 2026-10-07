#define AppName "SSHClient"
#define AppVersion "0.3"
#define BuildDir "..\cmake-build-release"

[Setup]
AppId={{D1F34587-410B-4F44-847F-AE0AFB70EB70}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
DefaultDirName={localappdata}\Programs\SSHClient
DefaultGroupName=SSHClient
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=SSHClient-Setup-{#AppVersion}
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\SSHClient.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "其他任务"; Flags: unchecked

[Files]
Source: "{#BuildDir}\SSHClient.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libstdc++-6.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\licenses\libssh2.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#BuildDir}\licenses\gcc-gplv3.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#BuildDir}\licenses\gcc-runtime-exception.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#BuildDir}\licenses\winpthreads.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#BuildDir}\licenses\mingw-crt.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion

[Icons]
Name: "{group}\SSHClient"; Filename: "{app}\SSHClient.exe"
Name: "{autodesktop}\SSHClient"; Filename: "{app}\SSHClient.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\SSHClient.exe"; Description: "启动 SSHClient"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\SSHClient"
Type: filesandordirs; Name: "{localappdata}\SSHClient"
