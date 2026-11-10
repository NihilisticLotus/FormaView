const
  SettingsKey = 'Software\FormaView\FormaView';
  ProfileKey = 'Software\FormaView\Installer';
  LocalStatePath = '{localappdata}\FormaView\FormaView';

type
  TAttributeTag = record
    Attributes, Tag: LongWord;
  end;

var
  DeleteSettingsRequested, DeleteHistoryRequested, CleanupIncomplete: Boolean;

function WtsQuery(SessionServer: Integer; Session, Kind: LongWord;
  var Buffer: Integer; var ByteCount: LongWord): Boolean;
  external 'WTSQuerySessionInformationW@wtsapi32.dll stdcall delayload uninstallonly';
procedure WtsFree(Buffer: Integer);
  external 'WTSFreeMemory@wtsapi32.dll stdcall delayload uninstallonly';
function CopyWide(Dest: string; Source, Count: Integer): Integer;
  external 'lstrcpynW@kernel32.dll stdcall delayload uninstallonly';
function SamAccount(Kind: Integer; Name: string; var Count: LongWord): Boolean;
  external 'GetUserNameExW@secur32.dll stdcall delayload uninstallonly';
function FileTag(Handle, InfoClass: Integer; var Info: TAttributeTag; Size: LongWord): Boolean;
  external 'GetFileInformationByHandleEx@kernel32.dll stdcall delayload uninstallonly';
function MarkForDeletion(Handle, InfoClass: Integer; var Remove: Byte; Size: LongWord): Boolean;
  external 'SetFileInformationByHandle@kernel32.dll stdcall delayload uninstallonly';

function SessionPart(Kind: LongWord): string;
var Buffer: Integer; ByteCount: LongWord; S: string; N: Integer;
begin
  Result := '';
  if not WtsQuery(0, $FFFFFFFF, Kind, Buffer, ByteCount) then exit;
  try
    if (ByteCount < 4) or (ByteCount > 1024) then exit;
    SetLength(S, ByteCount div 2);
    CopyWide(S, Buffer, Length(S));
    N := Pos(#0, S);
    if N > 1 then Result := Copy(S, 1, N - 1);
  finally
    WtsFree(Buffer);
  end;
end;

function IsSessionAccount: Boolean;
var UserName, Domain, Account: string; Count: LongWord;
begin
  Result := False;
  UserName := SessionPart(5);
  Domain := SessionPart(7);
  if (UserName = '') or (Domain = '') then exit;
  Count := 512; SetLength(Account, Count);
  if SamAccount(2, Account, Count) then
    Result := CompareText(Copy(Account, 1, Count), Domain + '\' + UserName) = 0;
end;

function AddUninstallText(Form: TSetupForm; Text: string; Y, H: Integer): TNewStaticText;
begin
  Result := TNewStaticText.Create(Form);
  Result.Parent := Form; Result.AutoSize := False; Result.WordWrap := True;
  Result.SetBounds(ScaleX(18), ScaleY(Y), Form.ClientWidth - ScaleX(36), ScaleY(H));
  Result.Caption := Text;
end;

function ChooseUninstallData: Boolean;
var Form: TSetupForm; Heading: TNewStaticText; KeepSettings, KeepHistory: TNewCheckBox;
    Next, Cancel: TNewButton; CanChange: Boolean; Hint: string;
begin
  Result := True;
  if UninstallSilent then exit;
  CanChange := IsSessionAccount;
  Form := CreateCustomForm(384, 205, False, True);
  try
    Form.Caption := '卸载 FormaView · 用户数据';
    Form.BorderStyle := bsDialog;
    Form.Color := clWindow;
    Form.ClientWidth := ScaleX(384); Form.ClientHeight := ScaleY(211);
    Heading := AddUninstallText(Form, '选择卸载后要保留的数据', 14, 22);
    Heading.Font.Style := [fsBold];
    AddUninstallText(Form, '以下选项仅适用于当前 Windows 账户。', 43, 23);
    KeepSettings := TNewCheckBox.Create(Form); KeepSettings.Parent := Form;
    KeepSettings.Caption := '保留用户设置';
    KeepSettings.SetBounds(ScaleX(18), ScaleY(74), ScaleX(330), ScaleY(21));
    KeepSettings.Checked := True; KeepSettings.Enabled := CanChange;
    KeepHistory := TNewCheckBox.Create(Form); KeepHistory.Parent := Form;
    KeepHistory.Caption := '保留任务历史';
    KeepHistory.SetBounds(ScaleX(18), ScaleY(101), ScaleX(330), ScaleY(21));
    KeepHistory.Checked := True; KeepHistory.Enabled := CanChange;
    if CanChange then Hint := '取消勾选的项目将在确认卸载后删除。' + #13#10 + '原始素材和导出文件始终保留。'
    else Hint := '无法确认当前会话与卸载使用的是同一账户，用户数据将保留。' + #13#10 + '原始素材和导出文件始终保留。';
    AddUninstallText(Form, Hint, 133, 35);
    Next := TNewButton.Create(Form); Next.Parent := Form;
    Next.Caption := '继续'; Next.ModalResult := mrOk;
    Next.SetBounds(Form.ClientWidth - ScaleX(178), ScaleY(173), ScaleX(76), ScaleY(25));
    Cancel := TNewButton.Create(Form); Cancel.Parent := Form;
    Cancel.Caption := '取消'; Cancel.ModalResult := mrCancel; Cancel.Cancel := True;
    Cancel.SetBounds(Form.ClientWidth - ScaleX(94), ScaleY(173), ScaleX(76), ScaleY(25));
    Form.ActiveControl := Cancel;
    Form.FlipAndCenterIfNeeded(True, nil, False);
    Result := Form.ShowModal = mrOk;
    if Result and CanChange then begin
      DeleteSettingsRequested := not KeepSettings.Checked;
      DeleteHistoryRequested := not KeepHistory.Checked;
    end;
  finally
    Form.Free;
  end;
end;

procedure CleanupProblem(Detail: string);
begin
  CleanupIncomplete := True;
  Log('FormaView uninstall data cleanup skipped or incomplete: ' + Detail);
end;

function OpenStatePath(Path: string; Directory: Boolean; var Handle: Integer): Boolean;
var Info: TAttributeTag; Access: LongWord; ErrorCode: Integer;
begin
  Result := False;
  Access := $80; { FILE_READ_ATTRIBUTES }
  if not Directory then Access := Access or $10000; { DELETE }
  { Open the object itself, never its reparse target. No share-delete or
    share-write while validating and processing that path component. }
  Handle := CreateFileW('\\?\' + Path, Access, 1, 0, 3, $02200000, 0);
  if Handle = -1 then begin
    ErrorCode := DLLGetLastError;
    if (ErrorCode <> 2) and (ErrorCode <> 3) then CleanupProblem(Path + ' (Win32 ' + IntToStr(ErrorCode) + ')');
    exit;
  end;
  if not FileTag(Handle, 9, Info, 8) or ((Info.Attributes and $400) <> 0) or
     (((Info.Attributes and $10) <> 0) <> Directory) then begin
    CloseHandle(Handle);
    CleanupProblem(Path + ' (unexpected file type or reparse point)');
    exit;
  end;
  Result := True;
end;

procedure DeleteStateFile(Path: string);
var Handle: Integer; Remove: Byte;
begin
  if not OpenStatePath(Path, False, Handle) then exit;
  try
    Remove := 1;
    if not MarkForDeletion(Handle, 4, Remove, 1) then
      CleanupProblem(Path + ' (file removal failed)');
  finally
    CloseHandle(Handle);
  end;
end;

function CharactersFrom(S, Allowed: string): Boolean;
var I: Integer;
begin
  Result := False;
  if S = '' then exit;
  for I := 1 to Length(S) do if Pos(Copy(S, I, 1), Allowed) = 0 then exit;
  Result := True;
end;

function UuidName(S: string): Boolean;
begin
  Result := (Length(S) = 36) and (Copy(S, 9, 1) = '-') and (Copy(S, 14, 1) = '-') and
    (Copy(S, 19, 1) = '-') and (Copy(S, 24, 1) = '-') and
    CharactersFrom(Copy(S, 1, 8) + Copy(S, 10, 4) + Copy(S, 15, 4) + Copy(S, 20, 4) + Copy(S, 25, 12), '0123456789abcdef');
end;

function IsKnownJson(Name, Base: string): Boolean;
begin
  Result := (Name = Base) or
    ((Length(Name) = Length(Base) + 7) and (Copy(Name, 1, Length(Base) + 1) = Base + '.') and
     CharactersFrom(Copy(Name, Length(Base) + 2, 6),
        '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'));
end;

function FragmentName(Name, Prefix: string): Boolean;
var Dot: Integer;
begin
  Dot := Pos('.', Name);
  Result := (Copy(Name, 1, Length(Prefix)) = Prefix) and
    CharactersFrom(Copy(Name, Length(Prefix) + 1, Dot - Length(Prefix) - 1), '0123456789') and
    IsKnownJson(Name, Copy(Name, 1, Dot - 1) + '.json');
end;

procedure CleanJournal(Dir: string; Kind: Integer);
var H: Integer; Entry: TFindRec; N, P: string;
begin
  if not OpenStatePath(Dir, True, H) then exit;
  try
    if ((Kind = 1) and (GetFileAttributesW('\\?\' + AddBackslash(Dir) + 'service.lock') <> $FFFFFFFF)) or
       ((Kind = 2) and (GetFileAttributesW('\\?\' + AddBackslash(Dir) + 'groups.lock') <> $FFFFFFFF)) then begin
      CleanupProblem(Dir + ' (journal lock present)'); exit;
    end;
    if not FindFirst('\\?\' + AddBackslash(Dir) + '*', Entry) then begin
      CleanupProblem(Dir + ' (cannot enumerate journal directory)'); exit;
    end;
    try
      repeat
        N := Entry.Name;
        P := AddBackslash(Dir) + N;
        if (Entry.Attributes and $10) <> 0 then begin
          if (Kind = 0) and (Length(N) = 64) and CharactersFrom(N, '0123456789abcdef') then
            CleanJournal(P, 1)
          else if (Kind = 0) and (Length(N) = 71) and (Copy(N, 65, 7) = '-groups') and
                  CharactersFrom(Copy(N, 1, 64), '0123456789abcdef') then CleanJournal(P, 2)
          else if (Kind = 2) and UuidName(N) then CleanJournal(P, 3)
          else if (Kind = 3) and CharactersFrom(N, '0123456789') then CleanJournal(P, 4)
          else if (Kind = 4) and (N = 'records') then CleanJournal(P, 1);
        end else begin
          if ((Kind = 0) and IsKnownJson(N, 'desktop-history.json')) or
             ((Kind = 1) and UuidName(Copy(N, 1, 36)) and IsKnownJson(N, Copy(N, 1, 36) + '.json')) or
             ((Kind = 3) and (IsKnownJson(N, 'index.json') or IsKnownJson(N, 'protection.json') or
                              FragmentName(N, 'protected-') or FragmentName(N, 'source-guard-'))) or
             ((Kind = 4) and (IsKnownJson(N, 'requests.json') or IsKnownJson(N, 'direct-result.json'))) then
            DeleteStateFile(P);
        end;
      until not FindNext(Entry);
    finally
      FindClose(Entry);
    end;
  finally
    CloseHandle(H);
  end;
end;

procedure CleanWithAncestors(Path, Target: string);
var H: Integer; NextPath: string; Separator: Integer;
begin
  if Path = Target then begin CleanJournal(Target, 0); exit; end;
  if not OpenStatePath(Path, True, H) then exit;
  try
    NextPath := Copy(Target, Length(AddBackslash(Path)) + 1, Length(Target));
    Separator := Pos('\', NextPath);
    if Separator > 0 then NextPath := Copy(NextPath, 1, Separator - 1);
    if NextPath <> '' then CleanWithAncestors(AddBackslash(Path) + NextPath, Target);
  finally
    CloseHandle(H);
  end;
end;

procedure ApplyUninstallData;
var Layout, Target, Drive: string; Names: TArrayOfString; I: Integer;
begin
  if not DeleteSettingsRequested and not DeleteHistoryRequested then exit;
  if not IsSessionAccount then begin
    CleanupProblem('Session account could not be verified');
  end else begin
    if DeleteSettingsRequested then begin
      Names := ['language', 'preferences', 'recent'];
      for I := 0 to GetArrayLength(Names) - 1 do
        if RegValueExists(HKCU64, SettingsKey, Names[I]) and
           not RegDeleteValue(HKCU64, SettingsKey, Names[I]) then CleanupProblem(SettingsKey + '\' + Names[I]);
    end;
    if DeleteHistoryRequested then begin
      Target := ExpandConstant(LocalStatePath) + '\installed\image-journal';
      if RegQueryStringValue(HKCU64, ProfileKey, 'TaskHistoryLayout', Layout) and (Layout = 'legacy') then
        Target := ExpandConstant(LocalStatePath) + '\image-journal';
      Drive := ExtractFileDrive(Target);
      if (Length(Drive) = 2) and (Copy(Drive, 2, 1) = ':') and (Pos('..', Target) = 0) then
        CleanWithAncestors(AddBackslash(Drive), Target)
      else CleanupProblem('Unexpected local task history path');
    end;
  end;
  if CleanupIncomplete then
    SuppressibleMsgBox('部分选定数据未能删除。相关记录保留，原始素材和导出文件未受影响。', mbInformation, MB_OK, IDOK);
end;
