# Locates the MSVC toolset, the Windows SDK and the WDK (NuGet layout or installed kit)
# and exports the paths used by build.ps1. Dot-source this file.
#
#   . .\build\toolchain.ps1
#
# Override any location with environment variables:
#   PNA2DP_WDK_ROOT   e.g. C:\Users\me\wdk\c   (NuGet "c" folder) or C:\Program Files (x86)\Windows Kits\10
#   PNA2DP_SDK_ROOT   e.g. C:\Program Files (x86)\Windows Kits\10
#   PNA2DP_KIT_VER    e.g. 10.0.26100.0
#   PNA2DP_KMDF_VER   e.g. 1.33

$ErrorActionPreference = 'Stop'

function Find-VsInstance {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022 Build Tools with the C++ workload." }
    $json = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -latest | ConvertFrom-Json
    if (-not $json) { throw "No Visual Studio instance with the MSVC x64 toolset was found." }
    return $json[0].installationPath
}

$script:VsRoot   = Find-VsInstance
$script:MsvcVer  = (Get-Content (Join-Path $VsRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt')).Trim()
$script:MsvcRoot = Join-Path $VsRoot "VC\Tools\MSVC\$MsvcVer"

$script:KitVer  = if ($env:PNA2DP_KIT_VER) { $env:PNA2DP_KIT_VER } else { '10.0.26100.0' }
$script:SdkRoot = if ($env:PNA2DP_SDK_ROOT) { $env:PNA2DP_SDK_ROOT } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }

function Find-WdkRoot {
    if ($env:PNA2DP_WDK_ROOT) { return $env:PNA2DP_WDK_ROOT }
    $candidates = @(
        (Join-Path $env:USERPROFILE 'wdk\c'),
        (Join-Path $PSScriptRoot '..\.toolchain\wdk\c'),
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10')
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "Include\$KitVer\km\portcls.h")) { return (Resolve-Path $c).Path }
    }
    throw "WDK not found. Run build\get-wdk.ps1 or set PNA2DP_WDK_ROOT."
}
$script:WdkRoot = Find-WdkRoot

function Find-KmdfVer {
    if ($env:PNA2DP_KMDF_VER) { return $env:PNA2DP_KMDF_VER }
    $dir = Join-Path $WdkRoot 'Include\wdf\kmdf'
    $vers = Get-ChildItem $dir | ForEach-Object { [version]$_.Name } | Sort-Object -Descending
    return $vers[0].ToString()
}
$script:KmdfVer = Find-KmdfVer

# Tool paths
$script:CL        = Join-Path $MsvcRoot 'bin\Hostx64\x64\cl.exe'
$script:LINK      = Join-Path $MsvcRoot 'bin\Hostx64\x64\link.exe'
$script:LIB       = Join-Path $MsvcRoot 'bin\Hostx64\x64\lib.exe'
$script:RC        = Join-Path $SdkRoot  "bin\$KitVer\x64\rc.exe"
$script:SIGNTOOL  = Join-Path $SdkRoot  "bin\$KitVer\x64\signtool.exe"
$script:MAKECERT  = Join-Path $SdkRoot  "bin\$KitVer\x64\makecert.exe"
$script:CERTMGR   = Join-Path $SdkRoot  "bin\$KitVer\x64\certmgr.exe"
$script:STAMPINF  = Join-Path $WdkRoot  "bin\$KitVer\x64\stampinf.exe"
$script:INF2CAT   = Join-Path $WdkRoot  "bin\$KitVer\x86\Inf2Cat.exe"
$script:INFVERIF  = Join-Path $WdkRoot  "tools\$KitVer\x64\infverif.exe"

# Include / lib directories
$script:KmIncludes = @(
    (Join-Path $WdkRoot "Include\$KitVer\km"),
    (Join-Path $WdkRoot "Include\$KitVer\km\crt"),
    (Join-Path $WdkRoot "Include\wdf\kmdf\$KmdfVer"),
    (Join-Path $SdkRoot "Include\$KitVer\shared"),
    (Join-Path $SdkRoot "Include\$KitVer\ucrt")
)
$script:KmLibDirs = @(
    (Join-Path $WdkRoot "Lib\$KitVer\km\x64"),
    (Join-Path $WdkRoot "Lib\wdf\kmdf\x64\$KmdfVer")
)
$script:UmIncludes = @(
    (Join-Path $MsvcRoot 'include'),
    (Join-Path $SdkRoot "Include\$KitVer\ucrt"),
    (Join-Path $SdkRoot "Include\$KitVer\um"),
    (Join-Path $SdkRoot "Include\$KitVer\shared"),
    (Join-Path $SdkRoot "Include\$KitVer\winrt"),
    (Join-Path $SdkRoot "Include\$KitVer\cppwinrt")
)
$script:UmLibDirs = @(
    (Join-Path $MsvcRoot 'lib\x64'),
    (Join-Path $SdkRoot "Lib\$KitVer\ucrt\x64"),
    (Join-Path $SdkRoot "Lib\$KitVer\um\x64")
)

foreach ($p in @($CL, $LINK, $RC)) { if (-not (Test-Path $p)) { throw "Missing tool: $p" } }

function Write-ToolchainSummary {
    Write-Host "MSVC    : $MsvcRoot"
    Write-Host "SDK     : $SdkRoot ($KitVer)"
    Write-Host "WDK     : $WdkRoot (KMDF $KmdfVer)"
}
