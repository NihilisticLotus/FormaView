const
  Ledger = 'Software\FormaView\M13AssociationOwnership';
  Caps = 'Software\FormaView\LocalFileCapabilities';
var
  AssocPage: TWizardPage;
  AssocList: TNewCheckListBox;
  AssocCount, AssocHint, SpaceInfo: TNewStaticText;
  ResultsPage: TOutputMsgMemoWizardPage;
  Extensions, Defaults: TArrayOfString;
  Rows: array[0..24] of Integer;
  Failed: array[0..24] of Boolean;
  RetryButton, DefaultsButton, RefreshButton: TNewButton;
  DefaultsOpened: Boolean;
  AssocReport: string;

function AssocQueryStringW(Flags, Kind: LongWord; Association, Extra: string;
  Output: string; var Length: LongWord): Integer;
  external 'AssocQueryStringW@shlwapi.dll stdcall';
procedure SHChangeNotify(Event, Flags: LongWord; Item1, Item2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

function QueryAssociation(Ext: string; Kind: LongWord): string;
var Buffer: string; Size: LongWord;
begin
  Size := 2048; SetLength(Buffer, Size);
  if AssocQueryStringW(0, Kind, Ext, 'open', Buffer, Size) = 0 then
    Result := Copy(Buffer, 1, Size - 1)
  else Result := '';
end;

function ProgId(I: Integer): string;
begin Result := 'FormaView.Local' + Extensions[I]; end;

function FormatDescription(I: Integer): string;
begin
  case I of
    0: Result := 'BMP 图像';
    1: Result := 'DIB 图像';
    2: Result := 'DDS 纹理';
    3: Result := 'OpenEXR 图像';
    4: Result := 'GIF 图像';
    5: Result := 'HDR 图像';
    6: Result := 'ICO 图标';
    7, 8, 9, 10: Result := 'JPEG 图像';
    11: Result := 'PNG 图像';
    12: Result := 'RGBE 图像';
    13: Result := 'TGA 图像';
    14, 15: Result := 'TIFF 图像';
    16: Result := 'WebP 图像';
    17: Result := 'FBX 模型';
    18: Result := 'glTF 模型';
    19: Result := 'GLB 模型';
    20: Result := 'OBJ 模型';
    21, 22: Result := 'ActorX 模型';
    23: Result := 'Alembic 几何缓存';
    24: Result := 'ActorX 动画 / 骨架';
  end;
end;

function DefaultDescription(I: Integer): string;
var Name, Id, Executable: string;
begin
  Id := QueryAssociation(Extensions[I], 20);
  Name := QueryAssociation(Extensions[I], 4);
  Executable := QueryAssociation(Extensions[I], 2);
  if (CompareText(ExtractFileName(Executable), 'OpenWith.exe') = 0) or (CompareText(Id, 'Unknown') = 0) then
    Result := '未设置默认应用'
  else if (Id = ProgId(I)) and (CompareText(ExtractFileName(Executable), 'formaview.exe') = 0) then Result := '当前默认：FormaView'
  else if Name <> '' then Result := '当前默认：' + Name
  else if Executable <> '' then Result := '当前默认：' + ExtractFileName(Executable)
  else if Id <> '' then Result := '已有关联（应用名称不可读取）'
  else Result := '未查得默认应用（可能未设置）';
end;

function CurrentOwner: string;
begin Result := RemoveBackslashUnlessRoot(ExpandConstant('{app}')); end;

function SameValue(Key, Name, Data: string): Boolean;
var Actual: string;
begin Result := RegQueryStringValue(HKCU64, Key, Name, Actual) and (Actual = Data); end;

function AllowedOwnedValue(Key, Name: string): Boolean;
var I: Integer; Ext, ClassKey: string; All: TArrayOfString;
begin
  Result := ((Key = Caps) and ((Name = 'ApplicationName') or (Name = 'ApplicationDescription'))) or
    ((Key = 'Software\RegisteredApplications') and (Name = 'FormaView.Local'));
  { Keep cleanup constrained even if an ownership record is damaged. }
  All := ['.bmp','.dib','.dds','.exr','.gif','.hdr','.ico','.jfif','.jpe','.jpeg','.jpg','.png',
    '.rgbe','.tga','.tif','.tiff','.webp','.fbx','.gltf','.glb','.obj','.psk','.pskx','.abc','.psa'];
  for I := 0 to GetArrayLength(All) - 1 do begin
    Ext := All[I]; ClassKey := 'Software\Classes\FormaView.Local' + Ext;
    Result := Result or ((Name = '') and ((Key = ClassKey) or (Key = ClassKey + '\DefaultIcon') or
      (Key = ClassKey + '\shell\open\command'))) or
      ((Key = 'Software\Classes\' + Ext + '\OpenWithProgids') and (Name = 'FormaView.Local' + Ext)) or
      ((Key = Caps + '\FileAssociations') and (Name = Ext));
  end;
end;

function PutOwned(Id, Key, Name, Data: string): Boolean;
var RecordKey, Ancestor, Parent: string;
begin
  Result := False;
  if not AllowedOwnedValue(Key, Name) then exit;
  RecordKey := Ledger + '\' + Id;
  if RegKeyExists(HKCU64, RecordKey) then begin
    if not SameValue(RecordKey, 'Owner', CurrentOwner) or
       not SameValue(RecordKey, 'Key', Key) or not SameValue(RecordKey, 'Name', Name) then exit;
    if not SameValue(Key, Name, Data) then exit;
    Result := True; exit;
  end;
  { A pre-existing value is not ours even if its text happens to match. }
  if RegValueExists(HKCU64, Key, Name) then exit;
  Ancestor := Key;
  if RegKeyExists(HKCU64, Ancestor) then Ancestor := ''
  else begin
    Parent := ExtractFileDir(Ancestor);
    while (Length(Parent) > 9) and not RegKeyExists(HKCU64, Parent) do begin
      Ancestor := Parent; Parent := ExtractFileDir(Parent);
    end;
  end;
  if not RegWriteStringValue(HKCU64, RecordKey, 'Owner', CurrentOwner) then exit;
  if not (RegWriteStringValue(HKCU64, RecordKey, 'Key', Key) and
          RegWriteStringValue(HKCU64, RecordKey, 'Name', Name) and
          RegWriteStringValue(HKCU64, RecordKey, 'Data', Data) and
          RegWriteStringValue(HKCU64, RecordKey, 'CreatedAncestor', Ancestor)) then begin
    RegDeleteKeyIncludingSubkeys(HKCU64, RecordKey); exit;
  end;
  Result := RegWriteStringValue(HKCU64, Key, Name, Data);
  if not Result then RegDeleteKeyIncludingSubkeys(HKCU64, RecordKey);
end;

procedure RemoveOwned(Prefix: string);
var Names, Keys, Ancestors: TArrayOfString; I, J: Integer;
    RecordKey, Key, Name, Data, Ancestor, Parent: string;
begin
  if not RegGetSubkeyNames(HKCU64, Ledger, Names) then exit;
  SetArrayLength(Keys, GetArrayLength(Names));
  SetArrayLength(Ancestors, GetArrayLength(Names));
  for I := 0 to GetArrayLength(Names) - 1 do begin
    RecordKey := Ledger + '\' + Names[I];
    if ((Prefix = '') or (Pos(Prefix, Names[I]) = 1)) and
       SameValue(RecordKey, 'Owner', CurrentOwner) then begin
      if RegQueryStringValue(HKCU64, RecordKey, 'Key', Key) and
         RegQueryStringValue(HKCU64, RecordKey, 'Name', Name) and
         RegQueryStringValue(HKCU64, RecordKey, 'Data', Data) then begin
        if not AllowedOwnedValue(Key, Name) then begin
          Log('Invalid ownership scope retained: ' + RecordKey); continue;
        end;
        if SameValue(Key, Name, Data) then begin
          if not RegDeleteValue(HKCU64, Key, Name) then begin
            Log('Association cleanup failed, retained ownership: ' + Key + '\' + Name);
            continue;
          end;
        end else Log('Association changed externally; preserved: ' + Key + '\' + Name);
        RegQueryStringValue(HKCU64, RecordKey, 'CreatedAncestor', Ancestor);
        Keys[I] := Key; Ancestors[I] := Ancestor;
        { Only these known ledger fields are removed; foreign additions survive. }
        RegDeleteValue(HKCU64, RecordKey, 'Owner');
        RegDeleteValue(HKCU64, RecordKey, 'Key');
        RegDeleteValue(HKCU64, RecordKey, 'Name');
        RegDeleteValue(HKCU64, RecordKey, 'Data');
        RegDeleteValue(HKCU64, RecordKey, 'CreatedAncestor');
        RegDeleteKeyIfEmpty(HKCU64, RecordKey);
      end;
    end;
  end;
  { Revisit ancestors after all owned values are gone. Never remove a populated key. }
  for J := 0 to 1 do
    for I := 0 to GetArrayLength(Keys) - 1 do begin
      Key := Keys[I]; Ancestor := Ancestors[I];
      if (Ancestor <> '') and ((Pos('Software\Classes\', Ancestor) = 1) or
          (Pos('Software\FormaView\', Ancestor) = 1) or (Ancestor = 'Software\RegisteredApplications')) then
        while (Length(Key) >= Length(Ancestor)) and (Pos(Ancestor, Key) = 1) do begin
          RegDeleteKeyIfEmpty(HKCU64, Key);
          Parent := ExtractFileDir(Key);
          if Parent = Key then break;
          Key := Parent;
        end;
    end;
  RegDeleteKeyIfEmpty(HKCU64, Ledger);
end;

function RegisterExtension(I: Integer): Boolean;
var Id, ClassKey, Command: string;
begin
  Id := Copy(Extensions[I], 2, 20) + '_';
  ClassKey := 'Software\Classes\' + ProgId(I);
  Command := '"' + ExpandConstant('{app}\formaview.exe') + '" --open "%1"';
  Result := PutOwned(Id + 'description', ClassKey, '', 'FormaView ' + Extensions[I]) and
    PutOwned(Id + 'icon', ClassKey + '\DefaultIcon', '', '"' + ExpandConstant('{app}\formaview.exe') + '",0') and
    PutOwned(Id + 'command', ClassKey + '\shell\open\command', '', Command) and
    PutOwned(Id + 'openwith', 'Software\Classes\' + Extensions[I] + '\OpenWithProgids', ProgId(I), '') and
    PutOwned(Id + 'capability', Caps + '\FileAssociations', Extensions[I], ProgId(I));
  if not Result then RemoveOwned(Id);
end;

procedure SelectionChanged(Sender: TObject);
var I, Count, Images, Models, Animation: Integer;
begin
  Count := 0; Images := 0; Models := 0; Animation := 0;
  for I := 0 to 24 do if AssocList.Checked[Rows[I]] then begin
    Count := Count + 1;
    if I < 17 then Images := Images + 1
    else if I < 24 then Models := Models + 1
    else Animation := Animation + 1;
  end;
  AssocCount.Caption := '已选 ' + IntToStr(Count) + ' / 25：图片 ' + IntToStr(Images) +
    '/17，模型 ' + IntToStr(Models) + '/7，动画/骨架 ' + IntToStr(Animation) + '/1。' + #13#10 +
    '安装后请在 Windows 中确认默认打开程序。';
end;

procedure ChoosePreset(Sender: TObject);
var I, Mode: Integer; Common: string;
begin
  Mode := TNewButton(Sender).Tag;
  Common := ',.png,.jpg,.jpeg,.bmp,.tga,.dds,.fbx,.obj,.gltf,.glb,.psk,.pskx,.psa,';
  for I := 0 to 24 do
    if Mode <> 2 then AssocList.Checked[Rows[I]] := (Mode = 0) or
      ((Mode = 1) and (Pos(',' + Extensions[I] + ',', Common) > 0));
  if Mode = 2 then WizardForm.ActiveControl := AssocList;
  SelectionChanged(Sender);
end;

procedure ChooseGroup(Sender: TObject);
var I, Mode, First, Last: Integer; Selected: Boolean;
begin
  Mode := TNewButton(Sender).Tag;
  case Mode div 2 of
    0: begin First := 0; Last := 16; end;
    1: begin First := 17; Last := 23; end;
    2: begin First := 24; Last := 24; end;
    else exit;
  end;
  Selected := (Mode mod 2) = 0;
  for I := First to Last do AssocList.Checked[Rows[I]] := Selected;
  SelectionChanged(Sender);
end;

procedure OpenDefaults(Sender: TObject);
var ErrorCode: Integer; Version: TWindowsVersion; Uri: string;
begin
  GetWindowsVersionEx(Version);
  Uri := 'ms-settings:defaultapps';
  if Version.Build >= 22000 then Uri := Uri + '?registeredAppUser=FormaView.Local';
  if not ShellExec('open', Uri, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode) then
    MsgBox('请打开 Windows 设置 → 应用 → 默认应用，找到 FormaView 并确认所选格式。', mbInformation, MB_OK);
end;

procedure RefreshDefaults(Sender: TObject);
var I, Pending, Done, FailedCount: Integer; Status: string;
begin
  Pending := 0; Done := 0; FailedCount := 0;
  AssocReport := '所选文件格式的默认打开程序：' + #13#10;
  for I := 0 to 24 do
    if AssocList.Checked[Rows[I]] then begin
      if Failed[I] then begin
        Status := '关联准备失败，可在完成页重试'; FailedCount := FailedCount + 1;
      end
      else if (QueryAssociation(Extensions[I], 20) = ProgId(I)) and
        (CompareText(QueryAssociation(Extensions[I], 2), ExpandConstant('{app}\formaview.exe')) = 0) then begin
        Status := '已默认使用 FormaView 打开'; Done := Done + 1;
      end else begin
        Status := '等待 Windows 确认；' + DefaultDescription(I); Pending := Pending + 1;
      end;
      AssocReport := AssocReport + Extensions[I] + ' · ' + FormatDescription(I) + '：' + Status + #13#10;
    end;
  if (Done + Pending = 0) then AssocReport := AssocReport + '没有已完成或待系统确认的关联。' + #13#10;
  AssocReport := AssocReport + #13#10 + '已设为默认：' + IntToStr(Done) + '；待系统确认：' + IntToStr(Pending) +
    '；注册失败：' + IntToStr(FailedCount) + '。';
  ResultsPage.RichEditViewer.Text := AssocReport;
  WizardForm.FinishedLabel.Caption := 'FormaView 0.1.0 已安装。' + #13#10 + '位置：' + ExpandConstant('{app}') + #13#10 +
    '已设为默认：' + IntToStr(Done) + ' 个格式；待系统确认：' + IntToStr(Pending) + ' 个格式。';
  if FailedCount > 0 then WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10 +
    IntToStr(FailedCount) + ' 项关联准备失败，可点击下方按钮重试。';
  Log(AssocReport);
end;

procedure ApplyAssociations(RetryOnly: Boolean);
var I, Count: Integer; SharedOk: Boolean; Status: string;
begin
  if not RetryOnly then begin
    RemoveOwned('');
    for I := 0 to 24 do Failed[I] := False;
  end;
  Count := 0;
  for I := 0 to 24 do if AssocList.Checked[Rows[I]] then Count := Count + 1;
  SharedOk := True;
  if Count > 0 then
    SharedOk := PutOwned('shared_name', Caps, 'ApplicationName', 'FormaView') and
      PutOwned('shared_description', Caps, 'ApplicationDescription', '预览图片、模型、动画和 Alembic 缓存文件。') and
      PutOwned('shared_registration', 'Software\RegisteredApplications', 'FormaView.Local', Caps);
  AssocReport := '文件关联结果' + #13#10 +
    '注册成功后，请在 Windows 中确认默认打开程序。' + #13#10;
  for I := 0 to 24 do begin
    if AssocList.Checked[Rows[I]] then begin
      if not RetryOnly or Failed[I] then Failed[I] := not (SharedOk and RegisterExtension(I));
      if Failed[I] then Status := '关联准备失败；可重试或跳过'
      else Status := '已准备好，等待系统默认应用确认';
    end else Status := '未选择；未新增注册';
    AssocReport := AssocReport + Extensions[I] + ': ' + Status + '；' + DefaultDescription(I) + #13#10;
  end;
  SHChangeNotify($08000000, 0, 0, 0);
  Log(AssocReport);
  ResultsPage.RichEditViewer.Text := AssocReport;
  RefreshDefaults(nil);
  Count := 0;
  for I := 0 to 24 do if Failed[I] then Count := Count + 1;
  RetryButton.Visible := Count > 0;
end;

procedure RetryAssociations(Sender: TObject);
begin ApplyAssociations(True); end;

procedure RefreshSpace(Sender: TObject);
var Path, Parent: string; FreeSpace, TotalSpace: Int64;
begin
  Path := WizardDirValue;
  while not DirExists(Path) and (Length(Path) > 3) do begin
    Parent := ExtractFileDir(Path);
    if Parent = Path then break;
    Path := Parent;
  end;
  if GetSpaceOnDisk64(Path, FreeSpace, TotalSpace) then
    SpaceInfo.Caption := '目标卷：' + ExtractFileDrive(Path) + '   可用：' + IntToStr(FreeSpace div 1048576) +
      ' MiB   总容量：' + IntToStr(TotalSpace div 1048576) + ' MiB' + #13#10 +
      '程序文件约 {#PayloadMiB} MiB，安装前另检查 16 MiB 临时空间余量。'
  else SpaceInfo.Caption := '目标卷空间暂不可读取；请确认目录与权限，安装前会再次检查。';
end;

function ExtensionList: TArrayOfString;
var Text: string; At, Count: Integer;
begin
  Text := '.bmp,.dib,.dds,.exr,.gif,.hdr,.ico,.jfif,.jpe,.jpeg,.jpg,.png,.rgbe,.tga,.tif,.tiff,.webp,.fbx,.gltf,.glb,.obj,.psk,.pskx,.abc,.psa,';
  SetArrayLength(Result, 25); Count := 0;
  while Text <> '' do begin
    At := Pos(',', Text); Result[Count] := Copy(Text, 1, At - 1);
    Delete(Text, 1, At); Count := Count + 1;
  end;
end;

procedure InitializeWizard;
var I: Integer; Button: TNewButton; Group, Param: string;
begin
  Extensions := ExtensionList;
  SetArrayLength(Defaults, 25);
  WizardForm.WelcomeLabel2.Caption := 'FormaView 0.1.0 · Windows x64' + #13#10#13#10 +
    '此向导将帮助你安装 FormaView。' + #13#10 +
    '你可以选择安装位置、快捷方式和默认打开的文件格式。';
  SpaceInfo := TNewStaticText.Create(WizardForm); SpaceInfo.Parent := WizardForm.SelectDirPage;
  SpaceInfo.SetBounds(0, WizardForm.SelectDirPage.Height - ScaleY(100), WizardForm.SelectDirPage.Width, ScaleY(55));
  SpaceInfo.WordWrap := True; WizardForm.DirEdit.OnChange := @RefreshSpace; RefreshSpace(nil);
  AssocPage := CreateCustomPage(wpSelectTasks, '文件关联', '选择希望默认使用 FormaView 打开的文件格式。');
  AssocHint := TNewStaticText.Create(WizardForm); AssocHint.Parent := AssocPage.Surface;
  AssocHint.SetBounds(0, 0, AssocPage.SurfaceWidth, ScaleY(34)); AssocHint.WordWrap := True;
  AssocHint.Caption := '选择文件格式。安装后 Windows 会请你确认默认应用；确认后，双击这些文件即可用 FormaView 打开。';
  for I := 0 to 3 do begin
    Button := TNewButton.Create(WizardForm); Button.Parent := AssocPage.Surface;
    Button.SetBounds(I * (AssocPage.SurfaceWidth div 4), ScaleY(38), AssocPage.SurfaceWidth div 4 - ScaleX(5), ScaleY(27));
    case I of 0: Button.Caption := '全部支持格式（25）'; 1: Button.Caption := '主要格式（13）';
      2: Button.Caption := '自定义选择'; 3: Button.Caption := '全不选 / 不关联'; end;
    Button.Tag := I; Button.OnClick := @ChoosePreset;
  end;
  { Separate group actions never replace selections in the other two groups. }
  for I := 0 to 5 do begin
    Button := TNewButton.Create(WizardForm); Button.Parent := AssocPage.Surface;
    Button.SetBounds(I * (AssocPage.SurfaceWidth div 6), ScaleY(73), AssocPage.SurfaceWidth div 6 - ScaleX(5), ScaleY(27));
    case I of
      0: Button.Caption := '图片全选'; 1: Button.Caption := '图片清空';
      2: Button.Caption := '模型全选'; 3: Button.Caption := '模型清空';
      4: Button.Caption := '动画全选'; 5: Button.Caption := '动画清空';
    end;
    Button.Tag := I; Button.OnClick := @ChooseGroup;
  end;
  AssocList := TNewCheckListBox.Create(WizardForm); AssocList.Parent := AssocPage.Surface;
  AssocList.SetBounds(0, ScaleY(108), AssocPage.SurfaceWidth, AssocPage.SurfaceHeight - ScaleY(147));
  AssocList.OnClickCheck := @SelectionChanged;
  Param := ',' + Lowercase(ExpandConstant('{param:ASSOC|}')) + ',';
  for I := 0 to 24 do begin
    Group := '';
    case I of 0: Group := '图片预览（17）'; 17: Group := '模型与缓存预览（7）';
      24: Group := '动画 / 骨架（1）'; end;
    if Group <> '' then AssocList.AddGroup(Group, '', 0, nil);
    Defaults[I] := DefaultDescription(I);
    Rows[I] := AssocList.AddCheckBox(Extensions[I] + '  ·  ' + FormatDescription(I) + '  ·  ' + Defaults[I], '', 0,
        Pos(',' + Extensions[I] + ',', Param) > 0, True, False, False, nil);
  end;
  AssocCount := TNewStaticText.Create(WizardForm); AssocCount.Parent := AssocPage.Surface;
  AssocCount.SetBounds(0, AssocPage.SurfaceHeight - ScaleY(31), AssocPage.SurfaceWidth, ScaleY(30));
  AssocCount.WordWrap := True; SelectionChanged(nil);
  ResultsPage := CreateOutputMsgMemoPage(wpInstalling, '完成文件关联',
    '在 Windows 中确认默认应用，然后返回这里刷新状态。',
    '选择 FormaView 作为所选格式的默认应用。你也可以暂时跳过，以后在 Windows 设置中更改。', '');
  ResultsPage.RichEditViewer.Height := ResultsPage.RichEditViewer.Height - ScaleY(38);
  RetryButton := TNewButton.Create(WizardForm); RetryButton.Parent := WizardForm.FinishedPage;
  RetryButton.SetBounds(ScaleX(180), ScaleY(230), ScaleX(190), ScaleY(27));
  RetryButton.Caption := '仅重试失败的关联'; RetryButton.OnClick := @RetryAssociations; RetryButton.Visible := False;
  DefaultsButton := TNewButton.Create(WizardForm); DefaultsButton.Parent := ResultsPage.Surface;
  DefaultsButton.SetBounds(0, ResultsPage.SurfaceHeight - ScaleY(29), ScaleX(225), ScaleY(27));
  DefaultsButton.Caption := '设置所选格式的默认程序'; DefaultsButton.OnClick := @OpenDefaults;
  RefreshButton := TNewButton.Create(WizardForm); RefreshButton.Parent := ResultsPage.Surface;
  RefreshButton.SetBounds(ScaleX(235), ResultsPage.SurfaceHeight - ScaleY(29), ScaleX(150), ScaleY(27));
  RefreshButton.Caption := '刷新关联结果'; RefreshButton.OnClick := @RefreshDefaults;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: string): string;
var I, Selected: Integer;
begin
  Selected := 0;
  Result := MemoDirInfo + NewLine + MemoComponentsInfo + NewLine + MemoTasksInfo + NewLine +
    '希望默认用 FormaView 打开的格式：' + NewLine;
  for I := 0 to 24 do
    if AssocList.Checked[Rows[I]] then begin
      Result := Result + Space + Extensions[I] + ' · ' + FormatDescription(I) + '；' + DefaultDescription(I) + NewLine;
      Selected := Selected + 1;
    end;
  if Selected > 0 then Result := Result + '共 ' + IntToStr(Selected) + ' 个格式。安装后请在 Windows 默认应用设置中确认以上选择。' + NewLine
  else Result := Result + Space + '不关联文件格式。' + NewLine;
  Result := Result + '用户设置、任务及素材文件保留。';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
var I: Integer;
begin
  Result := False;
  if PageID = ResultsPage.ID then begin
    Result := True;
    for I := 0 to 24 do
      if AssocList.Checked[Rows[I]] then Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then ApplyAssociations(False);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveOwned(''); SHChangeNotify($08000000, 0, 0, 0);
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
var I: Integer; NeedDefaults: Boolean;
begin
  if CurPageID = wpSelectDir then RefreshSpace(nil);
  if CurPageID = ResultsPage.ID then begin
    RefreshDefaults(nil);
    NeedDefaults := False;
    for I := 0 to 24 do
      if AssocList.Checked[Rows[I]] and not Failed[I] and
        ((QueryAssociation(Extensions[I], 20) <> ProgId(I)) or
         (CompareText(QueryAssociation(Extensions[I], 2), ExpandConstant('{app}\formaview.exe')) <> 0)) then
        NeedDefaults := True;
    if NeedDefaults and not DefaultsOpened and not WizardSilent then begin
      DefaultsOpened := True; OpenDefaults(nil);
    end;
  end;
  if CurPageID = wpFinished then RefreshDefaults(nil);
end;
