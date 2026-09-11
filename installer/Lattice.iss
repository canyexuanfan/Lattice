; Lattice Inno Setup 安装脚本
; 先运行 scripts\package.ps1 构建 Release 并调用 ISCC.exe。

#define MyAppName "Lattice"
#define MyAppVersion "0.4.52"
#define MyAuthorSignature "@十七°"
#define MyAppPublisher "Lattice"
#define MyAppExeName "Lattice.exe"
#define MyAppFolderName "Lattice"
#ifndef OfflineBuild
  #define OfflineBuild 0
#endif
#if Int(OfflineBuild) == 1
  #define BuildDir "..\x64\ReleaseOffline"
  #define InstallerSuffix "-Offline"
#else
  #define BuildDir "..\x64\Release"
  #define InstallerSuffix ""
#endif
#define InstallerBaseName "Lattice-Setup-" + MyAppVersion + InstallerSuffix

[Setup]
AppId={{6D5B2D54-44D8-4F80-8B94-1C6D1A3DBA0E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion} {#MyAuthorSignature}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}.0
DefaultDirName=E:\Program\Lattice
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
AppendDefaultDirName=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputDir=..\release
OutputBaseFilename={#InstallerBaseName}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
SetupIconFile=..\assets\branding\lattice-logo.ico
WizardSizePercent=110
WizardBackColor=$F7F9FC
WizardImageFile=assets\wizard-left.png
WizardImageBackColor=$071222
WizardSmallImageFile=assets\wizard-small.png
WizardSmallImageBackColor=$071222
CloseApplications=yes
RestartApplications=no
UsePreviousTasks=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Messages]
WelcomeLabel1=欢迎进入 [name] 安装向导
WelcomeLabel2=把散落在桌面的文件、文件夹、快捷方式和系统图标，整理成清晰的分类。%n%n桌面项目始终保留在原始路径；Lattice 只接管显示归属，退出后 Explorer 原生图标会立即恢复。
WizardSelectDir=选择安装位置
SelectDirDesc=选择 [name] 的安装位置
SelectDirLabel3=请输入或选择上级目录；点击“下一步”时，安装器会自动补齐 Lattice 子文件夹。
SelectDirBrowseLabel=可以手动修改路径，也可以点击“浏览”选择目录；已经包含 Lattice 时不会重复追加。
ReadyLabel1=准备好开始整理桌面了吗？
ReadyLabel2a=点击“安装”完成部署；如需调整安装位置，请点击“上一步”。
ReadyLabel2b=点击“安装”完成部署。
InstallingLabel=正在把 [name] 安装到你的电脑，请稍候。
FinishedHeadingLabel=[name] 安装完成
FinishedLabelNoIcons=安装完成。现在可以启动 [name]，开始整理桌面项目。
FinishedLabel=安装完成。现在可以启动 [name]，开始整理桌面项目。
ClickFinish=点击“完成”退出安装向导。


[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\Data"; Permissions: users-modify

[Tasks]
Name: "startup"; Description: "随 Windows 启动"; GroupDescription: "附加选项："
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项："
Name: "startmenuicon"; Description: "添加到开始菜单"; GroupDescription: "附加选项："

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startmenuicon
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Lattice"; Flags: deletevalue; Check: not WizardIsTaskSelected('startup')
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Lattice"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startup

[InstallDelete]
Type: files; Name: "{app}\Luno.exe"
Type: files; Name: "{app}\DesktopOrganizer.exe"
Type: files; Name: "{userdesktop}\{#MyAppName}.lnk"; Check: not WizardIsTaskSelected('desktopicon')
Type: files; Name: "{group}\{#MyAppName}.lnk"; Check: not WizardIsTaskSelected('startmenuicon')
Type: files; Name: "{group}\卸载 {#MyAppName}.lnk"; Check: not WizardIsTaskSelected('startmenuicon')

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\{#MyAppExeName}"; Flags: nowait runasoriginaluser; Check: WizardSilent

[Code]
const
  WizardImageDesignWidth = 480;
  WizardImageDesignHeight = 918;
  WizardVersionLeft = 48;
  WizardVersionTop = 854;
  WizardVersionFontHeight = 18;
  ProductMainWindowClass = 'Lattice.MainWindow';
  ProductMessageDialogClass = 'Lattice.MessageDialog';
  LegacyLunoMainWindowClass = 'Luno.MainWindow';
  LegacyLunoMessageDialogClass = 'Luno.MessageDialog';
  OldestMainWindowClass = 'DesktopOrganizer.MainWindow';
  OldestMessageDialogClass = 'DesktopOrganizer.MessageDialog';
  ProductInstanceMutex = 'Global\Lattice.LocalOnly.V0';
  LegacyLunoInstanceMutex = 'Global\Luno.LocalOnly.V0';
  OldestInstanceMutex = 'Global\DesktopOrganizer.LocalOnly.V0';
  ProductUpdateExitMessageName = 'Lattice.RequestExitForUpdate.V1';
  LegacyLunoUpdateExitMessageName = 'Luno.RequestExitForUpdate.V1';
  OldestUpdateExitMessageName = 'DesktopOrganizer.RequestExitForUpdate.V1';
  WM_CLOSE = $0010;
  ProductExitPollMilliseconds = 100;
  ProductExitMessageWaitAttempts = 30;
  ProductExitCloseWaitAttempts = 20;
  ShellIconsKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons';
  OverlayBackupKey = 'Software\Lattice\Installer';
  OverlayValueName = '29';
  OverlayBackupReadyName = 'ShortcutOverlayBackupReady';
  OverlayHadOriginalName = 'ShortcutOverlayHadOriginal';
  OverlayOriginalValueName = 'ShortcutOverlayOriginalValue';
  OverlayAppliedValueName = 'ShortcutOverlayAppliedValue';
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

function PostMessageW(Wnd: HWND; Msg: Cardinal; WParam: WPARAM; LParam: LPARAM): Boolean;
  external 'PostMessageW@user32.dll stdcall';

function RegisterWindowMessageW(MessageName: String): Cardinal;
  external 'RegisterWindowMessageW@user32.dll stdcall';

function GetWindowThreadProcessId(Wnd: HWND; var ProcessId: DWORD): DWORD;
  external 'GetWindowThreadProcessId@user32.dll stdcall';

procedure SHChangeNotify(EventId: LongWord; Flags: LongWord; Item1: LongWord; Item2: LongWord);
  external 'SHChangeNotify@shell32.dll stdcall';

var
  ProductExitDialogDismissed: Boolean;
  ProductExitProcessId: DWORD;
  ProductConfigSnapshotPath: String;
  ProductConfigTargetPath: String;
  ProductConfigSnapshotReady: Boolean;
  DetectedUpdateExitMessageName: String;
  LegacyStartupWasEnabled: Boolean;

function ShortcutOverlayValue: String;
begin
  Result := ExpandConstant('{app}\{#MyAppExeName},-102');
end;

procedure NotifyShortcutOverlayChanged;
begin
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;

procedure SnapshotShortcutOverlay;
var
  Ready: Cardinal;
  ExistingValue: String;
  HadOriginal: Boolean;
begin
  Ready := 0;
  if RegQueryDWordValue(HKCU, OverlayBackupKey, OverlayBackupReadyName, Ready) and
     (Ready = 1) then
    exit;

  HadOriginal := RegQueryStringValue(HKCU, ShellIconsKey, OverlayValueName, ExistingValue);
  if not RegWriteDWordValue(HKCU, OverlayBackupKey, OverlayHadOriginalName, Ord(HadOriginal)) then
    RaiseException('无法保存原快捷方式箭头状态，安装已停止。');
  if HadOriginal and
     not RegWriteStringValue(HKCU, OverlayBackupKey, OverlayOriginalValueName, ExistingValue) then
    RaiseException('无法保存原快捷方式箭头路径，安装已停止。');
  if not RegWriteDWordValue(HKCU, OverlayBackupKey, OverlayBackupReadyName, 1) then
    RaiseException('无法完成快捷方式箭头备份，安装已停止。');
end;

procedure ApplyShortcutOverlay;
var
  AppliedValue: String;
begin
  SnapshotShortcutOverlay;
  AppliedValue := ShortcutOverlayValue;
  if not RegWriteStringValue(HKCU, ShellIconsKey, OverlayValueName, AppliedValue) then
    RaiseException('无法应用 Lattice 快捷方式箭头，安装已停止。');
  if not RegWriteStringValue(HKCU, OverlayBackupKey, OverlayAppliedValueName, AppliedValue) then
    RaiseException('无法记录 Lattice 快捷方式箭头状态，安装已停止。');
  NotifyShortcutOverlayChanged;
end;

procedure RestoreShortcutOverlay;
var
  Ready: Cardinal;
  HadOriginal: Cardinal;
  CurrentValue: String;
  AppliedValue: String;
  OriginalValue: String;
begin
  Ready := 0;
  if not RegQueryDWordValue(HKCU, OverlayBackupKey, OverlayBackupReadyName, Ready) or
     (Ready <> 1) then
    exit;

  if RegQueryStringValue(HKCU, OverlayBackupKey, OverlayAppliedValueName, AppliedValue) and
     RegQueryStringValue(HKCU, ShellIconsKey, OverlayValueName, CurrentValue) and
     (CompareText(CurrentValue, AppliedValue) = 0) then
  begin
    HadOriginal := 0;
    RegQueryDWordValue(HKCU, OverlayBackupKey, OverlayHadOriginalName, HadOriginal);
    if (HadOriginal = 1) and
       RegQueryStringValue(HKCU, OverlayBackupKey, OverlayOriginalValueName, OriginalValue) then
      RegWriteStringValue(HKCU, ShellIconsKey, OverlayValueName, OriginalValue)
    else
      RegDeleteValue(HKCU, ShellIconsKey, OverlayValueName);
    NotifyShortcutOverlayChanged;
  end
  else
    Log('Shortcut overlay changed outside Lattice; leaving the current user value untouched.');

  RegDeleteValue(HKCU, OverlayBackupKey, OverlayBackupReadyName);
  RegDeleteValue(HKCU, OverlayBackupKey, OverlayHadOriginalName);
  RegDeleteValue(HKCU, OverlayBackupKey, OverlayOriginalValueName);
  RegDeleteValue(HKCU, OverlayBackupKey, OverlayAppliedValueName);
end;

function SnapshotProductConfig: Boolean;
begin
  Result := True;
  if ProductConfigSnapshotReady then
  begin
    Log('Reusing the original pre-update product config snapshot.');
    exit;
  end;
  ProductConfigTargetPath := ExpandConstant('{userappdata}\Lattice\config.ini');
  if not FileExists(ProductConfigTargetPath) then
    ProductConfigTargetPath := ExpandConstant('{userappdata}\Luno\config.ini');
  if not FileExists(ProductConfigTargetPath) then
    ProductConfigTargetPath := ExpandConstant('{userappdata}\DesktopOrganizer\config.ini');
  if not FileExists(ProductConfigTargetPath) then
  begin
    Log('No existing product config needs update preservation.');
    exit;
  end;

  ProductConfigSnapshotPath := ExpandConstant('{tmp}\Lattice-config-before-update.ini');
  if not CopyFile(ProductConfigTargetPath, ProductConfigSnapshotPath, False) then
  begin
    Log('Failed to snapshot the existing product config before update.');
    Result := False;
    exit;
  end;
  ProductConfigSnapshotReady := True;
  Log('Existing product config was snapshotted before update exit.');
end;

procedure RestoreProductConfigSnapshot;
begin
  if not ProductConfigSnapshotReady then
    exit;
  if not ForceDirectories(ExtractFileDir(ProductConfigTargetPath)) then
    RaiseException('无法恢复 Lattice 用户配置目录，安装已停止。');
  if not CopyFile(ProductConfigSnapshotPath, ProductConfigTargetPath, False) then
    RaiseException('无法恢复安装前的 Lattice 用户配置，安装已停止。');
  ProductConfigSnapshotReady := False;
  Log('Pre-update product config was restored before the new version can start.');
end;

procedure SnapshotLegacyStartupState;
var
  ExistingValue: String;
begin
  LegacyStartupWasEnabled :=
    RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Luno', ExistingValue) or
    RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'DesktopOrganizer', ExistingValue);
end;

procedure MigrateLegacyStartupState;
begin
  if LegacyStartupWasEnabled and WizardIsTaskSelected('startup') then
  begin
    if not RegWriteStringValue(
      HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'Lattice',
      ExpandConstant('"{app}\{#MyAppExeName}"')) then
      RaiseException('无法迁移 Lattice 开机启动设置，安装已停止。');
  end;
  RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Luno');
  RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'DesktopOrganizer');
end;

procedure CreateWizardVersionLabel(Image: TBitmapImage);
var
  VersionLabel: TLabel;
  ScaledFontHeight: Integer;
begin
  VersionLabel := TLabel.Create(WizardForm);
  VersionLabel.Parent := Image.Parent;
  VersionLabel.Transparent := True;
  VersionLabel.AutoSize := True;
  VersionLabel.ShowAccelChar := False;
  VersionLabel.Caption := 'Lattice / {#MyAppVersion} {#MyAuthorSignature}';
  VersionLabel.Font.Name := 'Microsoft YaHei UI';
  VersionLabel.Font.Color := StrToColor('#CBECF9');
  VersionLabel.Font.Style := [];

  ScaledFontHeight :=
    (Image.Height * WizardVersionFontHeight + WizardImageDesignHeight div 2) div
    WizardImageDesignHeight;
  if ScaledFontHeight < 1 then
    ScaledFontHeight := 1;
  VersionLabel.Font.Height := -ScaledFontHeight;
  VersionLabel.Left := Image.Left +
    (Image.Width * WizardVersionLeft + WizardImageDesignWidth div 2) div
    WizardImageDesignWidth;
  VersionLabel.Top := Image.Top +
    (Image.Height * WizardVersionTop + WizardImageDesignHeight div 2) div
    WizardImageDesignHeight;
  VersionLabel.BringToFront;
end;

function FindProductMainWindow: HWND;
begin
  DetectedUpdateExitMessageName := ProductUpdateExitMessageName;
  Result := FindWindowByClassName(ProductMainWindowClass);
  if Result = 0 then
  begin
    Result := FindWindowByClassName(LegacyLunoMainWindowClass);
    DetectedUpdateExitMessageName := LegacyLunoUpdateExitMessageName;
  end;
  if Result = 0 then
  begin
    Result := FindWindowByClassName(OldestMainWindowClass);
    DetectedUpdateExitMessageName := OldestUpdateExitMessageName;
  end;
end;

function FindProductMessageDialog: HWND;
begin
  Result := FindWindowByClassName(ProductMessageDialogClass);
  if Result = 0 then
    Result := FindWindowByClassName(LegacyLunoMessageDialogClass);
  if Result = 0 then
    Result := FindWindowByClassName(OldestMessageDialogClass);
end;

function IsProductRunning: Boolean;
begin
  Result :=
    CheckForMutexes(ProductInstanceMutex) or
    CheckForMutexes(LegacyLunoInstanceMutex) or
    CheckForMutexes(OldestInstanceMutex);
end;

procedure DismissProductExitDialog;
var
  DialogWindow: HWND;
  DialogProcessId: DWORD;
begin
  DialogWindow := FindProductMessageDialog;
  if DialogWindow <> 0 then
  begin
    DialogProcessId := 0;
    GetWindowThreadProcessId(DialogWindow, DialogProcessId);
    if (ProductExitProcessId = 0) or (DialogProcessId <> ProductExitProcessId) then
      exit;
    if not ProductExitDialogDismissed then
      Log('The product displayed a non-interactive exit warning; closing it with the safe WM_CLOSE result.');
    ProductExitDialogDismissed := True;
    PostMessageW(DialogWindow, WM_CLOSE, 0, 0);
  end;
end;

function WaitForProductExit(Attempts: Integer): Boolean;
var
  Attempt: Integer;
begin
  Result := not IsProductRunning;
  if Result then
    exit;

  for Attempt := 1 to Attempts do
  begin
    DismissProductExitDialog;
    Sleep(ProductExitPollMilliseconds);
    if not IsProductRunning then
    begin
      Result := True;
      exit;
    end;
  end;
end;

function RequestProductExitForUpdate: Boolean;
var
  MainWindow: HWND;
  UpdateExitMessage: Cardinal;
begin
  Result := True;
  ProductExitDialogDismissed := False;
  ProductExitProcessId := 0;
  if not IsProductRunning then
    exit;

  Log('Lattice or a compatible earlier version is running; requesting a graceful exit before updating files.');
  MainWindow := FindProductMainWindow;
  if MainWindow = 0 then
  begin
    Log('The product mutex exists but the main window is already gone; checking briefly for normal cleanup.');
    Result := WaitForProductExit(ProductExitCloseWaitAttempts);
    exit;
  end;

  GetWindowThreadProcessId(MainWindow, ProductExitProcessId);

  UpdateExitMessage := RegisterWindowMessageW(DetectedUpdateExitMessageName);
  if UpdateExitMessage <> 0 then
  begin
    Log('Posting the matching product update-exit message.');
    PostMessageW(MainWindow, UpdateExitMessage, 0, 0);
    if WaitForProductExit(ProductExitMessageWaitAttempts) then
      exit;
  end;

  MainWindow := FindProductMainWindow;
  if MainWindow <> 0 then
  begin
    Log('Falling back to WM_CLOSE for a compatible earlier version.');
    PostMessageW(MainWindow, WM_CLOSE, 0, 0);
  end;
  Result := WaitForProductExit(ProductExitCloseWaitAttempts);
end;

function TrimTrailingBackslashes(Value: String): String;
begin
  Result := Value;
  while (Length(Result) > 3) and (Result[Length(Result)] = '\') do
    Delete(Result, Length(Result), 1);
end;

function EnsureProductSubfolder(Value: String): String;
var
  Clean: String;
begin
  Clean := TrimTrailingBackslashes(Value);
  if Clean = '' then
  begin
    Result := Value;
    exit;
  end;

  if CompareText(ExtractFileName(Clean), '{#MyAppFolderName}') <> 0 then
    Result := AddBackslash(Clean) + '{#MyAppFolderName}'
  else
    Result := Clean;
end;

procedure NormalizeInstallDirectory;
var
  Original: String;
  Normalized: String;
begin
  Original := WizardForm.DirEdit.Text;
  Normalized := EnsureProductSubfolder(Original);
  Log('NormalizeInstallDirectory input=[' + Original + '] output=[' + Normalized + ']');
  if CompareText(Normalized, Original) <> 0 then
    WizardForm.DirEdit.Text := Normalized;
  Log('NormalizeInstallDirectory final=[' + WizardForm.DirEdit.Text + ']');
end;

procedure DirEditExit(Sender: TObject);
begin
  Log('DirEditExit called');
  NormalizeInstallDirectory;
end;

procedure InitializeWizard;
begin
  Log('InitializeWizard called');
  CreateWizardVersionLabel(WizardForm.WizardBitmapImage);
  CreateWizardVersionLabel(WizardForm.WizardBitmapImage2);
  WizardForm.DirEdit.OnExit := @DirEditExit;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  Log('CurPageChanged page=' + IntToStr(CurPageID));
  if CurPageID = wpSelectDir then
    NormalizeInstallDirectory;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Log('NextButtonClick page=' + IntToStr(CurPageID) + ' dir=[' + WizardForm.DirEdit.Text + ']');
  if CurPageID = wpSelectDir then
    NormalizeInstallDirectory;
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  SnapshotLegacyStartupState;
  if not SnapshotProductConfig then
  begin
    Result := '无法创建安装前配置快照。为避免改变格子位置、大小、设置或图标顺序，本次安装已停止。';
    exit;
  end;
  if not RequestProductExitForUpdate then
    Result := 'Lattice 未能正常结束更新退出。请先从托盘菜单选择“退出”；如果托盘图标已经消失，请在任务管理器中结束残留的 Lattice 进程，然后点击“重试”。安装器不会强制结束进程，以免损坏桌面数据。';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RestoreProductConfigSnapshot;
    MigrateLegacyStartupState;
    ApplyShortcutOverlay;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RestoreShortcutOverlay;
end;
