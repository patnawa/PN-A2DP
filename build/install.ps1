<#
.SYNOPSIS
  Installs the PN-A2DP driver on every paired A2DP sink (or one device) and installs the
  codec host service. Run elevated.

.USAGE
  .\build\install.ps1 -Package .\out\Release\package             # all A2DP devices
  .\build\install.ps1 -Package .\out\Release\package -Device "BTHENUM\{0000110b-...}\..."
  .\build\install.ps1 -Uninstall                                # restore the inbox driver

  Windows prefers the inbox Microsoft driver for the generic hardware ID, so the driver is
  forced onto the device node with pnputil /install (equivalent to Device Manager's
  "Have Disk" update). Windows Update may later replace it; re-run this script.
#>
param(
    [string]$Package = (Join-Path $PSScriptRoot '..\out\Release\package'),
    [string]$Device = '',
    [switch]$Uninstall,
    [switch]$AllowTestDriver,
    [switch]$NoService
)
$ErrorActionPreference = 'Stop'
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw "run this script from an elevated PowerShell" }

$sinkClass = 'BTHENUM\{0000110b-0000-1000-8000-00805f9b34fb}'
# plain Get-PnpDevice returns present and non-present nodes; -PresentOnly:$false returns only non-present ones
$devices = Get-PnpDevice | Where-Object { $_.InstanceId -like "$sinkClass*" }
if ($Device) { $devices = $devices | Where-Object { $_.InstanceId -eq $Device } }
if (-not $devices) { throw "no A2DP sink device nodes found; pair a Bluetooth headset first" }

if ($Uninstall) {
    $oem = (pnputil /enum-drivers | Select-String -Pattern 'Published Name|Original Name' | ForEach-Object { $_.Line }) -join "`n"
    $names = [regex]::Matches($oem, 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+pna2dp\.inf') | ForEach-Object { $_.Groups[1].Value }
    foreach ($n in $names) { Write-Host "removing $n"; pnputil /delete-driver $n /uninstall /force | Out-Null }
    foreach ($d in $devices) { pnputil /scan-devices | Out-Null }
    if (Test-Path "$env:ProgramFiles\PN-A2DP\pna2dp-codechost.exe") { & "$env:ProgramFiles\PN-A2DP\pna2dp-codechost.exe" --uninstall }
    Write-Host "PN-A2DP removed; Windows will reinstall the inbox A2DP driver on the next device scan."
    return
}

$inf = Join-Path $Package 'pna2dp.inf'
if (-not (Test-Path $inf)) { throw "driver package not found: $inf" }

# The portable installer exposes the same read-only verifier used by the wizard.
# Check before staging, replacing files or touching a device. Development use
# requires an explicit opt-in and an already prepared test machine.
$verifier = Join-Path (Split-Path ([IO.Path]::GetFullPath($Package))) 'pna2dp-setup.exe'
if (!(Test-Path -LiteralPath $verifier)) { throw 'Build the installer first (-Target setup), or use a complete portable distribution.' }
$verifyReport = Join-Path ([IO.Path]::GetTempPath()) ('pna2dp-install-preflight-' + [guid]::NewGuid() + '.txt')
$verifyArgs = @('--verify-package', ('"' + [IO.Path]::GetFullPath($Package) + '"'), '--report', ('"' + $verifyReport + '"'))
if ($AllowTestDriver) { $verifyArgs += '--allow-test-driver' }
$verifyProcess = Start-Process -FilePath $verifier -ArgumentList $verifyArgs -WindowStyle Hidden -PassThru
if (!$verifyProcess.WaitForExit(60000)) { $verifyProcess.Kill(); throw 'Package verification timed out.' }
if ($verifyProcess.ExitCode -ne 0) { throw "Driver verification failed. $(Get-Content -LiteralPath $verifyReport -Raw)" }

# stage in the driver store
$result = pnputil /add-driver $inf
Write-Host ($result -join "`n")
$published = ([regex]::Match(($result -join "`n"), 'Published Name:\s+(oem\d+\.inf)')).Groups[1].Value
if (-not $published) {
    # already staged: find it
    $all = (pnputil /enum-drivers) -join "`n"
    $published = ([regex]::Match($all, 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+pna2dp\.inf')).Groups[1].Value
}
if (-not $published) { throw "driver was not staged (is it signed / is test signing enabled?)" }
$storeInf = Join-Path $env:SystemRoot "INF\$published"

foreach ($d in $devices) {
    Write-Host "installing on $($d.InstanceId) ($($d.FriendlyName))"
    # Force this device node onto our INF (driver ranking otherwise keeps the inbox driver).
    & (Join-Path $PSScriptRoot 'update-device-driver.ps1') -InstanceId $d.InstanceId -Inf $storeInf
}

if (-not $NoService) {
    $hostExe = Join-Path $Package '..\pna2dp-codechost.exe'   # ($host is a read-only automatic variable)
    if (Test-Path $hostExe) {
        $dst = "$env:ProgramFiles\PN-A2DP"
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item $hostExe $dst -Force
        Get-ChildItem (Join-Path $Package '..') -Filter 'pna2dp-codec-*.dll' | Copy-Item -Destination $dst -Force
        Get-ChildItem (Join-Path $Package '..') -Filter 'freeaptx.dll' | Copy-Item -Destination $dst -Force
        Get-ChildItem (Join-Path $Package '..') -Filter 'pna2dp-panel.exe' | Copy-Item -Destination $dst -Force
        Get-ChildItem (Join-Path $Package '..') -Filter 'pna2dpctl.exe' | Copy-Item -Destination $dst -Force
        & "$dst\pna2dp-codechost.exe" --uninstall 2>$null | Out-Null
        & "$dst\pna2dp-codechost.exe" --install
    }
}
Write-Host "done. Check with: pna2dpctl list"
