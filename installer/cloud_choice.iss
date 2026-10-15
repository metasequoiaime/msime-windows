{ 升级换目录时，旧配置会迁移到新目录。任一目录已有配置，就跳过安装选项。}
function UserConfigExistsBeforeInstall: Boolean;
begin
  Result := FileExists(UserConfigPath);
  if (not Result) and
     (CompareText(GetDataDir(''), ResolvePreviousDataDir) <> 0) then
    Result := FileExists(AddBackslash(ResolvePreviousDataDir) + 'config.toml');
end;

{ 只改本次安装新生成的 config.toml。用户取消云候选时，写入失败必须报错。}
procedure ApplyNetworkChoiceToUserConfig;
var
  Lines: TArrayOfString;
  Index: Integer;
  Trimmed: String;
  InGeneral: Boolean;
begin
  if UserConfigExistedBeforeInstall or (NetworkPage = nil) then
    Exit;
  if NetworkPage.Values[CloudCandidatesIndex] then
    Exit;
  if not LoadStringsFromFile(UserConfigPath, Lines) then
    RaiseException('无法读取新安装的用户配置：' + UserConfigPath);

  InGeneral := False;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Trimmed := Trim(Lines[Index]);
    if (Length(Trimmed) > 0) and (Trimmed[1] = '[') then
      InGeneral := (Trimmed = '[general]')
    else if InGeneral and (Pos('cloud_candidates', Trimmed) = 1) then
    begin
      Lines[Index] := 'cloud_candidates = false';
      if not SaveStringsToUTF8FileWithoutBOM(UserConfigPath, Lines, False) then
        RaiseException('无法保存云候选选择到 ' + UserConfigPath);
      Exit;
    end;
  end;
  RaiseException('新安装的用户配置缺少 cloud_candidates：' + UserConfigPath);
end;
