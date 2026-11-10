function SelectedFormats: Integer;
var I: Integer;
begin
  Result := 0;
  for I := 0 to 24 do if AssocList.Checked[Rows[I]] then Result := Result + 1;
end;

procedure SelectionChanged(Sender: TObject);
begin
  AssocCount.Caption := '已选 ' + IntToStr(SelectedFormats) + ' / 25 个预览格式';
end;

procedure ChoosePreset(Sender: TObject);
var I, Mode: Integer; Common: string;
begin
  Mode := TNewButton(Sender).Tag;
  Common := ',.png,.jpg,.jpeg,.bmp,.tga,.dds,.fbx,.obj,.gltf,.glb,.psk,.pskx,.psa,';
  for I := 0 to 24 do
    AssocList.Checked[Rows[I]] := (Mode = 2) or
      ((Mode = 1) and (Pos(',' + Extensions[I] + ',', Common) > 0));
  SelectionChanged(Sender);
end;

procedure OpenDefaults(Sender: TObject);
var ErrorCode: Integer; Version: TWindowsVersion; Uri: string; Opened: Boolean;
begin
  GetWindowsVersionEx(Version);
  Uri := 'ms-settings:defaultapps';
  if Version.Build >= 22000 then begin
    if IsAdminInstallMode then Uri := Uri + '?registeredAppMachine=FormaView.Local'
    else Uri := Uri + '?registeredAppUser=FormaView.Local';
  end;
  if IsAdminInstallMode then
    Opened := ShellExecAsOriginalUser('open', Uri, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode)
  else
    Opened := ShellExec('open', Uri, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
  if not Opened then
    MsgBox('请在自己的账户中打开 Windows 设置 → 应用 → 默认应用，搜索 FormaView，然后确认所选格式。', mbInformation, MB_OK);
end;

function ConfirmedDefault(I: Integer): Boolean;
begin
  Result := False;
  { An elevated setup may run under a different account. Never report that
    account's defaults as the original user's choices. }
  if not IsAdminInstallMode then
    Result := (QueryAssociation(Extensions[I], 20) = ProgId(I)) and
      (CompareText(QueryAssociation(Extensions[I], 2), ExpandConstant('{app}\formaview.exe')) = 0);
end;

procedure RefreshDefaults(Sender: TObject);
var I, Pending, Done, FailedCount: Integer; Status: string;
begin
  Pending := 0; Done := 0; FailedCount := 0;
  AssocReport := '';
  for I := 0 to 24 do if AssocList.Checked[Rows[I]] then begin
    if Failed[I] then begin
      Status := '注册未完成'; FailedCount := FailedCount + 1;
    end else if ConfirmedDefault(I) then begin
      Status := '已设为默认应用'; Done := Done + 1;
    end else begin
      Status := '已注册，请在 Windows 设置中确认'; Pending := Pending + 1;
    end;
    AssocReport := AssocReport + Extensions[I] + '   ' + FormatDescription(I) + '   ·   ' + Status + #13#10;
  end;
  ResultsPage.RichEditViewer.Text := AssocReport;
  DefaultsButton.Enabled := (Pending + Done) > 0;
  RetryButton.Visible := FailedCount > 0;
  WizardForm.FinishedLabel.Caption := 'FormaView 已就绪。';
  if IsAdminInstallMode and (Pending > 0) then
    WizardForm.FinishedLabel.Caption := '各账户可在 Windows 的默认应用设置中确认文件格式。'
  else if Pending > 0 then
    WizardForm.FinishedLabel.Caption := '还有 ' + IntToStr(Pending) + ' 个格式可在 Windows 设置中确认。';
  Log('File associations: ' + IntToStr(Done) + ' confirmed, ' + IntToStr(Pending) +
    ' registered, ' + IntToStr(FailedCount) + ' failed.' + #13#10 + AssocReport);
end;

procedure ApplyAssociations(RetryOnly: Boolean);
var I: Integer; SharedOk: Boolean;
begin
  if not RetryOnly then begin
    for I := 0 to 24 do begin
      Failed[I] := False;
      if not AssocList.Checked[Rows[I]] then RemoveOwned(Copy(Extensions[I], 2, 20) + '_');
    end;
    if SelectedFormats = 0 then RemoveOwned('shared_');
  end;
  SharedOk := True;
  if SelectedFormats > 0 then
    SharedOk := PutOwned('shared_name', Caps, 'ApplicationName', 'FormaView') and
      PutOwned('shared_description', Caps, 'ApplicationDescription', '预览图片、模型、动画和 Alembic 缓存文件。') and
      PutOwned('shared_registration', 'Software\RegisteredApplications', 'FormaView.Local', Caps);
  for I := 0 to 24 do
    if AssocList.Checked[Rows[I]] and (not RetryOnly or Failed[I]) then
      Failed[I] := not (SharedOk and RegisterExtension(I));
  SHChangeNotify($08000000, 0, 0, 0);
  RefreshDefaults(nil);
end;

procedure RetryAssociations(Sender: TObject);
begin ApplyAssociations(True); end;

procedure RefreshSpace(Sender: TObject);
var Path, Parent, LocationHint: string; FreeSpace, TotalSpace: Int64;
begin
  Path := WizardDirValue;
  while not DirExists(Path) and (Length(Path) > 3) do begin
    Parent := ExtractFileDir(Path);
    if Parent = Path then break;
    Path := Parent;
  end;
  if PreviousInstallDir <> '' then LocationHint := '沿用现有安装目录。安装范围：' + InstallScopeLabel + '。'
  else if IsAdminInstallMode then LocationHint := '此安装供本机所有用户使用，程序保存在 Program Files。'
  else LocationHint := '此安装仅供当前账户使用。可点击“浏览”更换安装目录。';
  if GetSpaceOnDisk64(Path, FreeSpace, TotalSpace) then
    SpaceInfo.Caption := LocationHint + #13#10#13#10 +
      '需要 ' + IntToStr({#PayloadMiB} + 16) + ' MiB（含临时空间）   ·   可用 ' +
      IntToStr(FreeSpace div 1048576) + ' MiB'
  else SpaceInfo.Caption := LocationHint + #13#10#13#10 + '暂时无法读取可用空间，安装前会再次检查。';
end;

function PreviousAssociation(I: Integer): Boolean;
var PreviousPath, Owner: string;
begin
  Result := RegQueryStringValue(InstallRootKey, UninstallKey, 'InstallLocation', PreviousPath) and
    RegQueryStringValue(InstallRootKey, Ledger + '\' + Copy(Extensions[I], 2, 20) + '_command', 'Owner', Owner) and
    (CompareText(RemoveBackslashUnlessRoot(PreviousPath), Owner) = 0);
end;

procedure InitializeWizard;
var I: Integer; Button: TNewButton; Group, Param, LabelText: string; Checked: Boolean;
begin
  Extensions := ExtensionList;
  WizardForm.WelcomeLabel2.Caption := '版本 {#PackageVersion}   ·   Windows x64' + #13#10#13#10 + WizardForm.WelcomeLabel2.Caption;
  if PreviousInstallDir <> '' then begin
    WizardForm.WelcomeLabel1.Caption := UpgradeActionLabel + ' FormaView';
    WizardForm.WelcomeLabel2.Caption := '已安装：' + PreviousInstallVersion + '   →   将安装：{#PackageVersion}' + #13#10#13#10 +
      '沿用原安装范围和目录，保留用户设置和任务历史。' + #13#10#13#10 +
      '如果 FormaView 正在运行，请先保存工作并关闭程序。';
  end;
  WizardForm.ReadyMemo.WordWrap := True;
  WizardForm.ReadyMemo.ScrollBars := ssVertical;
  WizardForm.ReadyMemo.Color := $FFFFFF;
  WizardForm.DiskSpaceLabel.Visible := False;
  if IsAdminInstallMode or (PreviousInstallDir <> '') then begin
    WizardForm.DirEdit.ReadOnly := True;
    WizardForm.DirBrowseButton.Visible := False;
  end;
  SpaceInfo := TNewStaticText.Create(WizardForm); SpaceInfo.Parent := WizardForm.SelectDirPage;
  SpaceInfo.SetBounds(0, WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(18),
    WizardForm.SelectDirPage.Width, ScaleY(75));
  SpaceInfo.Anchors := [akLeft, akTop, akRight]; SpaceInfo.AutoSize := False; SpaceInfo.WordWrap := True;
  WizardForm.DirEdit.OnChange := @RefreshSpace; RefreshSpace(nil);
  WizardForm.SelectTasksLabel.Height := ScaleY(49);
  WizardForm.TasksList.Top := ScaleY(60);
  WizardForm.TasksList.Height := ScaleY(70);

  AssocPage := CreateCustomPage(wpSelectTasks, '文件关联', '选择希望双击后用 FormaView 打开的文件格式。');
  AssocHint := TNewStaticText.Create(WizardForm); AssocHint.Parent := AssocPage.Surface;
  AssocHint.SetBounds(0, 0, AssocPage.SurfaceWidth, ScaleY(32));
  AssocHint.Anchors := [akLeft, akTop, akRight]; AssocHint.AutoSize := False; AssocHint.WordWrap := True;
  AssocHint.Caption := '选择预设或逐项勾选。安装后，在 Windows 的默认应用设置中确认。';
  for I := 0 to 2 do begin
    Button := TNewButton.Create(WizardForm); Button.Parent := AssocPage.Surface;
    Button.SetBounds(I * ScaleX(107), ScaleY(34), ScaleX(100), ScaleY(26));
    case I of
      0: Button.Caption := '不关联';
      1: Button.Caption := '常用格式（13）';
      2: Button.Caption := '全部格式（25）';
    end;
    Button.Tag := I; Button.OnClick := @ChoosePreset;
  end;
  AssocList := TNewCheckListBox.Create(WizardForm); AssocList.Parent := AssocPage.Surface;
  AssocList.SetBounds(0, ScaleY(70), AssocPage.SurfaceWidth, AssocPage.SurfaceHeight - ScaleY(95));
  AssocList.Anchors := [akLeft, akTop, akRight, akBottom];
  AssocList.OnClickCheck := @SelectionChanged;
  Param := Lowercase(ExpandConstant('{param:ASSOC|#auto#}'));
  for I := 0 to 24 do begin
    Group := '';
    case I of
      0: Group := '图片（17）';
      17: Group := '模型与缓存（7）';
      24: Group := '动画与骨架（1）';
    end;
    if Group <> '' then AssocList.AddGroup(Group, '', 0, nil);
    LabelText := Extensions[I] + '   ·   ' + FormatDescription(I);
    if not IsAdminInstallMode then LabelText := LabelText + '   ·   ' + DefaultDescription(I);
    if Param = '#auto#' then Checked := PreviousAssociation(I)
    else Checked := Pos(',' + Extensions[I] + ',', ',' + Param + ',') > 0;
    Rows[I] := AssocList.AddCheckBox(LabelText, '', 1, Checked, True, False, False, nil);
  end;
  AssocCount := TNewStaticText.Create(WizardForm); AssocCount.Parent := AssocPage.Surface;
  AssocCount.SetBounds(0, AssocPage.SurfaceHeight - ScaleY(19), AssocPage.SurfaceWidth, ScaleY(19));
  AssocCount.Anchors := [akLeft, akRight, akBottom]; SelectionChanged(nil);
  ResultsPage := CreateOutputMsgMemoPage(wpInstalling, '确认默认应用',
    '确认后，双击所选格式即可用 FormaView 打开。',
    '在 Windows 设置中选择所需格式，也可以稍后设置。', '');
  ResultsPage.RichEditViewer.Height := ResultsPage.RichEditViewer.Height - ScaleY(39);
  DefaultsButton := TNewButton.Create(WizardForm); DefaultsButton.Parent := ResultsPage.Surface;
  DefaultsButton.SetBounds(0, ResultsPage.SurfaceHeight - ScaleY(28), ScaleX(118), ScaleY(27));
  DefaultsButton.Anchors := [akLeft, akBottom];
  DefaultsButton.Caption := '打开默认应用设置'; DefaultsButton.OnClick := @OpenDefaults;
  RefreshButton := TNewButton.Create(WizardForm); RefreshButton.Parent := ResultsPage.Surface;
  RefreshButton.SetBounds(ScaleX(126), ResultsPage.SurfaceHeight - ScaleY(28), ScaleX(86), ScaleY(27));
  RefreshButton.Anchors := [akLeft, akBottom];
  RefreshButton.Caption := '刷新状态'; RefreshButton.OnClick := @RefreshDefaults;
  RefreshButton.Visible := not IsAdminInstallMode;
  RetryButton := TNewButton.Create(WizardForm); RetryButton.Parent := ResultsPage.Surface;
  RetryButton.SetBounds(ScaleX(220), ResultsPage.SurfaceHeight - ScaleY(28), ScaleX(90), ScaleY(27));
  RetryButton.Anchors := [akLeft, akBottom];
  RetryButton.Caption := '重试失败项'; RetryButton.OnClick := @RetryAssociations; RetryButton.Visible := False;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: string): string;
var I, ItemsOnLine: Integer; Shortcuts: string;
begin
  Shortcuts := '';
  if WizardIsTaskSelected('startmenu') then Shortcuts := '开始菜单';
  if WizardIsTaskSelected('desktopicon') then begin
    if Shortcuts <> '' then Shortcuts := Shortcuts + '、';
    Shortcuts := Shortcuts + '桌面';
  end;
  if Shortcuts = '' then Shortcuts := '不创建';
  Result := '安装范围' + NewLine + Space + InstallScopeLabel + NewLine + NewLine +
    '安装目录' + NewLine + Space + WizardDirValue + NewLine + NewLine +
    '快捷方式' + NewLine + Space + Shortcuts + NewLine + NewLine;
  if PreviousInstallDir <> '' then
    Result := UpgradeActionLabel + '版本' + NewLine + Space + PreviousInstallVersion +
      '  →  {#PackageVersion}' + NewLine + NewLine + Result;
  if SelectedFormats = 0 then begin
    Result := Result + '文件关联' + NewLine + Space + '不关联';
    exit;
  end;
  Result := Result + '文件关联（' + IntToStr(SelectedFormats) + ' 个格式）' + NewLine + Space;
  ItemsOnLine := 0;
  for I := 0 to 24 do if AssocList.Checked[Rows[I]] then begin
    if ItemsOnLine = 8 then begin
      Result := Result + NewLine + Space;
      ItemsOnLine := 0;
    end;
    if ItemsOnLine > 0 then Result := Result + '  ';
    Result := Result + Extensions[I];
    ItemsOnLine := ItemsOnLine + 1;
  end;
  Result := Result + NewLine + Space + '安装后可在 Windows 中确认默认应用。';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = ResultsPage.ID) and (SelectedFormats = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then RetirePriorFiles;
  if CurStep = ssPostInstall then begin
    RetiredCommitted := True;
    ApplyAssociations(False);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveOwned(''); SHChangeNotify($08000000, 0, 0, 0);
  end;
  if CurUninstallStep = usPostUninstall then ApplyUninstallData;
end;

procedure InitializeUninstallProgressForm;
var RetainedData: TNewStaticText;
begin
  RetainedData := TNewStaticText.Create(UninstallProgressForm);
  RetainedData.Parent := UninstallProgressForm.InstallingPage;
  RetainedData.AutoSize := False; RetainedData.WordWrap := True;
  RetainedData.SetBounds(UninstallProgressForm.ProgressBar.Left,
    UninstallProgressForm.ProgressBar.Top + UninstallProgressForm.ProgressBar.Height + ScaleY(14),
    UninstallProgressForm.ProgressBar.Width, ScaleY(35));
  if DeleteSettingsRequested or DeleteHistoryRequested then
    RetainedData.Caption := '按所选选项处理当前账户的数据。原始素材和导出文件会保留。'
  else RetainedData.Caption := '用户设置、任务历史、原始素材和导出文件会保留。';
  UninstallProgressForm.CancelButton.Visible := False;
  UninstallProgressForm.Bevel.Visible := False;
  UninstallProgressForm.ClientHeight := UninstallProgressForm.InnerNotebook.Top +
    RetainedData.Top + RetainedData.Height + ScaleY(16);
  UninstallProgressForm.OuterNotebook.Height := UninstallProgressForm.ClientHeight;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectDir then begin
    WizardForm.DiskSpaceLabel.Visible := False;
    RefreshSpace(nil);
  end;
  if CurPageID = ResultsPage.ID then RefreshDefaults(nil);
  if CurPageID = wpFinished then RefreshDefaults(nil);
  if (CurPageID = wpReady) and (PreviousInstallDir <> '') then
    WizardForm.NextButton.Caption := UpgradeActionLabel;
end;
