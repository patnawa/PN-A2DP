<#
.SYNOPSIS
  Forces a specific device instance to use a given INF (what Device Manager does with
  "Update driver > Have Disk"), through SetupAPI's DiInstallDevice. Run elevated.
#>
param(
    [Parameter(Mandatory)][string]$InstanceId,
    [Parameter(Mandatory)][string]$Inf,
    [switch]$PassThru
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script from an elevated PowerShell.' }
$Inf = (Resolve-Path -LiteralPath $Inf).Path
$src = @"
using System;
using System.Runtime.InteropServices;
public static class PnDrv {
    [StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct SP_DRVINFO_DATA {
        public int cbSize; public int DriverType; public IntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string ProviderName;
        public long DriverDate; public ulong DriverVersion; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct SP_DEVINSTALL_PARAMS {
        public int cbSize; public int Flags; public int FlagsEx; public IntPtr hwndParent; public IntPtr InstallMsgHandler; public IntPtr InstallMsgHandlerContext;
        public IntPtr FileQueue; public IntPtr ClassInstallReserved; public int Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string DriverPath; }
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr g, IntPtr h);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiOpenDeviceInfo(IntPtr set, string id, IntPtr h, int flags, ref SP_DEVINFO_DATA d);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiGetDeviceInstallParams(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiSetDeviceInstallParams(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiBuildDriverInfoList(IntPtr set, ref SP_DEVINFO_DATA d, int type);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiEnumDriverInfo(IntPtr set, ref SP_DEVINFO_DATA d, int type, int index, ref SP_DRVINFO_DATA drv);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiSetSelectedDriver(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DRVINFO_DATA drv);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool DiInstallDevice(IntPtr hwnd, IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DRVINFO_DATA drv, int flags, out bool reboot);
    public const int SPDIT_COMPATDRIVER = 2; public const int DI_ENUMSINGLEINF = 0x00010000; public const int DIIRFLAG_FORCE_INF = 0x00000002;
    public static int Install(string instanceId, string inf, out bool reboot) {
        reboot = false;
        IntPtr set = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
        if (set == new IntPtr(-1)) return Marshal.GetLastWin32Error();
        try {
        var dev = new SP_DEVINFO_DATA(); dev.cbSize = Marshal.SizeOf(dev);
        if (!SetupDiOpenDeviceInfo(set, instanceId, IntPtr.Zero, 0, ref dev)) return Marshal.GetLastWin32Error();
        var p = new SP_DEVINSTALL_PARAMS(); p.cbSize = Marshal.SizeOf(p);
        if (!SetupDiGetDeviceInstallParams(set, ref dev, ref p)) return Marshal.GetLastWin32Error();
        p.Flags |= DI_ENUMSINGLEINF; p.DriverPath = inf;
        if (!SetupDiSetDeviceInstallParams(set, ref dev, ref p)) return Marshal.GetLastWin32Error();
        if (!SetupDiBuildDriverInfoList(set, ref dev, SPDIT_COMPATDRIVER)) return Marshal.GetLastWin32Error();
        var drv = new SP_DRVINFO_DATA(); drv.cbSize = Marshal.SizeOf(drv);
        if (!SetupDiEnumDriverInfo(set, ref dev, SPDIT_COMPATDRIVER, 0, ref drv)) return Marshal.GetLastWin32Error();
        if (!SetupDiSetSelectedDriver(set, ref dev, ref drv)) return Marshal.GetLastWin32Error();
        if (!DiInstallDevice(IntPtr.Zero, set, ref dev, ref drv, DIIRFLAG_FORCE_INF, out reboot)) return Marshal.GetLastWin32Error();
        return 0;
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }
}
"@
if (-not ('PnDrv' -as [type])) { Add-Type -TypeDefinition $src }
$reboot = $false
# Release the host's persistent device handles before asking PnP to remove the
# old stack. Otherwise a routine update can be deferred until a system restart.
$hostService = Get-Service 'pna2dp-codechost' -ErrorAction SilentlyContinue
$restartHost = $hostService -and $hostService.Status -eq 'Running'
try {
    if ($restartHost) {
        $hostInfo = Get-CimInstance Win32_Service -Filter "Name='pna2dp-codechost'"
        $hostProcess = if ($hostInfo.ProcessId) { Get-Process -Id $hostInfo.ProcessId -ErrorAction SilentlyContinue }
        Stop-Service -InputObject $hostService -ErrorAction Stop
        $hostService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
        if ($hostProcess -and -not $hostProcess.WaitForExit(30000)) { throw 'Codec host did not exit; device update was not attempted.' }
    }
    $rc = [PnDrv]::Install($InstanceId, $Inf, [ref]$reboot)
} finally {
    if ($restartHost) { Start-Service 'pna2dp-codechost' }
}
if ($rc -ne 0) { throw "DiInstallDevice failed with Win32 error $rc (0x$('{0:x}' -f $rc))" }
Write-Host "installed $Inf on $InstanceId$(if ($reboot) { ' (reboot required)' })"
if ($PassThru) { [pscustomobject]@{ InstanceId = $InstanceId; Inf = $Inf; RebootRequired = $reboot } }
