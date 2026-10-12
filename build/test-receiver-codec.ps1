<# Read-only physical receiver observation. Select/activate the desired codec in
   the panel and play audio first. Never changes preferences, volume or drivers.
   Logs contain local device details and are not intended for public upload. #>
param(
    [Parameter(Mandatory)][string]$InstanceId,
    [Parameter(Mandatory)][ValidateSet('SBC','AAC','aptX','aptX HD','aptX LL','LDAC')][string]$ExpectedCodec,
    [ValidateRange(5,600)][int]$Seconds = 15,
    [ValidateSet('Release','Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cli = Join-Path $root "out\$Config\pna2dpctl.exe"
$report = Join-Path $root ('out\verification\receiver-codec-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $report | Out-Null
function SelectedIndex {
    $list = @(& $cli list)
    if ($LASTEXITCODE) { throw 'Cannot enumerate PN devices.' }
    $matchesFound = @($list | ForEach-Object {
        if ($_ -match '^\[(\d+)\].*\(([0-9A-Fa-f]{12})\)' -and $InstanceId.IndexOf($Matches[2], [StringComparison]::OrdinalIgnoreCase) -ge 0) { [int]$Matches[1] }
    })
    if ($matchesFound.Count -ne 1) { throw 'Expected one interface for the exact receiver.' }
    return $matchesFound[0]
}
function ReadCounters([int]$index) {
    $text = (& $cli stats -d $index) -join "`n"
    if ($LASTEXITCODE -or $text -notmatch 'packets sent\s*:\s*(\d+)') { throw 'Packet counters unavailable.' }
    $sent = [uint64]$Matches[1]
    if ($text -notmatch 'packets dropped\s*:\s*(\d+)\s+write failures\s*(\d+)\s+latency trims\s*(\d+)') { throw 'Error counters unavailable.' }
    return [pscustomobject]@{sent=$sent;dropped=[uint64]$Matches[1];failures=[uint64]$Matches[2];trims=[uint64]$Matches[3]}
}
$samples = [Collections.Generic.List[object]]::new()
try {
    $node = Get-PnpDevice -InstanceId $InstanceId -ErrorAction Stop
    if ($node.Class -ne 'MEDIA' -or $node.InstanceId -notlike 'BTHENUM\{0000110B-*') { throw 'Select the exact A2DP audio device instance.' }
    $index = SelectedIndex
    $caps = (& $cli caps -d $index) -join "`n"
    if ($LASTEXITCODE -or $caps -notmatch '\d+ stream endpoints') { throw 'Receiver capabilities unavailable.' }
    $caps | Set-Content -LiteralPath (Join-Path $report 'capabilities.txt') -Encoding UTF8
    $configBefore = (& $cli config -d $index) -join "`n"
    if ($LASTEXITCODE) { throw 'Saved preferences unavailable.' }
    $before = ReadCounters $index
    for ($second = 0; $second -le $Seconds; $second++) {
        $index = SelectedIndex
        $status = (& $cli status -d $index) -join "`n"
        if ($LASTEXITCODE -or $status -notmatch 'streaming \[audio running\]') { throw 'Receiver must remain streaming; start playback before this test.' }
        if ($status -notmatch '(?m)^codec\s*:\s*(.*?)\s+(\d+) Hz') { throw 'Negotiated codec unavailable.' }
        $actual = ($Matches[1] -replace '\s+\(ABR reduced\)$','').Trim(); $rate = [int]$Matches[2]
        if ($actual -ne $ExpectedCodec) { throw "Expected $ExpectedCodec but receiver is streaming $actual. No setting was changed." }
        if ($status -notmatch 'bitrate\s*:\s*(\d+)') { throw 'Bitrate unavailable.' }
        $samples.Add([pscustomobject]@{seconds=$second;codec=$actual;rate=$rate;kbps=[int]$Matches[1]})
        if ($second -lt $Seconds) { Start-Sleep -Seconds 1 }
    }
    $after = ReadCounters $index
    $configAfter = (& $cli config -d $index) -join "`n"
    if ($LASTEXITCODE -or $configAfter -ne $configBefore) { throw 'Saved preferences changed or became unavailable.' }
    if ($after.sent -le $before.sent -or $after.dropped -ne $before.dropped -or $after.failures -ne $before.failures -or $after.trims -ne $before.trims) { throw 'Packet progress failed or transport errors/counter resets were observed.' }
    Write-Host "PASS $ExpectedCodec transport: $Seconds seconds, $($after.sent-$before.sent) packets; settings preserved. Listening quality still requires human confirmation."
} finally {
    $samples | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $report 'observations.json') -Encoding UTF8
    Write-Host "Local report: $report"
}
