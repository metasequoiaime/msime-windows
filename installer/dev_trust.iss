// Included only in development builds and the isolated installer probe.
function PrepareDevCertificate: String;
var
  CertificatePath, ScriptPath: String;
  ExitCode: Integer;
begin
  Result := '';
  try
    ExtractTemporaryFile('dev-build.cer');
    ExtractTemporaryFile('import-dev-trust.ps1');
    CertificatePath := ExpandConstant('{tmp}\dev-build.cer');
    ScriptPath := ExpandConstant('{tmp}\import-dev-trust.ps1');
    if CompareText(GetSHA256OfFile(CertificatePath), '{#DevCertificateSha256}') <> 0 then
    begin
      Result := 'Development signing certificate SHA256 mismatch.';
      exit;
    end;
    { Keep older builds trusted if a later install stage fails or an old DLL is still loaded. }
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ScriptPath +
      '" -CertificatePath "' + CertificatePath + '" -Keep',
      '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
      Result := 'Cannot start development certificate import: ' + IntToStr(ExitCode)
    else if ExitCode <> 0 then
      Result := 'Development certificate import failed: ' + IntToStr(ExitCode);
  except
    Result := GetExceptionMessage;
  end;
  if Result <> '' then
    Log(Result)
  else
    Log('Development signing certificate is trusted.');
end;
