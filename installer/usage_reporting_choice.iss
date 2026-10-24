{ 匿名使用统计的安装选项。
  Server 只在 config.toml 的 [general] usage_reporting 显式为 true 时发送（server/src/telemetry/usage_reporting.h）。
  出厂模板写的是 "ask"，表示还没选；从没选过的旧配置干脆没有这个键。两种情况安装程序都要问一次，首装和升级一样。
  已经记下 true 或 false 的升级跳过询问页，原样保留。

  这一页满足 SignPath 对收集数据的软件的要求：安装时展示隐私说明，并提供关闭选项。
  静默安装看不到向导页，所以不替用户做决定：只有命令行给了 /USAGEREPORTING=1 或 0 才写入，否则什么都不记，Server 也就不发送。
  命令行开关优先于已有的选择，供批量部署统一设定。

  调用方需要提供 GetDataDir、ResolvePreviousDataDir 和 UserConfigPath。}

const
  UsageReportingPrivacyUrl = 'https://github.com/metasequoiaime/MSIME-Windows/blob/main/PRIVACY.md';
  UsageReportingKey = 'usage_reporting';

var
  UsageReportingPage: TInputOptionWizardPage;
  UsageReportingDecidedBeforeInstall: Boolean;

{ 配置值里的 true / false，其他写法（包括 "ask"）都算还没选，返回空串。}
function ParseUsageReportingValue(const Value: String): String;
var
  Text: String;
  Hash: Integer;
begin
  Text := Value;
  Hash := Pos('#', Text);
  if Hash > 0 then
    Text := Copy(Text, 1, Hash - 1);
  Text := Trim(Text);
  if Text = 'true' then
    Result := 'true'
  else if Text = 'false' then
    Result := 'false'
  else
    Result := '';
end;

{ 这一行是 [general] 里的 usage_reporting 赋值时返回 True。}
function IsUsageReportingLine(const Line: String; var Value: String): Boolean;
var
  Trimmed: String;
  Equals: Integer;
begin
  Result := False;
  Trimmed := Trim(Line);
  Equals := Pos('=', Trimmed);
  if (Equals > 0) and (Trim(Copy(Trimmed, 1, Equals - 1)) = UsageReportingKey) then
  begin
    Value := Copy(Trimmed, Equals + 1, Length(Trimmed));
    Result := True;
  end;
end;

{ 某份 config.toml 里记下的选择：'true'、'false'，没有文件、没有这个键或还没选时为空串。}
function UsageReportingDecisionIn(const ConfigPath: String): String;
var
  Lines: TArrayOfString;
  Index: Integer;
  Trimmed: String;
  Value: String;
  InGeneral: Boolean;
begin
  Result := '';
  if (ConfigPath = '') or (not FileExists(ConfigPath)) or (not LoadStringsFromFile(ConfigPath, Lines)) then
    Exit;
  InGeneral := False;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Trimmed := Trim(Lines[Index]);
    if (Length(Trimmed) > 0) and (Trimmed[1] = '[') then
      InGeneral := (Trimmed = '[general]')
    else if InGeneral and IsUsageReportingLine(Trimmed, Value) then
    begin
      Result := ParseUsageReportingValue(Value);
      Exit;
    end;
  end;
end;

{ 安装前已经记下的选择。换目录升级时配置还在原目录，本次目录没有配置就去那里看。}
function ExistingUsageReportingDecision: String;
begin
  Result := UsageReportingDecisionIn(UserConfigPath);
  if (Result = '') and (not FileExists(UserConfigPath)) and (ResolvePreviousDataDir <> '') and
     (CompareText(GetDataDir(''), ResolvePreviousDataDir) <> 0) then
    Result := UsageReportingDecisionIn(AddBackslash(ResolvePreviousDataDir) + 'config.toml');
end;

{ /USAGEREPORTING=1 或 0（也接受 true / false）。没给或写错时返回空串，写错的值记进安装日志。}
function UsageReportingSwitch: String;
var
  Value: String;
begin
  Value := Lowercase(Trim(ExpandConstant('{param:USAGEREPORTING|}')));
  if (Value = '1') or (Value = 'true') then
    Result := 'true'
  else if (Value = '0') or (Value = 'false') then
    Result := 'false'
  else
  begin
    Result := '';
    if Value <> '' then
      Log('Ignoring /USAGEREPORTING=' + Value + ': expected 0 or 1, recording no decision');
  end;
end;

function ShouldSkipUsageReportingPage: Boolean;
begin
  Result := (UsageReportingSwitch <> '') or (ExistingUsageReportingDecision <> '');
end;

procedure UsageReportingPrivacyClick(Sender: TObject);
var
  ErrorCode: Integer;
begin
  ShellExecAsOriginalUser('open', UsageReportingPrivacyUrl, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

procedure CreateUsageReportingPage(AfterID: Integer);
var
  Link: TNewStaticText;
begin
  UsageReportingPage := CreateInputOptionPage(
    AfterID,
    '匿名使用统计',
    '是否发送匿名使用统计',
    '勾选后，输入法每天向本项目自己的服务器 api.msime.app 发送一条活跃记录，用来统计有多少安装在使用。' +
    '记录里只有随机事件 id、本机随机生成的安装 id、平台名和版本号，不含任何输入内容、候选词、账号或设备信息。' + #13#10 +
    '不勾选就不发送。安装后随时可以在「设置 → 关于 → 匿名使用统计」里改变这个选择。',
    False,
    False
  );
  UsageReportingPage.Add('发送匿名使用统计（每天一条活跃记录）');
  UsageReportingPage.Values[0] := True;

  Link := TNewStaticText.Create(UsageReportingPage);
  Link.Parent := UsageReportingPage.Surface;
  Link.Caption := '查看隐私说明（PRIVACY.md）';
  Link.Cursor := crHand;
  Link.Font.Color := clBlue;
  Link.Font.Style := [fsUnderline];
  Link.OnClick := @UsageReportingPrivacyClick;
  Link.Left := UsageReportingPage.CheckListBox.Left;
  Link.Top := UsageReportingPage.SurfaceHeight - Link.Height;
  UsageReportingPage.CheckListBox.Height := Link.Top - UsageReportingPage.CheckListBox.Top - ScaleY(8);
end;

{ 把选择写进 [general]：替换已有的 usage_reporting 行；没有这一行就插在 [general] 后面；连 [general] 都没有就追加一节。}
function WriteUsageReportingDecision(const Decision: String): String;
var
  Lines: TArrayOfString;
  Output: TArrayOfString;
  Index: Integer;
  Count: Integer;
  GeneralAt: Integer;
  Trimmed: String;
  Value: String;
  InGeneral: Boolean;
  Replaced: Boolean;
  Line: String;
begin
  Result := '';
  Line := UsageReportingKey + ' = ' + Decision;
  if not FileExists(UserConfigPath) then
  begin
    Result := '找不到配置文件';
    Exit;
  end;
  if not LoadStringsFromFile(UserConfigPath, Lines) then
  begin
    Result := '无法读取';
    Exit;
  end;

  GeneralAt := -1;
  InGeneral := False;
  Replaced := False;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Trimmed := Trim(Lines[Index]);
    if (Length(Trimmed) > 0) and (Trimmed[1] = '[') then
    begin
      InGeneral := (Trimmed = '[general]');
      if InGeneral and (GeneralAt < 0) then
        GeneralAt := Index;
    end
    else if InGeneral and (not Replaced) and IsUsageReportingLine(Trimmed, Value) then
    begin
      Lines[Index] := Line;
      Replaced := True;
    end;
  end;

  if Replaced then
    Output := Lines
  else
  begin
    Count := GetArrayLength(Lines);
    if GeneralAt >= 0 then
    begin
      SetArrayLength(Output, Count + 1);
      for Index := 0 to GeneralAt do
        Output[Index] := Lines[Index];
      Output[GeneralAt + 1] := Line;
      for Index := GeneralAt + 1 to Count - 1 do
        Output[Index + 1] := Lines[Index];
    end
    else
    begin
      SetArrayLength(Output, Count + 3);
      for Index := 0 to Count - 1 do
        Output[Index] := Lines[Index];
      Output[Count] := '';
      Output[Count + 1] := '[general]';
      Output[Count + 2] := Line;
    end;
  end;

  if not SaveStringsToUTF8FileWithoutBOM(UserConfigPath, Output, False) then
    Result := '无法保存';
end;

{ 安装前调用：迁移和 [Files] 都还没动配置时记下是否已有选择。}
procedure RememberUsageReportingDecision;
begin
  UsageReportingDecidedBeforeInstall := ExistingUsageReportingDecision <> '';
end;

{ 安装后调用。Interactive 为 False 时（静默安装）向导页没有显示过，不能拿它的默认值当作用户的选择。
  写入失败不中止安装：没写进去就是还没选，Server 不会发送。}
procedure ApplyUsageReportingChoice(Interactive: Boolean);
var
  Decision: String;
  Problem: String;
begin
  Decision := UsageReportingSwitch;
  if Decision = '' then
  begin
    if (not Interactive) or UsageReportingDecidedBeforeInstall or (UsageReportingPage = nil) then
      Exit;
    if UsageReportingPage.Values[0] then
      Decision := 'true'
    else
      Decision := 'false';
  end;

  Problem := WriteUsageReportingDecision(Decision);
  if Problem = '' then
  begin
    Log('Recorded usage_reporting = ' + Decision + ' in ' + UserConfigPath);
    Exit;
  end;
  Log('Cannot record usage_reporting = ' + Decision + ' in ' + UserConfigPath + ': ' + Problem);
  SuppressibleMsgBox(
    '无法把匿名使用统计的选择写入 ' + UserConfigPath + '（' + Problem + '）。' + #13#10 +
    '在你做出选择之前不会发送任何统计，可以在安装完成后到「设置 → 关于 → 匿名使用统计」里设置。',
    mbError, MB_OK, IDOK);
end;
