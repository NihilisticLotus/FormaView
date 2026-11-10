#ifndef Payload
  #error Payload must be supplied by build.py
#endif
#ifndef PackageVersion
  #define PackageVersion "0.1.0"
#endif
#ifndef CandidateId
  #error CandidateId must be supplied by build.py
#endif
#define Identity "{618397E7-C70C-4D14-B601-D6FB2F562BA1}"

[Setup]
AppId={{618397E7-C70C-4D14-B601-D6FB2F562BA1}
AppName=FormaView
AppVersion={#PackageVersion}
AppVerName=FormaView {#PackageVersion}
VersionInfoVersion={#PackageVersion}.0
VersionInfoProductName=FormaView
VersionInfoDescription=FormaView Windows x64 installer
DefaultDirName={localappdata}\Programs\FormaView
DefaultGroupName=FormaView
DisableProgramGroupPage=yes
DisableWelcomePage=no
WizardSizePercent=145,140
ChangesAssociations=yes
DisableDirPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
SetupIconFile={#Payload}\formaview.ico
UninstallDisplayIcon={app}\formaview.exe
UninstallDisplayName=FormaView {#PackageVersion}
OutputDir={#OutputPath}
OutputBaseFilename=FormaView-{#PackageVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
SetupLogging=yes
InfoBeforeFile=install-notice.txt
AllowNoIcons=yes
Uninstallable=yes
UsedUserAreasWarning=no
#ifdef TestExtraDiskSpace
; Acceptance-only requirement injection; absent from the delivery build.
ExtraDiskSpaceRequired={#TestExtraDiskSpace}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[LangOptions]
DialogFontName=Microsoft YaHei UI
DialogFontSize=10

[Messages]
SetupWindowTitle=安装 - %1
ButtonBack=< 上一步
ButtonNext=下一步 >
ButtonInstall=安装
ButtonCancel=取消
ButtonFinish=完成
ButtonBrowse=浏览...
WizardInfoBefore=许可与第三方声明
InfoBeforeLabel=请阅读以下安装说明。
InfoBeforeClickLabel=阅读后点击“下一步”继续。
WelcomeLabel1=欢迎安装 FormaView
FinishedHeadingLabel=FormaView 安装完成
WizardSelectDir=安装位置与磁盘空间
SelectDirDesc=选择 FormaView 的专用安装目录。
SelectDirLabel3=FormaView 将安装到以下目录。
SelectDirBrowseLabel=点击“下一步”继续，或点击“浏览”选择其他目录。
DiskSpaceMBLabel=安装文件至少需要 [mb] MB；还需预留安装临时空间。
WizardSelectComponents=安装组件
SelectComponentsDesc=核心程序和运行依赖必须完整安装。
SelectComponentsLabel2=以下组件为程序运行所必需。点击“下一步”继续。
ComponentsDiskSpaceMBLabel=当前组件至少需要 [mb] MB 磁盘空间。
WizardSelectTasks=快捷方式
SelectTasksDesc=选择需要创建的快捷方式。
SelectTasksLabel2=选择需要的快捷方式，然后点击“下一步”。
WizardReady=确认安装与关联变更
ReadyLabel1=安装前请核对目录、组件、快捷方式及逐项文件关联摘要。
ReadyLabel2a=点击“安装”开始，或点击“上一步”修改选择。
ReadyLabel2b=点击“安装”开始。

[Types]
Name: "full"; Description: "核心程序及全部运行依赖（必需）"

[Components]
Name: "core"; Description: "FormaView 程序及全部运行依赖（约 {#PayloadMiB} MiB）"; Types: full; Flags: fixed

[Tasks]
Name: "startmenu"; Description: "创建开始菜单入口"
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Files]
#include "files.generated.iss"

[Dirs]
; The preflight write probe creates this directory before Inno sees it.
; Explicit ownership lets uninstall remove it ONLY when empty.
Name: "{app}"; Flags: uninsalwaysuninstall

[Icons]
Name: "{group}\FormaView"; Filename: "{app}\formaview.exe"; WorkingDir: "{app}"; Tasks: startmenu
Name: "{group}\Uninstall FormaView"; Filename: "{uninstallexe}"; Tasks: startmenu
Name: "{group}\安装使用说明"; Filename: "{app}\安装使用说明.txt"; Tasks: startmenu
Name: "{userdesktop}\FormaView"; Filename: "{app}\formaview.exe"; WorkingDir: "{app}"; Tasks: desktopicon

; Association writes and cleanup are explicitly ownership-checked in code.
; Only Inno's owned-file log controls removal. User state is always retained.
[Run]
Filename: "{app}\formaview.exe"; Description: "运行 FormaView"; Flags: postinstall nowait skipifsilent unchecked

[Registry]
; User task history survives uninstall. The first successful setup selects a
; layout; later upgrades and reinstalls retain it.
Root: HKCU64; Subkey: "Software\FormaView\Installer"; ValueType: string; ValueName: "TaskHistoryLayout"; ValueData: "{code:InitialTaskHistoryLayout}"; Flags: createvalueifdoesntexist

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#Identity}_is1';

var
  LegacyTaskHistoryUpgrade: Boolean;

function InitialTaskHistoryLayout(Param: string): string;
begin
  if LegacyTaskHistoryUpgrade then Result := 'legacy'
  else Result := 'installed';
end;

function GetFileAttributesW(Name: string): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function CreateFileW(Name: string; Access, Share: LongWord; Security: Integer;
  Creation, Flags: LongWord; Template: Integer): Integer;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: Integer): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function ReparseInPath(Path: string): Boolean;
var Attr: LongWord; Parent: string;
begin
  Result := False;
  while Length(Path) > 2 do begin
    Attr := GetFileAttributesW(Path);
    if (Attr <> $FFFFFFFF) and ((Attr and $400) <> 0) then begin
      Result := True; exit;
    end;
    Parent := ExtractFileDir(Path);
    if Parent = Path then exit;
    Path := Parent;
  end;
end;

function HasContents(Path: string): Boolean;
var Find: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Path) + '*', Find) then begin
    try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then begin
          Result := True; exit;
        end;
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
end;

function CheckFiles(Root, ListFile: string; CheckOwnership: Boolean): string;
var Lines, Previous: TArrayOfString; I, J, Handle: Integer;
    Name, Full: string; Owned: Boolean;
begin
  Result := '';
  if not LoadStringsFromFile(ListFile, Lines) then begin
    Result := 'Cannot read owned-file manifest. Reinstall to repair. / 无法读取安装清单，请重装修复。'; exit;
  end;
  if CheckOwnership and not LoadStringsFromFile(AddBackslash(Root) + 'owned-files.txt', Previous) then begin
    Result := 'Missing previous ownership manifest. / 缺少原安装文件清单。'; exit;
  end;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Name := Lines[I];
    if (Name = '') or (Pos('..', Name) > 0) or (Pos(':', Name) > 0) or
       (Copy(Name, 1, 1) = '\') or (Copy(Name, 1, 1) = '/') then begin
      Result := 'Invalid ownership manifest.'; exit;
    end;
    Full := AddBackslash(Root) + Name;
    if ReparseInPath(Full) then begin
      Result := 'Junction/symbolic link detected; no files changed. / 路径含目录联接或符号链接，已停止。' + #13#10 + Full; exit;
    end;
    if DirExists(Full) then begin
      Result := 'A directory conflicts with an installed file: ' + Full; exit;
    end;
    if FileExists(Full) then begin
      if CheckOwnership then begin
        Owned := False;
        for J := 0 to GetArrayLength(Previous) - 1 do
          if CompareText(Name, Previous[J]) = 0 then Owned := True;
        if not Owned then begin
          Result := 'Existing user file would be overwritten: ' + Full; exit;
        end;
      end;
      Handle := CreateFileW(Full, $C0000000, 0, 0, 3, $80, 0);
      if Handle = -1 then begin
        Result := 'File is in use or not writable. Close FormaView and its MCP service, then retry. / 文件占用或无写权限，请关闭程序及 MCP 服务后重试。' + #13#10 + Full; exit;
      end;
      CloseHandle(Handle);
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): string;
var Root, PreviousPath, Marker, Probe, SpacePath, Parent: string; Existing: Boolean;
    FreeSpace, TotalSpace, RequiredSpace: Int64;
begin
  Root := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  Result := '';
  if (Length(Root) <= 3) or (Copy(Root, 1, 2) = '\\') or ReparseInPath(Root) then begin
    Result := 'Choose a local dedicated folder without junctions. / 请选择不含目录联接的本机专用目录。'; exit;
  end;
  if RegQueryStringValue(HKCU64, UninstallKey, 'InstallLocation', PreviousPath) then
    if CompareText(RemoveBackslashUnlessRoot(PreviousPath), Root) <> 0 then begin
      Result := 'Upgrade must use the existing folder. To move, uninstall first; settings and tasks are retained. / 覆盖安装须使用原目录；迁移请先卸载，设置和任务会保留。'; exit;
    end;
  Marker := AddBackslash(Root) + 'installation.ini';
  Existing := GetIniString('Install', 'AppId', '', Marker) = '{#Identity}';
  LegacyTaskHistoryUpgrade := Existing and (PreviousPath <> '');
  if HasContents(Root) and not Existing then begin
    Result := 'Choose an empty folder. Existing user files will not be overwritten. / 请选择空目录，安装器不会覆盖已有用户文件。'; exit;
  end;
  if Existing and not FileExists(AddBackslash(Root) + 'unins000.dat') then begin
    Result := 'Incomplete prior installation; keep user data and use a new empty folder. / 原卸载记录缺失，请保留用户数据并选择新空目录。'; exit;
  end;
  SpacePath := Root;
  while not DirExists(SpacePath) and (Length(SpacePath) > 3) do begin
    Parent := ExtractFileDir(SpacePath);
    if Parent = SpacePath then break;
    SpacePath := Parent;
  end;
  RequiredSpace := {#PayloadMiB} * 1048576;
  RequiredSpace := RequiredSpace + 16777216;
#ifdef TestExtraDiskSpace
  RequiredSpace := RequiredSpace + {#TestExtraDiskSpace};
#endif
  if not GetSpaceOnDisk64(SpacePath, FreeSpace, TotalSpace) then begin
    Result := 'Cannot read target disk space. / 无法读取目标卷空间，请检查目录后重试。'; exit;
  end;
  if FreeSpace < RequiredSpace then begin
    Result := 'Insufficient disk space. / 磁盘空间不足：至少需要 ' + IntToStr(RequiredSpace div 1048576) +
      ' MiB，可用 ' + IntToStr(FreeSpace div 1048576) + ' MiB。'; exit;
  end;
  ExtractTemporaryFile('owned-files.txt');
  Result := CheckFiles(Root, ExpandConstant('{tmp}\owned-files.txt'), Existing);
  if Result <> '' then begin Log(Result); exit; end;
  if not ForceDirectories(Root) then begin
    Result := 'No permission to create installation folder. / 无权限创建安装目录。'; exit;
  end;
  Probe := AddBackslash(Root) + '.formaview-write-test-618397E7';
  if FileExists(Probe) or DirExists(Probe) then begin
    Result := 'Write-test path already exists; no existing file will be removed.'; exit;
  end;
  if not SaveStringToFile(Probe, 'FormaView installer write check', False) then begin
    Result := 'No write permission. Choose a writable folder. / 无写入权限，请选择可写目录。'; exit;
  end;
  if not DeleteFile(Probe) then Result := 'Cannot remove installer write probe; installation stopped.';
end;

function InitializeUninstall: Boolean;
var Problem, Root: string;
begin
  Root := ExpandConstant('{app}');
  Problem := CheckFiles(Root, AddBackslash(Root) + 'owned-files.txt', False);
  Result := Problem = '';
  if not Result then begin
    Log(Problem);
    SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);
  end;
end;

#include "associations.iss"
