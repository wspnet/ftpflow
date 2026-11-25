; ============================================================================
; FTPFlow - Windows 安装程序脚本 (Inno Setup 6)
; 由 tools/build_installer.py 调用 ISCC.exe 编译生成 FTPFlow-Setup.exe
;
; 界面：标准 Windows 安装向导（欢迎 → 选择安装路径 → 快捷方式 → 安装 → 完成）
; 特点：简洁美观、支持选择安装位置、可选桌面/开始菜单快捷方式、一键卸载
; ============================================================================

#define MyAppName "FTPFlow"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "FTPFlow"
#define MyAppExeName "ftpflow.exe"

[Setup]
AppId={{6D9E2F3A-1B4C-4E7A-9A5D-3F8C2B7E6A91}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://www.example.com
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoProductName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
DefaultDirName={localappdata}\FTPFlow
DefaultGroupName=FTPFlow
DisableProgramGroupPage=no
OutputDir=..\dist
OutputBaseFilename=FTPFlow-Setup
SetupIconFile=..\assets\ftpflow.ico
UninstallDisplayIcon={app}\ftpflow.ico
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardImageFile=..\assets\wizard.bmp
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 中文界面 + 免管理员安装（默认装到用户目录，也可浏览选择其他位置）
ShowLanguageDialog=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce
Name: "quicklaunch"; Description: "创建开始菜单快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce

[Files]
; 主程序
Source: "..\dist\ftpflow\ftpflow.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\ftpflow\ftpflow.ico"; DestDir: "{app}"; Flags: ignoreversion
; Qt 运行时 DLL
Source: "..\dist\ftpflow\Qt6*.dll"; DestDir: "{app}"; Flags: ignoreversion
; Qt 插件
Source: "..\dist\ftpflow\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\ftpflow\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\FTPFlow"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\ftpflow.ico"
Name: "{group}\卸载 FTPFlow"; Filename: "{uninstallexe}"
Name: "{autodesktop}\FTPFlow"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\ftpflow.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 FTPFlow"; Flags: nowait postinstall skipifsilent
