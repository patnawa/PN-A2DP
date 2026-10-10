<#
.SYNOPSIS
  Builds PN-A2DP: kernel driver, user-mode codec host, control tool and tests.

.USAGE
  .\build\build.ps1                  # everything, Release x64
  .\build\build.ps1 -Target driver   # driver only
  .\build\build.ps1 -Target tests    # SBC self test
  .\build\build.ps1 -Config Debug
  .\build\build.ps1 -Sign            # test-sign the driver with a self-signed cert (see sign-test.ps1)

  No Visual Studio IDE or WDK VSIX required: cl/link are invoked directly with the
  WDK NuGet layout (see get-wdk.ps1 and toolchain.ps1).
#>
param(
    [ValidateSet('all','driver','host','cli','tests')][string]$Target = 'all',
    [ValidateSet('Release','Debug')][string]$Config = 'Release',
    [switch]$Sign,
    [switch]$Verbose
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'toolchain.ps1')
Write-ToolchainSummary

$Out = Join-Path $Root "out\$Config"
$Obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $Obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$toolArgs) {
    if ($Verbose) { Write-Host "> $exe $($toolArgs -join ' ')" }
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed with exit code $LASTEXITCODE" }
}

# ---------------------------------------------------------------------------
# Kernel driver
# ---------------------------------------------------------------------------
function Build-Driver {
    $src = Join-Path $Root 'driver\pna2dp\src'
    $inc = Join-Path $Root 'driver\pna2dp\inc'
    $objd = Join-Path $Obj 'driver'
    New-Item -ItemType Directory -Force $objd | Out-Null

    $defs = @('/D_WIN64','/D_AMD64_','/DAMD64','/DWINVER=0x0A00','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0A00000A',
              "/DKMDF_VERSION_MAJOR=$($KmdfVer.Split('.')[0])","/DKMDF_VERSION_MINOR=$($KmdfVer.Split('.')[1])",
              '/DDEPRECATE_DDK_FUNCTIONS=1','/D_NEW_DELETE_OPERATORS_','/DPC_IMPLEMENTATION','/DUNICODE','/D_UNICODE','/DPOOL_NX_OPTIN=1')
    if ($Config -eq 'Debug') { $defs += '/DDBG=1','/D_DEBUG' } else { $defs += '/DNDEBUG' }
    $opt = if ($Config -eq 'Debug') { @('/Od','/Oy-') } else { @('/O2','/Oi') }
    $common = @('/nologo','/c','/W4','/WX-','/GS','/Gy','/Zi','/kernel','/GR-','/EHs-c-','/Zp8','/FC','/guard:cf',
                '/wd4201','/wd4214','/wd4127','/wd4324','/wd4100','/wd4457','/wd4456','/Zc:wchar_t','/Zc:forScope','/Zc:inline','/Zc:strictStrings-',
                '/std:c++17','/permissive-',"/Fd$objd\pna2dp.pdb") + $opt + $defs
    $incs = @("/I$inc") + ($KmIncludes | ForEach-Object { "/I$_" })

    $cpp = Get-ChildItem (Join-Path $src '*.cpp')
    $objs = @()
    foreach ($f in $cpp) {
        $o = Join-Path $objd ($f.BaseName + '.obj')
        Invoke-Tool $CL ($common + $incs + @("/Fo$o", $f.FullName))
        $objs += $o
    }
    # shared C sources (SBC codec)
    $sbc = Join-Path $Root 'common\sbc\sbc.c'
    $o = Join-Path $objd 'sbc.obj'
    $ccommon = $common | Where-Object { $_ -notmatch '^/std:|^/permissive|^/GR|^/EHs|^/Zc:strictStrings' }
    Invoke-Tool $CL ($ccommon + @('/TC') + $incs + @("/Fo$o", $sbc))
    $objs += $o

    # resources
    $res = Join-Path $objd 'pna2dp.res'
    $rcIncs = @("/I$inc") + ($KmIncludes | ForEach-Object { "/I$_" }) + @("/I$SdkRoot\Include\$KitVer\um")
    Invoke-Tool $RC (@('/nologo','/D_WIN64','/DWINNT=1') + $rcIncs + @("/fo$res", (Join-Path $src 'pna2dp.rc')))

    $sys = Join-Path $Out 'pna2dp.sys'
    $libs = $KmLibDirs | ForEach-Object { "/LIBPATH:$_" }
    $largs = @('/nologo','/DRIVER','/SUBSYSTEM:NATIVE,10.0','/ENTRY:FxDriverEntry','/NODEFAULTLIB','/RELEASE','/INCREMENTAL:NO','/OPT:REF','/OPT:ICF',
               '/DEBUG','/MACHINE:X64','/NXCOMPAT','/DYNAMICBASE','/guard:cf','/MERGE:_TEXT=.text','/MERGE:_PAGE=PAGE','/SECTION:INIT,d',
               '/OSVERSION:10.0','/VERSION:10.0','/INTEGRITYCHECK',"/OUT:$sys","/PDB:$Out\pna2dp.pdb","/MAP:$Out\pna2dp.map") + $libs + $objs + @($res) +
             @('portcls.lib','stdunk.lib','ksguid.lib','libcntpr.lib','ntoskrnl.lib','hal.lib','wmilib.lib','wdmsec.lib','BufferOverflowFastFailK.lib','wdfdriverentry.lib','wdfldr.lib')
    Invoke-Tool $LINK $largs
    Write-Host "built $sys ($((Get-Item $sys).Length) bytes)"

    # package: INF + sys (+ cat when signing)
    $pkg = Join-Path $Out 'package'
    New-Item -ItemType Directory -Force $pkg | Out-Null
    Copy-Item $sys $pkg -Force
    Copy-Item (Join-Path $Root 'driver\pna2dp\inf\pna2dp.inf') $pkg -Force
    if (Test-Path $STAMPINF) {
        Invoke-Tool $STAMPINF @('-f', (Join-Path $pkg 'pna2dp.inf'), '-d', '*', '-v', '0.1.0.0', '-k', $KmdfVer)
    }
    if ($Sign) { & (Join-Path $PSScriptRoot 'sign-test.ps1') -Package $pkg }
}

# ---------------------------------------------------------------------------
# User-mode pieces
# ---------------------------------------------------------------------------
function Build-UserExe([string]$name, [string[]]$sources, [string[]]$extraIncs, [string[]]$extraLibs, [string[]]$extraDefs) {
    $objd = Join-Path $Obj $name
    New-Item -ItemType Directory -Force $objd | Out-Null
    $opt = if ($Config -eq 'Debug') { @('/Od','/MTd','/D_DEBUG') } else { @('/O2','/MT','/DNDEBUG') }
    $common = @('/nologo','/c','/W4','/WX-','/Zi','/EHsc','/std:c++17','/permissive-','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/D_CRT_SECURE_NO_WARNINGS',"/Fd$objd\$name.pdb") + $opt + $extraDefs
    $incs = @("/I$(Join-Path $Root 'driver\pna2dp\inc')", "/I$(Join-Path $Root 'common')") + ($UmIncludes | ForEach-Object { "/I$_" }) + $extraIncs
    $objs = @()
    foreach ($s in $sources) {
        $f = Get-Item $s
        $o = Join-Path $objd ($f.BaseName + '.obj')
        $c = $common
        if ($f.Extension -eq '.c') { $c = $common | Where-Object { $_ -notmatch '^/std:|^/permissive|^/EHsc' }; $c += '/TC' }
        Invoke-Tool $CL ($c + $incs + @("/Fo$o", $f.FullName))
        $objs += $o
    }
    $exe = Join-Path $Out "$name.exe"
    $libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }
    Invoke-Tool $LINK (@('/nologo','/DEBUG','/INCREMENTAL:NO','/SUBSYSTEM:CONSOLE',"/OUT:$exe","/PDB:$Out\$name.pdb") + $libs + $objs + $extraLibs)
    Write-Host "built $exe"
}

function Build-Tests {
    Build-UserExe 'sbc_test' @((Join-Path $Root 'common\sbc\sbc.c'), (Join-Path $Root 'common\sbc\test\sbc_test.c')) @() @() @()
    Build-UserExe 'plugin_test' @((Join-Path $Root 'codec-host\test\plugin_test.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @() @()
}

function Build-Cli {
    Build-UserExe 'pna2dpctl' @((Join-Path $Root 'tools\pna2dpctl\pna2dpctl.cpp')) @() @('setupapi.lib','cfgmgr32.lib','advapi32.lib','ole32.lib') @()
}

function Build-Host {
    $srcs = @((Join-Path $Root 'codec-host\host\main.cpp'), (Join-Path $Root 'codec-host\host\device.cpp'), (Join-Path $Root 'codec-host\host\plugins.cpp'),
              (Join-Path $Root 'codec-host\plugins\sbc\sbc_plugin.cpp'), (Join-Path $Root 'common\sbc\sbc.c'))
    Build-UserExe 'pna2dp-codechost' $srcs @("/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','cfgmgr32.lib','advapi32.lib','ole32.lib') @()
}

switch ($Target) {
    'driver' { Build-Driver }
    'tests'  { Build-Tests }
    'cli'    { Build-Cli }
    'host'   { Build-Host }
    'all'    { Build-Tests; Build-Driver; Build-Cli; Build-Host }
}
Write-Host "done."
