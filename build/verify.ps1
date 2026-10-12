<#
.SYNOPSIS
  Reproducible software verification. Does not install drivers, change boot settings,
  or select an audio endpoint. -Sign creates/reuses a development signing certificate.
.EXAMPLE
  .\build\verify.ps1 -Sign -Sanitize -Interop
#>
param(
    [ValidateSet('Release','Debug')][string]$Config = 'Release',
    [switch]$SkipBuild,
    [switch]$Sign,
    [switch]$Sanitize,
    [switch]$Interop
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputDir = Join-Path $projectRoot "out\$Config"
$reportDir = Join-Path $projectRoot ('out\verification\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
. (Join-Path $PSScriptRoot 'toolchain.ps1')
$results = [Collections.Generic.List[object]]::new()

function Invoke-Check([string]$name, [scriptblock]$action) {
    $log = Join-Path $reportDir "$name.log"
    Write-Host "$name ..."
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $global:LASTEXITCODE = 0
    try {
        & $action *> $log
        if ($LASTEXITCODE -ne 0) { throw "exit code $LASTEXITCODE" }
        $results.Add([pscustomobject]@{ Check = $name; Passed = $true; Seconds = $timer.Elapsed.TotalSeconds; Log = $log })
    } catch {
        Get-Content -LiteralPath $log -Tail 15
        $results.Add([pscustomobject]@{ Check = $name; Passed = $false; Error = $_.Exception.Message; Log = $log })
        throw "$name failed: $($_.Exception.Message)"
    }
}

try {
    if (-not $SkipBuild) {
        Invoke-Check 'build-package' { & (Join-Path $PSScriptRoot 'build.ps1') -Target package -Config $Config -Sign:$Sign }
    }
    Invoke-Check 'inf-verification' { & $INFVERIF /w /v (Join-Path $outputDir 'package\pna2dp.inf') }
    Invoke-Check 'core' { & (Join-Path $outputDir 'core_test.exe') }
    Invoke-Check 'boot-mode' { & (Join-Path $outputDir 'boot_mode_test.exe') }
    $vectors = Join-Path $reportDir 'vectors'
    New-Item -ItemType Directory -Path $vectors | Out-Null
    Invoke-Check 'sbc' { & (Join-Path $outputDir 'sbc_test.exe') $vectors }
    $pluginVectors = Join-Path $reportDir 'plugin-vectors'
    New-Item -ItemType Directory -Path $pluginVectors | Out-Null
    Invoke-Check 'plugins' { & (Join-Path $outputDir 'plugin_test.exe') $outputDir $pluginVectors }
    Invoke-Check 'controls-and-machine' { & (Join-Path $outputDir 'pnctl_test.exe') }
    Invoke-Check 'audio-endpoints' { & (Join-Path $outputDir 'audio_test.exe') --list }
    Invoke-Check 'windows-audio' { & (Join-Path $outputDir 'windows_audio_test.exe') }
    Invoke-Check 'spatial-discovery' { & (Join-Path $outputDir 'spatial_test.exe') }
    if ($Interop) {
        if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw '-Interop needs ffmpeg in PATH' }
        Invoke-Check 'sbc-independent-decoder' { & python (Join-Path $PSScriptRoot 'test-interop.py') $vectors }
        Invoke-Check 'plugin-independent-decoder' { & python (Join-Path $PSScriptRoot 'test-plugin-interop.py') $pluginVectors }
    }
    if ($Sanitize) {
        Invoke-Check 'build-address-sanitizer' { & (Join-Path $PSScriptRoot 'build.ps1') -Target tests -Config $Config -Sanitize }
        $savedPath = $env:PATH
        try {
            $env:PATH = (Split-Path $CL) + ';' + $env:PATH
            $asanDir = Join-Path $projectRoot "out\$Config-Asan"
            Invoke-Check 'asan-core' { & (Join-Path $asanDir 'core_test.exe') }
            Invoke-Check 'asan-sbc' { & (Join-Path $asanDir 'sbc_test.exe') }
            Invoke-Check 'asan-controls' { & (Join-Path $asanDir 'pnctl_test.exe') }
        } finally { $env:PATH = $savedPath }
    }
    Invoke-Check 'installer-payload' {
        $extractDir = Join-Path $reportDir 'extracted'
        $setup = Join-Path $outputDir 'pna2dp-setup.exe'
        $process = Start-Process -FilePath $setup -ArgumentList @('--extract', ('"' + $extractDir + '"')) -PassThru -WindowStyle Hidden
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Installer extraction timed out' }
        if ($process.ExitCode -ne 0) { throw "Installer extraction failed: $($process.ExitCode)" }
        $portable = Join-Path $outputDir 'dist\portable'
        $files = @(Get-ChildItem -LiteralPath $extractDir -Recurse -File)
        if ($files.Count -lt 10) { throw 'Incomplete installer payload' }
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($extractDir.Length + 1)
            $expected = Join-Path $portable $relative
            if (-not (Test-Path -LiteralPath $expected) -or (Get-FileHash -LiteralPath $expected).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
                throw "Installer payload mismatch: $relative"
            }
        }
        Write-Output "$($files.Count) extracted files match the portable distribution byte for byte."
    }
    Invoke-Check 'gui-startup' {
        $process = Start-Process -FilePath (Join-Path $outputDir 'pna2dp-panel.exe') -ArgumentList '--demo' -PassThru -WindowStyle Hidden
        try {
            if (-not $process.WaitForInputIdle(10000) -or $process.HasExited) { throw 'Control panel failed to start' }
            Write-Output 'Control panel demo reached its message loop.'
        } finally {
            if (-not $process.HasExited) {
                [void]$process.CloseMainWindow()
                if (-not $process.WaitForExit(5000)) { $process.Kill() }
            }
        }
    }
    Get-ChildItem -LiteralPath (Join-Path $outputDir 'dist') -File |
        Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reportDir 'artifact-hashes.json') -Encoding UTF8
    Write-Host "All $($results.Count) software checks passed. Hardware playback has not been tested by this script."
} finally {
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $reportDir 'results.json') -Encoding UTF8
    Write-Host "Report: $reportDir"
}
