# Run the actual helpers from a moved checkout. Native commands are synthetic;
# no Server process, installed user data, microphone, signing or registration.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime test helpers ' + [Guid]::NewGuid())
$originalLocation = Get-Location
try {
    $scripts = Join-Path $fixture 'server/tests/scripts'
    New-Item -ItemType Directory -Force $scripts | Out-Null
    Copy-Item (Join-Path $source 'server/tests/scripts/*.ps1') $scripts
    function global:cmake {
        $global:MsimeTestCalls.Add(@{ tool = 'cmake'; arguments = @($args); location = (Get-Location).Path })
        $global:LASTEXITCODE = if ($global:MsimeTestCalls.Count -eq $global:MsimeTestFailAt) { 17 } else { 0 }
    }
    function global:ctest {
        $global:MsimeTestCalls.Add(@{ tool = 'ctest'; arguments = @($args); location = (Get-Location).Path })
        $global:LASTEXITCODE = if ($global:MsimeTestCalls.Count -eq $global:MsimeTestFailAt) { 17 } else { 0 }
    }
    Set-Location $fixture
    foreach ($configuration in @('Debug', 'Release')) {
        $build = Join-Path $fixture $(if ($configuration -eq 'Release') { 'server/build-release' } else { 'server/build' })
        $preset = if ($configuration -eq 'Release') { 'default-release' } else { 'default' }
        foreach ($failure in @(0, 1, 2, 3)) {
            $global:MsimeTestCalls = [Collections.Generic.List[object]]::new()
            $global:MsimeTestFailAt = $failure
            $rejected = $false
            try { & (Join-Path $scripts 'llaunch.ps1') -Configuration $configuration }
            catch { $rejected = $_.Exception.Message -match 'Server test(s| configure| build) failed \(17\)' }
            if ($rejected -ne ($failure -ne 0)) { throw "Wrong failure result at stage $failure ($configuration)" }
            $expectedCount = if ($failure -eq 0) { 3 } else { $failure }
            if ($global:MsimeTestCalls.Count -ne $expectedCount) { throw 'Continued after failure or skipped a stage' }
            if ((Get-Location).Path -ne $fixture) { throw 'Changed the caller working directory' }
            if ($failure -ne 0) { continue }
            $configure, $compile, $test = $global:MsimeTestCalls
            if ($configure.location -ne (Join-Path $fixture 'server') -or
                ($configure.arguments -join '|') -ne "--preset=$preset|-DBUILD_TESTING=ON") {
                throw 'Configured the obsolete standalone tests project'
            }
            if (($compile.arguments -join '|') -ne "--build|$build|--config|$configuration|--target|MetasequoiaImeServerTests|MetasequoiaImeSettingsUiTests|test_webview_contract") {
                throw 'Did not build every registered test in the parent Server tree'
            }
            if ($test.tool -ne 'ctest' -or ($test.arguments -join '|') -ne "--test-dir|$build|-C|$configuration|--output-on-failure|--no-tests=error|--timeout|120") {
                throw 'Did not run the matching CTest registry with empty-test rejection'
            }
        }
    }
    $global:LASTEXITCODE = 0
    Write-Host 'Debug/Release test helpers resolve the Server project and propagate every failure'
} finally {
    Set-Location $originalLocation
    Remove-Item Function:/cmake, Function:/ctest -ErrorAction SilentlyContinue
    Remove-Variable MsimeTestCalls, MsimeTestFailAt -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path $fixture) { Remove-Item $fixture -Recurse -Force }
}
