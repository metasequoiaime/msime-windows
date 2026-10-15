[Setup]
AppId=MsimeCloudChoiceProbe
AppName=MsimeCloudChoiceProbe
AppVersion=1
DefaultDirName={localappdata}\MsimeCloudChoiceProbe
PrivilegesRequired=lowest
CreateUninstallRegKey=no
Uninstallable=no
OutputBaseFilename=cloud-choice-probe
Compression=none
DisableProgramGroupPage=yes

[Files]
Source: "..\default_config\config.default.toml"; DestDir: "{param:TARGET}"; DestName: "config.toml"; Flags: onlyifdoesntexist

[Code]
var
  NetworkPage: TInputOptionWizardPage;
  CloudCandidatesIndex: Integer;
  UserConfigExistedBeforeInstall: Boolean;

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

#include "../cloud_choice.iss"

procedure InitializeWizard;
begin
  NetworkPage := CreateInputOptionPage(wpWelcome, 'Network', 'Network', 'Network', False, False);
  CloudCandidatesIndex := NetworkPage.Add('Cloud candidates');
  NetworkPage.Values[CloudCandidatesIndex] := ExpandConstant('{param:CLOUD|1}') <> '0';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  OldConfig: String;
begin
  Result := ConfigMigrationProblem(ResolvePreviousDataDir, GetDataDir(''));
  if Result <> '' then
    Exit;
  UserConfigExistedBeforeInstall := UserConfigExistsBeforeInstall;
  OldConfig := AddBackslash(ResolvePreviousDataDir) + 'config.toml';
  if FileExists(OldConfig) and (not FileExists(UserConfigPath)) then
  begin
    ForceDirectories(GetDataDir(''));
    if not CopyFile(OldConfig, UserConfigPath, True) then
      Result := 'Cannot copy previous config';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ApplyNetworkChoiceToUserConfig;
end;
