<# Live test: reconnects one explicitly selected PN-A2DP receiver during playback.
   Does not change codec, volume, device bindings, services, or boot settings. #>
param(
    [Parameter(Mandatory)][string]$InstanceId,
    [ValidateRange(1,20)][int]$Cycles = 3,
    [ValidateRange(0,2000)][int]$ExpectedSbcKbps = 0,
    [ValidateSet('Release','Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root "out\$Config"
$cli = Join-Path $bin 'pna2dpctl.exe'
$report = Join-Path $root ('out\verification\reconnect-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $report | Out-Null
$results = [Collections.Generic.List[object]]::new()
function DeviceIndex {
    $indices = @(& $cli list | ForEach-Object {
        if ($_ -match '^\[(\d+)\].*\(([0-9A-Fa-f]{12})\)' -and $InstanceId.IndexOf($Matches[2], [StringComparison]::OrdinalIgnoreCase) -ge 0) { [int]$Matches[1] }
    })
    if ($indices.Count -ne 1) { throw 'The exact selected receiver must expose one PN-A2DP control interface.' }
    return $indices[0]
}
function ReadStats([int]$index) {
    $text = (& $cli stats -d $index 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $text -notmatch 'packets sent\s*:\s*(\d+)') { throw 'Packet statistics unavailable.' }
    $sent = [uint64]$Matches[1]
    if ($text -notmatch 'packets dropped\s*:\s*(\d+)\s+write failures\s*(\d+)\s+latency trims\s*(\d+)') { throw 'Transport error counters unavailable.' }
    return [pscustomobject]@{sent=$sent; dropped=[uint64]$Matches[1]; failures=[uint64]$Matches[2]; trims=[uint64]$Matches[3]}
}
function CheckQuality([string]$status) {
    # Optional receiver-specific regression. Leave unset for adaptive/congested links.
    if ($ExpectedSbcKbps -and ($status -notmatch 'codec\s*:\s*SBC\s' -or
        $status -notmatch ("bitrate\s*:\s*" + $ExpectedSbcKbps + '\s+kbps\b'))) {
        throw "Expected SBC at $ExpectedSbcKbps kbps on the selected receiver."
    }
}
try {
    $index = DeviceIndex
    $configBefore = (& $cli config -d $index) -join "`n"
    $initial = (& $cli status -d $index) -join "`n"
    if ($initial -notmatch 'streaming \[audio running\]') { throw 'Play audio on the selected receiver before starting this live test.' }
    CheckQuality $initial
    $receiverVolume = $initial -match 'receiver absolute volume'
    for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
        $index = DeviceIndex
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $reply = (& $cli reconnect -d $index 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $reply -notmatch '^reconnecting') { throw "Reconnect request failed: $reply" }
        do {
            Start-Sleep -Milliseconds 250
            $status = (& $cli status -d $index 2>&1) -join "`n"
            $ready = $status -match 'streaming \[audio running\]' -and (!$receiverVolume -or $status -match 'receiver absolute volume')
        } while (!$ready -and $timer.Elapsed.TotalSeconds -lt 30)
        if (!$ready) { throw "Cycle $cycle did not recover streaming and volume integration within 30 seconds." }
        CheckQuality $status
        $seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        $health = & (Join-Path $bin 'health_test.exe') --live $InstanceId (Join-Path $report "cycle-$cycle.txt")
        $health | Set-Content (Join-Path $report "cycle-$cycle-checks.txt")
        if ($LASTEXITCODE -ne 0) { throw "Cycle $cycle lost the selected device's audio endpoint or control interface." }
        $before = ReadStats $index
        Start-Sleep -Seconds 3
        $after = ReadStats $index
        if ($after.sent -le $before.sent -or $after.dropped -ne $before.dropped -or $after.failures -ne $before.failures -or $after.trims -ne $before.trims) { throw "Cycle $cycle transport stalled, reset counters, or recorded an error after recovery." }
        CheckQuality ((& $cli status -d $index) -join "`n")
        if (((& $cli config -d $index) -join "`n") -ne $configBefore) { throw "Cycle $cycle changed saved audio preferences." }
        $result = [pscustomobject]@{cycle=$cycle; passed=$true; recoverySeconds=$seconds; packets=$after.sent-$before.sent; drops=0; writeFailures=0; trims=0}
        $results.Add($result)
        Write-Host "PASS reconnect $cycle : ${seconds}s, $($result.packets) packets after recovery, no transport errors; endpoint and settings retained."
    }
} catch {
    $results.Add([pscustomobject]@{passed=$false; error=$_.Exception.Message})
    throw
} finally {
    $results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'results.json') -Encoding UTF8
    Write-Host "Report: $report"
}
