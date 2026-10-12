<# Read-only application/package verification; never installs or changes drivers. #>
param([ValidateSet('Release','Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root "out\$Config"
$setup = Join-Path $bin 'dist-windows\pna2dp-setup.exe'
$report = Join-Path $root ('out\verification\windows-mode-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $report | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check([string]$name, [scriptblock]$body) {
    try { & $body; $results.Add([pscustomobject]@{name=$name; passed=$true}); Write-Host "PASS $name" }
    catch { $results.Add([pscustomobject]@{name=$name; passed=$false; error=$_.Exception.Message}); Write-Host "FAIL $name : $_" }
}
function SetupRun([string[]]$arguments) {
    $process = Start-Process -FilePath $setup -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Package check timed out.' }
    if ($process.ExitCode -ne 0) { throw "Setup returned $($process.ExitCode)" }
}
Check 'application-only preflight' {
    $path = Join-Path $report 'preflight.txt'
    SetupRun @('--check','--report',('"' + $path + '"'))
    if ((Get-Content -LiteralPath $path -Raw) -notmatch 'Windows mode package: no PN-A2DP kernel driver') { throw 'Wrong package type.' }
}
$extract = Join-Path $report 'payload'
Check 'payload excludes kernel drivers and codec host' {
    SetupRun @('--extract',('"' + $extract + '"'))
    $allowed = @('pna2dp-panel.exe','pna2dpctl.exe','LICENSE','THIRD-PARTY-NOTICES.md','README.md','13-windows-mode.md')
    $files = @(Get-ChildItem -LiteralPath $extract -Recurse -File)
    if ($files.Count -ne $allowed.Count) { throw 'Unexpected payload file count.' }
    foreach ($file in $files) { if ($file.Name -notin $allowed) { throw "Unexpected file $($file.Name)" } }
    foreach ($name in @('pna2dp-panel.exe','pna2dpctl.exe')) {
        if ((Get-FileHash -LiteralPath (Join-Path $extract $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $bin $name)).Hash) { throw "Stale payload: $name" }
    }
}
Check 'portable package excludes kernel drivers and codec host' {
    $portable = Join-Path $bin 'dist-windows\portable'
    $forbidden = @(Get-ChildItem -LiteralPath $portable -Recurse -File | Where-Object { $_.Extension -in @('.sys','.inf','.cat','.cer','.dll') -or $_.Name -eq 'pna2dp-codechost.exe' })
    if ($forbidden.Count) { throw 'Driver, certificate, codec host, or plugin found.' }
}
Check 'Windows endpoint identity and invalid-input regression' {
    & (Join-Path $bin 'windows_audio_test.exe') *> (Join-Path $report 'audio.txt')
    if ($LASTEXITCODE) { throw 'Windows audio tests failed.' }
}
Check 'Windows CLI lists paired devices without PN driver dependency' {
    & (Join-Path $bin 'pna2dpctl.exe') windows list *> (Join-Path $report 'devices.txt')
    if ($LASTEXITCODE) { throw 'Enumeration failed.' }
    & (Join-Path $bin 'pna2dpctl.exe') windows status -d invalid *> (Join-Path $report 'invalid-index.txt')
    if ($LASTEXITCODE -ne 1) { throw 'Invalid device index accepted.' }
}
Check 'spatial resolver regression' {
    & (Join-Path $bin 'spatial_test.exe') *> (Join-Path $report 'spatial.txt')
    if ($LASTEXITCODE) { throw 'Spatial tests failed.' }
}
$results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $report 'results.json') -Encoding UTF8
Write-Host "Report: $report"
if (@($results | Where-Object { !$_.passed }).Count) { throw 'Windows mode verification failed.' }
