; FlashRec Windows 安装脚本（Inno Setup 6）
;
; 由 tools/package_windows.sh 调用，**不手写死版本与路径**：脚本把本文件里的三个占位符
;   __FR_VERSION__  __FR_SOURCE__  __FR_OUTPUT__
; 替换成实际值，生成 dist/.winpkg/installer.iss 后再交给 ISCC.exe 编译。
; 为什么不用 ISCC 的 /D 传参：Windows 路径里的反斜杠进 ISPP 会踩转义坑，占位符替换则
; 两边都不必关心转义。
;
; 职责边界：本文件只负责"把一个已经备好的目录变成安装程序"，**不重新罗列文件清单** ——
; 暂存目录由 `cmake --install` 按 CMakeLists.txt 的 WIN32 安装规则产出（那是唯一真值），
; 这里只写 `Source: ...\*` 整目录纳入。

#define MyAppName "FlashRec"
#define MyAppPublisher "FlashRec"
#define MyAppExeName "flashrec.exe"

[Setup]
; AppId 必须固定不变：它就是「添加/删除程序」里的身份。若跟着版本走，用户升级后会看到
; 两个并存的条目、旧版本卸载不掉。
AppId={{7A3E9C21-4B6D-4F58-9C2A-1E5D8B7F0A34}
AppName={#MyAppName}
AppVersion=__FR_VERSION__
AppPublisher={#MyAppPublisher}
; {autopf} 是"自适应 Program Files"：有管理员权限时 = C:\Program Files\...，
; 没有时 = %LOCALAPPDATA%\Programs\...。配 PrivilegesRequired=lowest ⇒ 默认不弹 UAC。
; 投屏接收端是普通用户应用，不该为装它要一次管理员密码。
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
PrivilegesRequired=lowest
; 产物只有 x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 刻意不设 SetupIconFile：assets/icons 只有 png 而 Inno 要 .ico，先不引入转换步骤；
; 卸载项图标直接用 exe 自身图标（flashrec.exe 的图标由 app.rc 注入）。
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} __FR_VERSION__
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 不写 WizardResizable：6.7 起该指令已废弃（写了只是多一条编译警告）

OutputDir=__FR_OUTPUT__
OutputBaseFilename=FlashRec-__FR_VERSION__-win64-setup
; 应用是单实例 + 托盘常驻：覆盖正在运行的 exe 会失败，所以先请用户关掉它。
; RestartApplications=no —— DMR 不该被安装程序自作主张拉起来（启动由用户点完成页的勾）。
CloseApplications=yes
RestartApplications=no

; 官方 Inno Setup 6.7.3 **不带**中文语言包（Languages/ 里没有 Chinese*），所以中文是可选：
; 打包脚本发现 Languages\ChineseSimplified.isl 存在才加 /DFR_ZH 把下面这段编进来。
; 这样换机器重下 Inno Setup、中文包还没补上时，编译仍然成功（退化为英文向导），
; 不会因为一个语言文件让整个打包挂掉。
[Languages]
#ifdef FR_ZH
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式:"; Flags: unchecked

[Files]
Source: "__FR_SOURCE__\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
