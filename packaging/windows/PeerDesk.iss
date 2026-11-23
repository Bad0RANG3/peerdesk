; PeerDesk Windows installer.
; The build script rewrites this file with a UTF-8 BOM before invoking ISCC.

#ifndef AppVersion
  #define AppVersion "0.9.0"
#endif
#ifndef PayloadDir
  #define PayloadDir "..\..\dist\stage"
#endif
#ifndef ArtifactDir
  #define ArtifactDir "..\..\dist"
#endif

#define MyAppName "PeerDesk"
#define MyAppPublisher "PeerDesk contributors"
#define MyAppUrl "https://github.com/Bad0RANG3/peerdesk"
#define MyAppExeName "peerdesk-client.exe"

[Setup]
AppId={{DFA1EBD0-B348-4B32-A0A7-7F4E5CDEAE93}
AppName={#MyAppName}
AppVersion={#AppVersion}
AppVerName={#MyAppName} {#AppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}/issues
AppUpdatesURL={#MyAppUrl}/releases
AppCopyright=Apache-2.0
DefaultDirName={localappdata}\Programs\PeerDesk
DefaultGroupName=PeerDesk
DisableProgramGroupPage=yes
DisableWelcomePage=no
ShowLanguageDialog=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#ArtifactDir}
OutputBaseFilename=PeerDesk-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}
VersionInfoTextVersion={#MyAppName} {#AppVersion}
VersionInfoDescription=PeerDesk installer
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Apache-2.0
VersionInfoOriginalFileName=PeerDesk-{#AppVersion}-setup.exe

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,*.ilk"

[Icons]
Name: "{autoprograms}\PeerDesk\PeerDesk"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Comment: "启动 PeerDesk"
Name: "{autodesktop}\PeerDesk"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Comment: "启动 PeerDesk"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即启动 PeerDesk"; Flags: nowait postinstall skipifsilent

[Messages]
SetupAppTitle=PeerDesk 安装程序
SetupWindowTitle=PeerDesk 安装程序 - %1
UninstallAppTitle=卸载 PeerDesk
UninstallAppFullTitle=卸载 %1
InformationTitle=提示
ConfirmTitle=确认
ErrorTitle=错误
SetupLdrStartupMessage=即将安装 %1。是否继续？
SetupAlreadyRunning=安装程序已经在运行。
WindowsVersionNotSupported=此程序不支持当前版本的 Windows。
OnlyOnTheseArchitectures=此程序只能安装在以下处理器架构的 Windows 上：%n%n%1
WinVersionTooLowError=此程序需要 %1 %2 或更高版本。
SetupAppRunningError=安装程序检测到 %1 正在运行。%n%n请关闭它的所有实例后点击“确定”继续，或点击“取消”退出。
UninstallAppRunningError=卸载程序检测到 %1 正在运行。%n%n请关闭它的所有实例后点击“确定”继续，或点击“取消”退出。
ErrorCreatingDir=安装程序无法创建目录“%1”。
ExitSetupTitle=退出安装
ExitSetupMessage=安装尚未完成。如果现在退出，PeerDesk 将不会被安装。%n%n你可以稍后重新运行安装程序。%n%n确定退出吗？
AboutSetupTitle=关于安装程序
AboutSetupMessage=%1 版本 %2%n%3%n%n%1 主页：%n%4
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安装(&I)
ButtonOK=确定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全部是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全部否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=浏览(&B)...
ButtonWizardBrowse=浏览(&R)...
ButtonNewFolder=新建文件夹(&M)
ClickNext=点击“下一步”继续，或点击“取消”退出安装。
BeveledLabel=PeerDesk · 自托管远程桌面
BrowseDialogTitle=选择文件夹
BrowseDialogLabel=在列表中选择文件夹，然后点击“确定”。
NewFolderName=新建文件夹
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=安装程序将在此电脑上安装 [name/ver]。%n%nPeerDesk 仅面向当前 Windows 用户安装，不需要管理员权限。建议先关闭正在运行的 PeerDesk 实例。
WizardSelectDir=选择安装位置
SelectDirDesc=[name] 应安装到哪里？
SelectDirLabel3=安装程序将把 [name] 安装到以下文件夹。
SelectDirBrowseLabel=点击“下一步”继续；如需选择其他文件夹，请点击“浏览”。
DiskSpaceGBLabel=至少需要 [gb] GB 可用磁盘空间。
DiskSpaceMBLabel=至少需要 [mb] MB 可用磁盘空间。
CannotInstallToNetworkDrive=安装程序不能安装到网络驱动器。
CannotInstallToUNCPath=安装程序不能安装到 UNC 路径。
InvalidPath=请输入带盘符的完整路径，例如：%n%nC:\APP%n%n或使用以下形式的 UNC 路径：%n%n\\server\share
InvalidDrive=所选驱动器或 UNC 共享不存在或无法访问，请重新选择。
DiskSpaceWarningTitle=磁盘空间不足
DiskSpaceWarning=安装至少需要 %1 KB 可用空间，但所选驱动器只有 %2 KB 可用。%n%n仍要继续吗？
DirExistsTitle=文件夹已存在
DirExists=文件夹：%n%n%1%n%n已经存在。是否仍要安装到该文件夹？
DirDoesntExistTitle=文件夹不存在
DirDoesntExist=文件夹：%n%n%1%n%n不存在。是否创建该文件夹？
WizardSelectTasks=选择附加任务
SelectTasksDesc=需要执行哪些附加任务？
SelectTasksLabel2=选择安装 [name] 时要执行的附加任务，然后点击“下一步”。
WizardReady=准备安装
ReadyLabel1=安装程序已准备好在你的电脑上安装 [name]。
ReadyLabel2a=点击“安装”继续；如需检查或修改设置，请点击“上一步”。
ReadyLabel2b=点击“安装”继续。
ReadyMemoUserInfo=用户信息：
ReadyMemoDir=安装位置：
ReadyMemoType=安装类型：
ReadyMemoComponents=已选组件：
ReadyMemoGroup=开始菜单文件夹：
ReadyMemoTasks=附加任务：
WizardPreparing=正在准备安装
PreparingDesc=安装程序正在准备将 [name] 安装到你的电脑。
PreviousInstallNotCompleted=上一次安装或卸载尚未完成。需要重启 Windows 才能继续。%n%n重启后请再次运行安装程序以完成 [name] 的安装。
CannotContinue=安装程序无法继续，请点击“取消”退出。
ApplicationsFound=以下应用正在使用安装程序需要更新的文件。建议允许安装程序自动关闭这些应用。
ApplicationsFound2=以下应用正在使用安装程序需要更新的文件。建议允许安装程序自动关闭这些应用；安装完成后，安装程序会尝试重新启动它们。
CloseApplications=自动关闭这些应用(&A)
DontCloseApplications=不关闭这些应用(&D)
ErrorCloseApplications=安装程序无法自动关闭所有应用。请手动关闭正在使用待更新文件的应用后重试。
WizardInstalling=正在安装
InstallingLabel=正在将 [name] 安装到你的电脑，请稍候。
FinishedHeadingLabel=PeerDesk 安装完成
FinishedLabelNoIcons=[name] 已安装完成。
FinishedLabel=[name] 已安装完成。你可以从开始菜单或桌面快捷方式启动 PeerDesk。
ClickFinish=点击“完成”退出安装程序。
FinishedRestartLabel=要完成 [name] 的安装，需要重启电脑。现在重启吗？
RunEntryExec=运行 %1
RunEntryShellExec=查看 %1
StatusClosingApplications=正在关闭应用...
StatusCreateDirs=正在创建目录...
StatusExtractFiles=正在解压文件...
StatusCreateIcons=正在创建快捷方式...
StatusCreateRegistryEntries=正在写入注册表...
StatusSavingUninstall=正在保存卸载信息...
StatusRunProgram=正在完成安装...
StatusRollback=正在回滚更改...
ErrorExecutingProgram=无法运行文件：%n%1
ConfirmUninstall=确定要卸载 %1 吗？%n%nPeerDesk 的程序文件和快捷方式会被删除；用户 AppData\Roaming\PeerDesk 中的连接偏好、最近连接以及用户接收目录中的文件会保留。
UninstallStatusLabel=正在从你的电脑中卸载 %1，请稍候。
UninstalledAll=%1 已成功卸载。
UninstalledMost=%1 卸载完成。%n%n部分元素无法删除，可能需要手动清理。
UninstalledAndNeedsRestart=要完成 %1 的卸载，需要重启电脑。%n%n现在重启吗？
WizardUninstalling=卸载状态
StatusUninstalling=正在卸载 %1...

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RegDeleteValue(
      HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'PeerDesk');
  end;
end;
