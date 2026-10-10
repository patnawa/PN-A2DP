<#
.SYNOPSIS
  Forces a specific device instance to use a given INF (what Device Manager does with
  "Update driver > Have Disk"), through SetupAPI's DiInstallDevice. Run elevated.
#>
param(
    [Parameter(Mandatory)][string]$InstanceId,
    [Parameter(Mandatory)][string]$Inf
)
$ErrorActionPreference = 'Stop'
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
        SetupDiDestroyDeviceInfoList(set);
        return 0;
    }
}
"@
Add-Type -TypeDefinition $src
$reboot = $false
$rc = [PnDrv]::Install($InstanceId, $Inf, [ref]$reboot)
if ($rc -ne 0) { throw "DiInstallDevice failed with Win32 error $rc (0x$('{0:x}' -f $rc))" }
Write-Host "installed $Inf on $InstanceId$(if ($reboot) { ' (reboot required)' })"
