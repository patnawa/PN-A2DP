<#
.SYNOPSIS
  Switches one paired Bluetooth stereo device to the PN-A2DP driver (or back), with the
  safety steps a first load of an unproven kernel driver needs. Run elevated.

.USAGE
  .\build\switch-device.ps1                      # the only A2DP device, to PN-A2DP
  .\build\switch-device.ps1 -Name 'CMT-SBT40D'   # pick by (part of) the friendly name
  .\build\switch-device.ps1 -Rollback            # back to the driver recorded before the switch

  Before switching it checks that test signing is active and the signing certificate is
  trusted, creates a System Restore point and records the device's current INF in
  %ProgramData%\PN-A2DP\previous-driver.txt. Afterwards it reports the device status, the
  driver service state and `pna2dpctl status`.

  Crash guard: the driver counts starts that end without a clean shutdown (a crash). After
  two in a row it refuses to load, so a crash loop ends by itself on the third boot and
  Windows boots with the speaker unavailable (Code 37). This script resets that count.

  If Windows still cannot boot, it shows the recovery environment after two failed boots:
  Troubleshoot > Advanced options > Command Prompt, then
      bcdedit /set {default} testsigning off
  and restart; the test-signed driver then cannot load and Windows boots normally. Safe Mode
  also works (the Bluetooth stack does not start there). Run this script with -Rollback
  afterwards.
#>
param(
    [string]$Name = '',
    [string]$InstanceId = '',
    [switch]$Rollback,
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'PN-A2DP')
)
$ErrorActionPreference = 'Stop'
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw "run this script from an elevated PowerShell" }

$state = Join-Path $env:ProgramData 'PN-A2DP'
New-Item -ItemType Directory -Force $state | Out-Null
$record = Join-Path $state 'previous-driver.txt'

$nodes = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'BTHENUM\{0000110B-0000-1000-8000-00805F9B34FB}*' })
if ($InstanceId) { $nodes = @($nodes | Where-Object { $_.InstanceId -eq $InstanceId }) }
if ($Name) { $nodes = @($nodes | Where-Object { $_.FriendlyName -like "*$Name*" }) }
if ($nodes.Count -eq 0) { throw "no paired Bluetooth stereo device found$(if ($Name) { " matching '$Name'" })" }
if ($nodes.Count -gt 1) { throw 'More than one device matches; use -Name to select exactly one.' }
$dev = $nodes[0]
$id = $dev.InstanceId
function Prop([string]$k) { (Get-PnpDeviceProperty -InstanceId $id -KeyName $k -ErrorAction SilentlyContinue).Data }
Write-Host "device : $($dev.FriendlyName)"
Write-Host "driver : $(Prop 'DEVPKEY_Device_DriverInfPath') ($(Prop 'DEVPKEY_Device_DriverProvider') $(Prop 'DEVPKEY_Device_DriverVersion')), service $(Prop 'DEVPKEY_Device_Service')"

$updater = Join-Path $PSScriptRoot 'update-device-driver.ps1'
if (-not (Test-Path $updater)) { $updater = Join-Path $InstallDir 'update-device-driver.ps1' }

if ($Rollback) {
    if (-not (Test-Path $record)) { throw "no previous driver recorded in $record; use the control panel's 'Use Microsoft driver' button instead" }
    $prev = (Get-Content $record | Where-Object { $_ -like "$id|*" } | Select-Object -Last 1)
    if (-not $prev) { throw "no previous driver recorded for $id" }
    $parts = $prev.Split('|')
    if ($parts.Count -lt 4 -or $parts[3] -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'The old rollback record has no package hash. OEM INF numbers can be reused; select a verified backup package instead of trusting this record.'
    }
    $inf = Join-Path $env:SystemRoot ("INF\" + $parts[1])
    if (-not (Test-Path -LiteralPath $inf) -or (Get-FileHash -LiteralPath $inf -Algorithm SHA256).Hash -ne $parts[3]) {
        throw 'The recorded previous driver is missing or its OEM INF number now identifies a different package. Restore from a verified backup.'
    }
    Write-Host "restoring $inf"
    $installed = & $updater -InstanceId $id -Inf $inf -PassThru
} else {
    # 1. preconditions: the driver can only load with test signing active and the certificate trusted
    # Query the running kernel: a BCD setting can be on while a restart is still pending.
    if (-not ('PnCodeIntegrity' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PnCodeIntegrity {
    [StructLayout(LayoutKind.Sequential)] public struct State { public uint Length, Options; }
    [DllImport("ntdll.dll")] public static extern int NtQuerySystemInformation(uint infoClass, ref State info, uint size, out uint returned);
    public static bool TestSigningActive() {
        var state = new State { Length = 8 };
        uint returned;
        int status = NtQuerySystemInformation(103, ref state, 8, out returned);
        if (status < 0) throw new InvalidOperationException("Cannot query running code-integrity state: " + status.ToString("X8"));
        return (state.Options & 2) != 0;
    }
}
'@
    }
    if (-not [PnCodeIntegrity]::TestSigningActive()) { throw "test signing is not active in the running kernel (bcdedit /set testsigning on, then restart). The driver would fail with Code 52." }
    $pkgInf = Join-Path $InstallDir 'driver\pna2dp.inf'
    $cer = Join-Path $InstallDir 'driver\pna2dp-test.cer'
    if (-not (Test-Path $pkgInf)) { throw "PN-A2DP is not installed in $InstallDir (run pna2dp-setup.exe first)" }
    if (Test-Path $cer) {
        $thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
        foreach ($store in 'Root', 'TrustedPublisher') {
            if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $thumb)) { throw "the PN-A2DP test certificate is not in LocalMachine\$store" }
        }
    }
    $sig = Get-AuthenticodeSignature (Join-Path $InstallDir 'driver\pna2dp.sys')
    Write-Host "signature: $($sig.Status) by $($sig.SignerCertificate.Subject)"
    if ($sig.Status -ne 'Valid') { throw "pna2dp.sys signature is not trusted on this machine: $($sig.StatusMessage)" }

    # 2. safety net
    try { Checkpoint-Computer -Description 'Before switching a Bluetooth device to PN-A2DP' -RestorePointType MODIFY_SETTINGS -ErrorAction Stop; Write-Host "restore point created" }
    catch { Write-Warning "no restore point: $($_.Exception.Message)" }
    $prevInf = Split-Path -Leaf (Prop 'DEVPKEY_Device_DriverInfPath')
    if ($prevInf -and (Prop 'DEVPKEY_Device_Service') -ne 'pna2dp') {
        $previousHash = (Get-FileHash -LiteralPath (Join-Path $env:SystemRoot "INF\$prevInf") -Algorithm SHA256).Hash
        "$id|$prevInf|$(Get-Date -Format s)|$previousHash" | Add-Content $record
        Write-Host "previous driver identity recorded: $prevInf ($previousHash)"
    }

    # a fresh start for the crash guard (see the header)
    $guard = 'HKLM:\SYSTEM\CurrentControlSet\Services\pna2dp\State', 'HKLM:\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters'
    foreach ($k in $guard) { Remove-ItemProperty -Path $k -Name UncleanStarts -ErrorAction SilentlyContinue }

    # 3. stage (already staged by the installer normally) and force the INF onto this node
    # Stage this exact package, including when an older PN-A2DP package is present.
    # Driver-store enumeration order does not identify the newest build.
    $r = pnputil /add-driver $pkgInf
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "the driver package could not be staged: $($r -join ' ')" }
    Write-Host ($r -join "`n")
    $oem = ([regex]::Match(($r -join "`n"), 'Published Name:\s+(oem\d+\.inf)')).Groups[1].Value
    if (-not $oem) { throw 'Cannot identify the staged package in pnputil output.' }
    Write-Host "installing $oem on $id"
    $installed = & $updater -InstanceId $id -Inf (Join-Path $env:SystemRoot "INF\$oem") -PassThru
}

# 4. result
if ($installed.RebootRequired) {
    Write-Warning 'Windows requires a manual restart to finish this device update. The running driver is not yet verified.'
    return
}
Start-Sleep -Seconds 4
$d = Get-PnpDevice -InstanceId $id
Write-Host ""
Write-Host "status : $($d.Status)  problem: $(Prop 'DEVPKEY_Device_ProblemCode')"
Write-Host "driver : $(Prop 'DEVPKEY_Device_DriverInfPath') ($(Prop 'DEVPKEY_Device_DriverProvider') $(Prop 'DEVPKEY_Device_DriverVersion')), service $(Prop 'DEVPKEY_Device_Service')"
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='pna2dp'" -ErrorAction SilentlyContinue
if ($svc) { Write-Host "pna2dp.sys: $($svc.State) ($($svc.PathName))" }
foreach ($k in 'HKLM:\SYSTEM\CurrentControlSet\Services\pna2dp\State', 'HKLM:\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters') {
    $u = (Get-ItemProperty -Path $k -Name UncleanStarts -ErrorAction SilentlyContinue).UncleanStarts
    if ($null -ne $u) { Write-Host "crash guard: $u unclean start(s) counted ($k)" }
}
$ctl = Join-Path $InstallDir 'pna2dpctl.exe'
if (-not $Rollback -and (Test-Path $ctl)) { Write-Host ""; & $ctl list; & $ctl status }
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-3) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Kernel-PnP|UserPnp|CodeIntegrity|Service Control Manager' -or $_.Message -match 'pna2dp' } |
    Select-Object -First 10 | ForEach-Object { Write-Host "event  $($_.TimeCreated.ToString('HH:mm:ss')) $($_.ProviderName) $($_.Id): $(($_.Message -split "`n")[0])" }
