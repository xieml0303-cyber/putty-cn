; PuTTY 中文版安装包脚本
; 编译：ISCC.exe installer\putty-cn.iss
; 产物：installer\output\putty-cn-0.85-cn3-setup.exe

#define MyAppName "PuTTY 中文版"
#define MyAppVersion "0.85-cn3"
#define MyAppPublisher "putty-cn 项目"
#define MyAppURL "https://github.com/xieml0303-cyber/putty-cn"
#define MyAppExeName "putty.exe"

[Setup]
AppId={{A3F1C9D2-5B7E-4C61-9D8A-2E4F6B1C7A30}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\Programs\PuTTY-cn
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=yes
AllowNoIcons=yes
OutputDir=output
OutputBaseFilename=putty-cn-{#MyAppVersion}-setup
SetupIconFile=putty.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupLogging=yes
VersionInfoVersion=0.85.0.2
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion=0.85.0.2

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "..\dist\putty.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\puttygen.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\plink.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\pscp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\psftp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\pageant.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\puttytel.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENCE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\PuTTY"; Filename: "{app}\putty.exe"
Name: "{group}\PuTTYgen 密钥工具"; Filename: "{app}\puttygen.exe"
Name: "{group}\PLink 命令行 SSH"; Filename: "{app}\plink.exe"
Name: "{group}\PSCP 文件传输"; Filename: "{app}\pscp.exe"
Name: "{group}\PSFTP 文件传输"; Filename: "{app}\psftp.exe"
Name: "{group}\Pageant 密钥代理"; Filename: "{app}\pageant.exe"
Name: "{group}\PuTTYtel 纯 Telnet"; Filename: "{app}\puttytel.exe"
Name: "{group}\使用说明"; Filename: "{app}\README.md"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\PuTTY"; Filename: "{app}\putty.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
