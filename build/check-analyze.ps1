<#
.SYNOPSIS
  Code-analysis ratchet for the kernel driver. Builds the driver with
  -Config Debug -Analyze, counts the unique compiler and /analyze warnings and fails
  only when the count rises above build\analyze-baseline.txt. Installs nothing.
.EXAMPLE
  .\build\check-analyze.ps1                  # build and compare
  .\build\check-analyze.ps1 -Log build.log   # compare an existing -Analyze build log
  .\build\check-analyze.ps1 -UpdateBaseline  # build and record the current warnings
.NOTES
  Line numbers move as sources change, so only the count is compared. The baseline lists
  each warning by file and code to show where a new one appeared. Lower the baseline
  (-UpdateBaseline) after fixing warnings so they cannot come back.
#>
param(
    [string]$Log = '',
    [string]$Baseline = (Join-Path $PSScriptRoot 'analyze-baseline.txt'),
    [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if (-not $Log) {
    $outDir = Join-Path $projectRoot 'out\Debug'
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $Log = Join-Path $outDir 'analyze-driver.log'
    $errorLog = Join-Path $outDir 'analyze-driver.stderr.log'
    # A child process keeps cl's stderr out of this script's error stream.
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + (Join-Path $PSScriptRoot 'build.ps1') + '"'),
                   '-Target', 'driver', '-Config', 'Debug', '-Analyze')
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $Log -RedirectStandardError $errorLog
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $Log, $errorLog -Tail 20
        throw "Debug -Analyze driver build failed with exit code $($process.ExitCode) (log: $Log)"
    }
}

# file(line[,col]): warning Cnnnn: message  (lazy file match, so that paths such as
# 'Program Files (x86)' still parse)
$pattern = '^\s*(?<file>.+?)\((?<line>\d+)(,\d+)?\)\s*:\s*warning\s+(?<code>C\d+)\s*:\s*(?<msg>.*)$'
$rootPrefix = $projectRoot.TrimEnd('\') + '\'
$unique = @{}
foreach ($text in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)) {
    $m = [regex]::Match($text, $pattern)
    if (-not $m.Success) { continue }
    $file = $m.Groups['file'].Value.Trim()
    if ($file.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { $file = $file.Substring($rootPrefix.Length) }
    else {
        # WDK / SDK headers: keep the part below Include\ so the kit location does not matter
        $at = $file.IndexOf('\Include\', [StringComparison]::OrdinalIgnoreCase)
        if ($at -ge 0) { $file = 'kit:' + $file.Substring($at + 9) }
    }
    $file = $file.ToLowerInvariant()
    $msg = $m.Groups['msg'].Value.Replace($projectRoot, '').Trim()
    $key = "$file($($m.Groups['line'].Value)): $($m.Groups['code'].Value): $msg"
    $unique[$key] = "$file $($m.Groups['code'].Value)"
}
$count = $unique.Count
$current = @($unique.Values | Sort-Object)

if ($UpdateBaseline) {
    $lines = @('# Unique warnings of the Debug -Analyze driver build (build\check-analyze.ps1).',
               '# The check fails when the count rises. Regenerate with -UpdateBaseline after',
               '# removing warnings; never raise it to hide a new one.',
               "count=$count") + $current
    [IO.File]::WriteAllText($Baseline, ($lines -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)
    Write-Host "analysis baseline set to $count warnings: $Baseline"
    return
}

if (-not (Test-Path -LiteralPath $Baseline)) { throw "analysis baseline not found: $Baseline (run with -UpdateBaseline)" }
$baseLines = @(Get-Content -LiteralPath $Baseline)
$countLine = $baseLines | Where-Object { $_ -match '^count=(\d+)$' } | Select-Object -First 1
if (-not $countLine) { throw "analysis baseline has no count= line: $Baseline" }
$allowed = [int]($countLine -replace '^count=', '')
$baseEntries = @($baseLines | Where-Object { $_ -and $_ -notmatch '^#|^count=' })

Write-Host "analysis warnings: $count (baseline $allowed)"
if ($count -gt $allowed) {
    # Show the file/code groups that grew, to point at the new warning(s).
    $before = @{}; foreach ($e in $baseEntries) { $before[$e] = 1 + [int]$before[$e] }
    $after = @{};  foreach ($e in $current)     { $after[$e]  = 1 + [int]$after[$e] }
    foreach ($k in ($after.Keys | Sort-Object)) {
        if ($after[$k] -gt [int]$before[$k]) { Write-Host "  more warnings than baseline: $k ($([int]$before[$k]) -> $($after[$k]))" }
    }
    foreach ($k in ($unique.Keys | Sort-Object)) { Write-Host "  $k" }
    throw "code analysis found $count warnings, more than the baseline of $allowed"
}
if ($count -lt $allowed) { Write-Host "below the baseline: lower it with -UpdateBaseline so fixed warnings stay fixed" }
