[Setup]
AppId=MsimeUsageReportingProbe
AppName=MsimeUsageReportingProbe
AppVersion=1
DefaultDirName={localappdata}\MsimeUsageReportingProbe
PrivilegesRequired=lowest
CreateUninstallRegKey=no
Uninstallable=no
OutputBaseFilename=usage-reporting-probe
Compression=none
DisableProgramGroupPage=yes

[Files]
Source: "..\default_config\config.default.toml"; DestDir: "{param:TARGET}"; DestName: "config.toml"; Flags: onlyifdoesntexist

[Code]
function GetDataDir(Param: String): String;
begin
  Result := ExpandConstant('{param:TARGET|}');
end;

function ResolvePreviousDataDir: String;
begin
  Result := ExpandConstant('{param:PREVIOUS|}');
end;

function UserConfigPath: String;
begin
  Result := AddBackslash(GetDataDir('')) + 'config.toml';
end;

#include "../usage_reporting_choice.iss"

{ 探针总以 /VERYSILENT 运行，向导页不会显示。/INTERACTIVE=1 模拟用户在询问页上点了「下一步」，/PAGE= 是那时勾选框的状态。}
procedure InitializeWizard;
begin
  CreateUsageReportingPage(wpWelcome);
  UsageReportingPage.Values[0] := ExpandConstant('{param:PAGE|1}') <> '0';
end;

{ 与 msime_setup.iss 同序：先在迁移之前记下已有的选择，再用同样的 robocopy /MOVE 搬 config.toml。}
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  OldDir, NewDir: String;
  ResultCode: Integer;
begin
  Result := '';
  OldDir := ResolvePreviousDataDir;
  NewDir := GetDataDir('');
  if ShouldSkipUsageReportingPage then
    Log('Usage reporting page: skipped')
  else
    Log('Usage reporting page: shown');
  RememberUsageReportingDecision;
  if (OldDir = '') or (CompareText(OldDir, NewDir) = 0) or (not DirExists(OldDir)) then
    Exit;
  ForceDirectories(NewDir);
  if not (Exec(
      ExpandConstant('{sys}\robocopy.exe'),
      '"' + OldDir + '" "' + NewDir + '" config.toml /MOVE /R:0 /W:0 /NJH /NJS /NP /NFL /NDL',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode < 8)) then
    Result := 'Cannot move config.toml';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ApplyUsageReportingChoice(ExpandConstant('{param:INTERACTIVE|0}') = '1');
end;
