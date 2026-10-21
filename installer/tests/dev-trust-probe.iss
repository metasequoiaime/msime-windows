[Setup]
AppName=MSIME development certificate probe
AppVersion=1
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
OutputBaseFilename=dev-trust-probe

[Files]
Source: "{#DevCertificate}"; DestName: "dev-build.cer"; Flags: dontcopy
Source: "{#ProbeScript}"; DestName: "import-dev-trust.ps1"; Flags: dontcopy

[Code]
#include "..\dev_trust.iss"

function InitializeSetup: Boolean;
var
  Problem: String;
begin
  Problem := PrepareDevCertificate;
  Log('Probe result: ' + Problem);
  Result := False;
end;
