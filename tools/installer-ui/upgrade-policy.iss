type
  TVersionParts = array[0..3] of Integer;

var
  PreviousInstallDir, PreviousInstallVersion: string;
  InstallVersionOrder: Integer; { incoming relative to installed }
  RetiredFiles: TArrayOfString;
  RetiredStorage: string;
  RetiredCount: Integer;
  RetiredCommitted: Boolean;

function ParseProductVersion(Text: string; var Parts: TVersionParts): Boolean;
var I, J, Digits, Value, Ch: Integer;
begin
  Result := False;
  for I := 0 to 3 do Parts[I] := 0;
  if (Copy(Text, 1, 1) = 'v') or (Copy(Text, 1, 1) = 'V') then Delete(Text, 1, 1);
  if (Length(Text) < 5) or (Length(Text) > 23) then exit;
  I := 0; Digits := 0; Value := 0;
  for J := 1 to Length(Text) do begin
    Ch := Ord(Text[J]);
    if Ch = Ord('.') then begin
      if (Digits = 0) or (I = 3) then exit;
      Parts[I] := Value; I := I + 1; Digits := 0; Value := 0;
    end else begin
      if (Ch < Ord('0')) or (Ch > Ord('9')) or (Digits = 5) then exit;
      Value := Value * 10 + Ch - Ord('0');
      if Value > 65535 then exit;
      Digits := Digits + 1;
    end;
  end;
  if (I < 2) or (Digits = 0) then exit;
  Parts[I] := Value;
  Result := True;
end;

function CompareProductVersions(A, B: string; var Order: Integer): Boolean;
var Left, Right: TVersionParts; I: Integer;
begin
  Order := 0;
  Result := ParseProductVersion(A, Left) and ParseProductVersion(B, Right);
  if not Result then exit;
  for I := 0 to 3 do
    if Left[I] <> Right[I] then begin
      if Left[I] < Right[I] then Order := -1 else Order := 1;
      exit;
    end;
end;

function ReadUpgradeState(var PriorDir, PriorVersion: string; var Order: Integer): string;
var Marker, MarkerVersion, Drive: string; MarkerOrder: Integer;
begin
  Result := ''; PriorDir := ''; PriorVersion := ''; Order := 0;
  if RegKeyExists(OtherInstallRootKey, UninstallKey) then begin
    if RegKeyExists(InstallRootKey, UninstallKey) then
      Result := '检测到两个安装范围中都登记了 FormaView。请先核对 Windows 已安装的应用，避免覆盖错误的副本。'
    else
      Result := '检测到另一安装范围中的 FormaView。请重新运行安装包，沿用原安装范围升级。';
    exit;
  end;
  if not RegKeyExists(InstallRootKey, UninstallKey) then exit;
  Result := '无法确认现有 FormaView 的版本或安装登记。请核对 Windows 已安装的应用及原目录，然后重试。';
  if not RegQueryStringValue(InstallRootKey, UninstallKey, 'InstallLocation', PriorDir) or
     not RegQueryStringValue(InstallRootKey, UninstallKey, 'DisplayVersion', PriorVersion) or
     not CompareProductVersions('{#PackageVersion}', PriorVersion, Order) then exit;
  PriorDir := RemoveBackslashUnlessRoot(PriorDir);
  Drive := ExtractFileDrive(PriorDir);
  if (Length(Drive) <> 2) or (Length(PriorDir) < 4) or (Copy(PriorDir, 3, 1) <> '\') or
     (Pos('..', PriorDir) <> 0) or ReparseInPath(PriorDir) or not DirExists(PriorDir) then exit;
  Marker := AddBackslash(PriorDir) + 'installation.ini';
  if ReparseInPath(Marker) or (GetIniString('Install', 'AppId', '', Marker) <> '{#Identity}') then exit;
  MarkerVersion := GetIniString('Install', 'Version', '', Marker);
  if not CompareProductVersions(MarkerVersion, PriorVersion, MarkerOrder) or (MarkerOrder <> 0) or
     not FileExists(AddBackslash(PriorDir) + 'unins000.dat') then exit;
  Result := '';
end;

function InitializeSetup: Boolean;
var Problem: string;
begin
  Problem := ReadUpgradeState(PreviousInstallDir, PreviousInstallVersion, InstallVersionOrder);
  Result := Problem = '';
  if not Result then begin
    Log('FormaView installation refused: ' + Problem);
    if not WizardSilent then SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);
  end;
end;

function UpgradeActionLabel: string;
begin
  if PreviousInstallDir = '' then Result := '安装'
  else if InstallVersionOrder > 0 then Result := '升级'
  else if InstallVersionOrder < 0 then Result := '覆盖安装'
  else Result := '重新安装';
end;

function SafeManifestName(Name: string): Boolean;
var I: Integer; Part: string;
begin
  Result := False;
  if (Name = '') or (Pos('..', Name) > 0) or (Pos(':', Name) > 0) or
     (Pos('/', Name) > 0) or (Pos('*', Name) > 0) or (Pos('?', Name) > 0) then exit;
  repeat
    I := Pos('\', Name);
    if I = 0 then Part := Name else Part := Copy(Name, 1, I - 1);
    if (Part = '') or (Copy(Part, Length(Part), 1) = '.') or
       (Copy(Part, Length(Part), 1) = ' ') then exit;
    if I > 0 then Delete(Name, 1, I);
  until I = 0;
  Result := True;
end;

function PlanRetiredPayload(Root, IncomingList: string): string;
var Incoming, Prior: TArrayOfString; Current: TStringList; I, Count, Handle: Integer; Name, Full: string;
begin
  SetArrayLength(RetiredFiles, 0);
  Result := '无法核对旧版安装文件清单。请使用适用于该旧版的升级包。';
  if not LoadStringsFromFile(IncomingList, Incoming) or
     not LoadStringsFromFile(AddBackslash(Root) + 'owned-files.txt', Prior) or
     (GetArrayLength(Incoming) = 0) or (GetArrayLength(Prior) = 0) then exit;
  Current := TStringList.Create;
  try
    Current.Sorted := True;
    for I := 0 to GetArrayLength(Incoming) - 1 do begin
      if not SafeManifestName(Incoming[I]) then exit;
      Current.Add(Lowercase(Incoming[I]));
    end;
    SetArrayLength(RetiredFiles, GetArrayLength(Prior));
    Count := 0;
    for I := 0 to GetArrayLength(Prior) - 1 do begin
      Name := Prior[I];
      if not SafeManifestName(Name) then exit;
      if Current.IndexOf(Lowercase(Name)) < 0 then begin
        Full := AddBackslash(Root) + Name;
        if ReparseInPath(Full) or DirExists(Full) then begin
          Result := '旧版安装清单中的路径与现有目录或链接冲突，请核对后再覆盖。' + #13#10 + Full;
          exit;
        end;
        if FileExists(Full) then begin
          Handle := CreateFileW(Full, $10080, 0, 0, 3, $80, 0);
          if Handle = -1 then begin
            Result := '旧版程序文件正在使用或无法移动。请关闭 FormaView 及其服务后重试。' + #13#10 + Full; exit;
          end;
          CloseHandle(Handle);
          RetiredFiles[Count] := Name; Count := Count + 1;
        end;
      end;
    end;
    SetArrayLength(RetiredFiles, Count);
    Result := '';
  finally
    Current.Free;
  end;
end;

function CreateRetiredDirectory(Path: string; Security: Integer): Boolean;
  external 'CreateDirectoryW@kernel32.dll stdcall delayload setuponly';

procedure RetirePriorFiles;
var Root, Parent, Source, Target, Stamp: string; I: Integer;
begin
  if GetArrayLength(RetiredFiles) = 0 then exit;
  Root := AddBackslash(ExpandConstant('{app}'));
  Parent := AddBackslash(ExtractFileDir(RemoveBackslashUnlessRoot(Root)));
  Stamp := GetDateTimeString('yyyymmddhhnnss', '-', '-');
  if ReparseInPath(Parent) then RaiseException('无法安全使用旧组件保留目录，未开始覆盖。');
  for I := 0 to 15 do begin
    Target := Parent + '.formaview-retired-' + Stamp + '-' + IntToStr(I);
    if CreateRetiredDirectory(Target, 0) then begin RetiredStorage := Target; break; end;
  end;
  if RetiredStorage = '' then RaiseException('无法准备旧组件的保留目录，未开始覆盖。');
  for I := 0 to GetArrayLength(RetiredFiles) - 1 do begin
    Source := Root + RetiredFiles[I];
    { Move on the same volume, without overwriting any destination. Keeping
      the bytes under an inert name permits recovery without mixing versions. }
    Target := AddBackslash(RetiredStorage) + RetiredFiles[I] + '.retired';
    if ReparseInPath(Source) or ReparseInPath(Target) or
       not ForceDirectories(ExtractFileDir(Target)) or not RenameFile(Source, Target) then
      RaiseException('无法移出旧程序文件，未开始复制新版文件：' + #13#10 + Source);
    RetiredCount := RetiredCount + 1;
    Log('FormaView retired previous file: ' + Source + ' -> ' + Target);
  end;
end;

procedure DeinitializeSetup;
var Source, Target: string; I: Integer; RestoreFailed: Boolean;
begin
  if RetiredCommitted or (RetiredStorage = '') then exit;
  RestoreFailed := False;
  for I := RetiredCount - 1 downto 0 do begin
    Source := AddBackslash(RetiredStorage) + RetiredFiles[I] + '.retired';
    Target := AddBackslash(ExpandConstant('{app}')) + RetiredFiles[I];
    if ReparseInPath(Source) or ReparseInPath(Target) or not RenameFile(Source, Target) then begin
      RestoreFailed := True;
      Log('FormaView could not restore retired file: ' + Source + ' -> ' + Target);
    end;
  end;
  RemoveDir(RetiredStorage); { only succeeds if empty }
  if RestoreFailed and not WizardSilent then
    SuppressibleMsgBox('覆盖未完成，部分旧文件仍保存在以下目录：' + #13#10 + RetiredStorage, mbError, MB_OK, IDOK);
end;
