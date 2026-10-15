{ 升级换目录时，旧配置会迁移到新目录。任一目录已有配置，就跳过安装选项。}
function UserConfigExistsBeforeInstall: Boolean;
begin
  Result := FileExists(UserConfigPath);
  if (not Result) and
     (CompareText(GetDataDir(''), ResolvePreviousDataDir) <> 0) then
    Result := FileExists(AddBackslash(ResolvePreviousDataDir) + 'config.toml');
end;

function SameFileContent(const A, B: String): Boolean;
begin
  try
    Result := CompareText(GetSHA256OfFile(A), GetSHA256OfFile(B)) = 0;
  except
    Result := False;
  end;
end;

{ 迁移前先检查冲突。迁移时再次检查，防止目录内容在停机期间改变。
  两份内容相同说明上次迁移已经复制过去、只是没删掉源文件，重试时不算冲突。}
function ConfigMigrationProblem(const OldDir, NewDir: String): String;
var
  OldConfig, NewConfig: String;
begin
  Result := '';
  if (OldDir = '') or (CompareText(OldDir, NewDir) = 0) or
     (not DirExists(OldDir)) then
    Exit;
  OldConfig := AddBackslash(OldDir) + 'config.toml';
  NewConfig := AddBackslash(NewDir) + 'config.toml';
  if FileExists(OldConfig) and FileExists(NewConfig) and
     (not SameFileContent(OldConfig, NewConfig)) then
    Result := '目标数据目录已有 config.toml，不能覆盖原有配置。请选择其他数据目录。';
end;

{ 冲突检查通过后两份还同时存在，只能是上次迁移留下的相同副本，删掉源文件再搬。}
procedure DropMigratedConfigCopy(const OldDir, NewDir: String);
begin
  if FileExists(AddBackslash(OldDir) + 'config.toml') and
     FileExists(AddBackslash(NewDir) + 'config.toml') then
    DeleteFile(AddBackslash(OldDir) + 'config.toml');
end;

{ 网络页因为旧配置存在而跳过。旧配置没搬过去，新目录就会装上出厂配置，
  用户关掉的云候选会被悄悄打开，所以必须中止安装。}
function ConfigMigrationFailure(const OldDir, NewDir: String): String;
begin
  Result := '';
  if FileExists(AddBackslash(OldDir) + 'config.toml') and
     (not FileExists(AddBackslash(NewDir) + 'config.toml')) then
    Result :=
      '无法把用户配置从 ' + OldDir + ' 移动到 ' + NewDir + '。' + #13#10 +
      '请确认输入法相关进程已全部退出、目标磁盘可写，然后重试安装。' + #13#10 +
      '你的配置仍留在 ' + OldDir + '，本次安装没有改动它。';
end;

{ 只改本次安装新生成的 config.toml。写入失败不中止安装：此时文件已经落盘，
  中止只会留下一个缺登录任务和目录权限的半成品。改为提示用户到设置页手动关闭。}
procedure ApplyNetworkChoiceToUserConfig;
var
  Lines: TArrayOfString;
  Index: Integer;
  Trimmed: String;
  InGeneral: Boolean;
  Problem: String;
begin
  if UserConfigExistedBeforeInstall or (NetworkPage = nil) then
    Exit;
  if NetworkPage.Values[CloudCandidatesIndex] then
    Exit;

  Problem := '缺少 cloud_candidates';
  if not LoadStringsFromFile(UserConfigPath, Lines) then
    Problem := '无法读取'
  else
  begin
    InGeneral := False;
    for Index := 0 to GetArrayLength(Lines) - 1 do
    begin
      Trimmed := Trim(Lines[Index]);
      if (Length(Trimmed) > 0) and (Trimmed[1] = '[') then
        InGeneral := (Trimmed = '[general]')
      else if InGeneral and (Pos('cloud_candidates', Trimmed) = 1) then
      begin
        Lines[Index] := 'cloud_candidates = false';
        if SaveStringsToUTF8FileWithoutBOM(UserConfigPath, Lines, False) then
          Exit;
        Problem := '无法保存';
        Break;
      end;
    end;
  end;

  Log('Cannot apply cloud candidates choice to ' + UserConfigPath + ': ' + Problem);
  SuppressibleMsgBox(
    '无法把「不启用云候选」写入 ' + UserConfigPath + '（' + Problem + '）。' + #13#10 +
    '云候选目前仍是开启状态，请在安装完成后到「设置 → 输入」里关闭它。',
    mbError, MB_OK, IDOK);
end;
