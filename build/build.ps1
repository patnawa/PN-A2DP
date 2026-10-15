<#
.SYNOPSIS
  Builds PN-A2DP: kernel driver, user-mode codec host, control tool and tests.

.USAGE
  .\build\build.ps1                  # everything, Release x64
  .\build\build.ps1 -Target driver   # driver only
  .\build\build.ps1 -Target tests    # SBC self test
  .\build\build.ps1 -Target service-tests # focused Windows service failure tests
  .\build\build.ps1 -Target process-tests # helper-process responsiveness tests
  .\build\build.ps1 -Target health-tests # audio diagnostics and quality-history tests
  .\build\build.ps1 -Target latency-tests # speaker-delay measurement analysis (synthetic recordings)
  .\build\build.ps1 -Target quality-tests # end-to-end audio quality harness (quality_test.exe; run it with
                                          # build\test-audio-quality.py, needs FFmpeg and the codec plugins)
  .\build\build.ps1 -Target windows  # application-only Windows mode installer + portable
  .\build\build.ps1 -Target gui      # control panel (pna2dp-panel.exe)
  .\build\build.ps1 -Target plugins  # LDAC/aptX/AAC codec plugins (needs the third_party submodules)
  .\build\build.ps1 -Target setup    # installer (pna2dp-setup.exe) embedding everything found in out\<Config>
  .\build\build.ps1 -Target package  # driver + host + tools + plugins + installer, collected in out\<Config>\dist
  .\build\build.ps1 -Target dist     # repackage existing binaries, preserving their signatures
  .\build\build.ps1 -Config Debug
  .\build\build.ps1 -Sign            # test-sign the driver with a self-signed cert (see sign-test.ps1)

  No Visual Studio IDE or WDK VSIX required: cl/link are invoked directly with the
  WDK NuGet layout (see get-wdk.ps1 and toolchain.ps1).
#>
param(
    [ValidateSet('all','driver','host','cli','tests','security-tests','core-tests','plugin-tests','quality-tests','plugins','service-tests','process-tests','health-tests','latency-tests','gui','setup','package','dist','windows')][string]$Target = 'all',
    [ValidateSet('Release','Debug')][string]$Config = 'Release',
    [switch]$Sign,
    [switch]$Sanitize,
    [switch]$Analyze,
    [string]$DriverPackage = '',
    [switch]$Production,
    [switch]$Verbose
)
$WindowsMode = $Target -eq 'windows'
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($Production -and ($Sign -or $Config -ne 'Release' -or !$DriverPackage)) { throw '-Production requires Release, -DriverPackage from Microsoft, and no -Sign.' }
if (($Production -or $DriverPackage) -and $Target -notin @('setup','dist')) { throw 'Use -Target setup or dist to preserve the signed driver bytes.' }
if ($DriverPackage) { $DriverPackage = (Resolve-Path -LiteralPath $DriverPackage).Path }
. (Join-Path $PSScriptRoot 'toolchain.ps1')
. (Join-Path $PSScriptRoot 'codec-distribution.ps1')
Write-ToolchainSummary

$Out = Join-Path $Root "out\$Config"
if ($Sanitize) {
    if ($Target -ne 'tests') { throw '-Sanitize is supported for -Target tests only' }
    $Out = Join-Path $Root "out\$Config-Asan"
}
$Obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $Obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$toolArgs) {
    if ($Verbose) { Write-Host "> $exe $($toolArgs -join ' ')" }
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed with exit code $LASTEXITCODE" }
}

function Remove-BuildDirectory([string]$path) {
    $resolved = [IO.Path]::GetFullPath($path)
    $allowedRoot = [IO.Path]::GetFullPath($Out).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the build output: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
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
    # /Qspectre: Spectre variant 1 load barriers. The driver links only WDK libraries
    # (/NODEFAULTLIB), so no Spectre-mitigated MSVC runtime libraries are needed.
    $common = @('/nologo','/c','/W4','/WX','/GS','/Gy','/Zi','/kernel','/GR-','/EHs-c-','/Zp8','/FC','/guard:cf','/Qspectre',
                '/wd4201','/wd4214','/wd4127','/wd4324','/wd4100','/wd4471','/Zc:wchar_t','/Zc:forScope','/Zc:inline','/Zc:strictStrings-',
                '/std:c++17','/permissive-',"/Fd$objd\pna2dp.pdb") + $opt + $defs
    $incs = @("/I$inc") + ($KmIncludes | ForEach-Object { "/I$_" })
    # /analyze:WX- keeps /analyze findings as warnings under /WX; check-analyze.ps1 ratchets them.
    if ($Analyze) { $common += '/analyze', '/analyze:external-', '/analyze:WX-' }

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
    # /CETCOMPAT marks the image shadow-stack compatible (kernel-mode hardware-enforced
    # stack protection loads only drivers that carry this flag).
    $largs = @('/nologo','/DRIVER','/SUBSYSTEM:NATIVE,10.0','/ENTRY:FxDriverEntry','/NODEFAULTLIB','/RELEASE','/INCREMENTAL:NO','/OPT:REF','/OPT:ICF',
               '/DEBUG','/MACHINE:X64','/NXCOMPAT','/DYNAMICBASE','/guard:cf','/CETCOMPAT','/MERGE:_TEXT=.text','/MERGE:_PAGE=PAGE','/SECTION:INIT,d',
               '/OSVERSION:10.0','/VERSION:10.0','/INTEGRITYCHECK',"/OUT:$sys","/PDB:$Out\pna2dp.pdb","/MAP:$Out\pna2dp.map") + $libs + $objs + @($res) +
             @('portcls.lib','stdunk.lib','ksguid.lib','libcntpr.lib','ntoskrnl.lib','hal.lib','wmilib.lib','wdmsec.lib','BufferOverflowFastFailK.lib','wdfdriverentry.lib','wdfldr.lib')
    Invoke-Tool $LINK $largs
    Write-Host "built $sys ($((Get-Item $sys).Length) bytes)"
    # Universal (DCH) driver rule: only DDIs on the WDK's universal list may be imported.
    Test-UniversalDriverApis $sys
    # No auto-vectorizer ISA dispatch in the kernel image: its AVX2 / AVX-512 paths run when
    # __isa_available says so, which nothing sets in the kernel (dead code), and would use vector
    # state without KeSaveExtendedProcessorState if anything did. pcm.cpp keeps its loops scalar
    # (#pragma loop(no_vector)), so user-mode tests time and run exactly the driver's code.
    if ((Get-Content -LiteralPath (Join-Path $Out 'pna2dp.map') -Raw) -match '__isa_available') {
        throw "pna2dp.sys contains auto-vectorizer ISA dispatch (__isa_available): keep that loop scalar (#pragma loop(no_vector))"
    }

    # package: INF + sys (+ cat when signing)
    $pkg = Join-Path $Out 'package'
    New-Item -ItemType Directory -Force $pkg | Out-Null
    # a catalog from an earlier -Sign run would no longer match the fresh binary
    if (-not $Sign) { Remove-Item (Join-Path $pkg 'pna2dp.cat'), (Join-Path $pkg 'pna2dp-test.cer') -Force -ErrorAction SilentlyContinue }
    Copy-Item $sys $pkg -Force
    Copy-Item (Join-Path $Root 'driver\pna2dp\inf\pna2dp.inf') $pkg -Force
    if (Test-Path $STAMPINF) {
        # Preserve the source INF's version. Reusing one version for changed
        # packages can cause Windows to keep an older driver-store package.
        Invoke-Tool $STAMPINF @('-f', (Join-Path $pkg 'pna2dp.inf'), '-d', '*', '-k', $KmdfVer)
    }
    # Validate every packaged INF, not only -Production ones; /w also applies the
    # Windows (universal/DCH) driver INF rules.
    if (-not (Test-Path -LiteralPath $INFVERIF)) { throw "InfVerif not found: $INFVERIF" }
    Invoke-Tool $INFVERIF @('/w', '/v', (Join-Path $pkg 'pna2dp.inf'))
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
    if ($Sanitize) { $common += '/fsanitize=address' }
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

function Build-ServiceTests {
    Build-UserExe 'service_failure_test' @((Join-Path $Root 'tools\common\test\service_failure_test.cpp')) @("/I$(Join-Path $Root 'tools\common')", "/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib') @()
}

function Build-ProcessTests {
    Build-UserExe 'process_wait_test' @((Join-Path $Root 'tools\common\test\process_wait_test.cpp')) @("/I$(Join-Path $Root 'tools\common')", "/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib') @()
}

function Build-HealthTests {
    Build-UserExe 'health_test' @((Join-Path $Root 'tools\common\test\health_test.cpp'), (Join-Path $Root 'tools\common\pnhealth.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp'), (Join-Path $Root 'tools\common\pnaudio.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp')) @("/I$(Join-Path $Root 'tools\common')", "/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib','uuid.lib') @()
}

# The speaker-delay measurement (pnlatency.cpp): its analysis runs on synthetic recordings;
# the test plays and records nothing.
function Build-LatencyTests {
    Build-UserExe 'latency_test' @((Join-Path $Root 'tools\common\test\latency_test.cpp'), (Join-Path $Root 'tools\common\pnlatency.cpp')) @("/I$(Join-Path $Root 'tools\common')") @('ole32.lib') @()
}

function Build-Tests {
    Build-SecurityTests
    Build-HealthTests
    Build-LatencyTests
    Build-ServiceTests
    Build-ProcessTests
    Build-UserExe 'boot_mode_test' @((Join-Path $Root 'tools\common\test\boot_mode_test.cpp'), (Join-Path $Root 'tools\common\pnboot.cpp'), (Join-Path $Root 'tools\common\pnsigning.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib','uuid.lib') @()
    Build-UserExe 'windows_audio_test' @((Join-Path $Root 'tools\audio-test\windows_audio_test.cpp'), (Join-Path $Root 'tools\common\pnaudio.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib','uuid.lib') @()
    Build-UserExe 'signing_test' @((Join-Path $Root 'tools\common\test\signing_test.cpp'), (Join-Path $Root 'tools\common\pnsigning.cpp')) @() @() @()
    Build-UserExe 'spatial_test' @((Join-Path $Root 'tools\audio-test\spatial_test.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp'), (Join-Path $Root 'tools\common\pnspatial.cpp'), (Join-Path $Root 'tools\common\pnaudio.cpp')) @() @('cfgmgr32.lib','ole32.lib','windowsapp.lib') @()
    Build-UserExe 'sbc_capture' @((Join-Path $Root 'tools\audio-test\sbc_capture.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp')) @() @('cfgmgr32.lib','ole32.lib') @()
    Build-UserExe 'ks_test' @((Join-Path $Root 'tools\audio-test\ks_test.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp')) @() @('ksuser.lib','cfgmgr32.lib','ole32.lib','advapi32.lib') @()
    Build-UserExe 'audio_test' @((Join-Path $Root 'tools\audio-test\audio_test.cpp')) @() @('ole32.lib','uuid.lib') @()
    Build-UserExe 'core_test' @((Join-Path $Root 'driver\pna2dp\test\core_test.cpp'), (Join-Path $Root 'driver\pna2dp\src\a2dp_codec.cpp'), (Join-Path $Root 'driver\pna2dp\src\rtp.cpp'), (Join-Path $Root 'driver\pna2dp\src\pcm.cpp'), (Join-Path $Root 'driver\pna2dp\src\avdtp_wire.cpp'), (Join-Path $Root 'common\sbc\sbc.c')) @() @() @()
    Build-UserExe 'sbc_test' @((Join-Path $Root 'common\sbc\sbc.c'), (Join-Path $Root 'common\sbc\test\sbc_test.c')) @() @() @()
    Build-UserExe 'plugin_test' @((Join-Path $Root 'codec-host\test\plugin_test.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @() @()
    Build-UserExe 'pnctl_test' @((Join-Path $Root 'tools\common\test\pnctl_test.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp')) `
        @("/I$(Join-Path $Root 'tools\common')", "/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','user32.lib') @()
}

function Build-SecurityTests {
    Build-UserExe 'secure_path_test' @((Join-Path $Root 'tools\common\test\secure_path_test.cpp')) @("/I$(Join-Path $Root 'tools\common')") @('advapi32.lib') @()
}

function Build-CoreTests {
    Build-UserExe 'core_test' @((Join-Path $Root 'driver\pna2dp\test\core_test.cpp'), (Join-Path $Root 'driver\pna2dp\src\a2dp_codec.cpp'), (Join-Path $Root 'driver\pna2dp\src\rtp.cpp'), (Join-Path $Root 'driver\pna2dp\src\pcm.cpp'), (Join-Path $Root 'driver\pna2dp\src\avdtp_wire.cpp'), (Join-Path $Root 'common\sbc\sbc.c')) @() @() @()
}

# The audio quality harness: the production PCM conversion, codec selection, SBC packing,
# RTP writer, kernel SBC encoder and the built-in SBC plugin, with the pump / WaveRT data path
# emulated around them; codec plugins are loaded from out\<Config> at run time.
function Build-QualityTests {
    $srcs = @((Join-Path $Root 'tools\audio-test\quality_test.cpp'), (Join-Path $Root 'driver\pna2dp\src\pcm.cpp'), (Join-Path $Root 'driver\pna2dp\src\a2dp_codec.cpp'),
              (Join-Path $Root 'driver\pna2dp\src\rtp.cpp'), (Join-Path $Root 'common\sbc\sbc.c'), (Join-Path $Root 'codec-host\plugins\sbc\sbc_plugin.cpp'))
    Build-UserExe 'quality_test' $srcs @("/I$(Join-Path $Root 'codec-host\include')") @() @()
}

function Build-Cli {
    Build-UserExe 'pna2dpctl' @((Join-Path $Root 'tools\pna2dpctl\pna2dpctl.cpp'), (Join-Path $Root 'tools\common\pnctl.cpp'), (Join-Path $Root 'tools\common\pnspatial.cpp'), (Join-Path $Root 'tools\common\pnaudio.cpp'), (Join-Path $Root 'tools\common\pnlatency.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','cfgmgr32.lib','advapi32.lib','ole32.lib','windowsapp.lib','newdev.lib','shell32.lib','crypt32.lib','user32.lib') @()
}

function Build-Host {
    $srcs = @((Join-Path $Root 'codec-host\host\main.cpp'), (Join-Path $Root 'codec-host\host\device.cpp'), (Join-Path $Root 'codec-host\host\plugins.cpp'),
              (Join-Path $Root 'codec-host\plugins\sbc\sbc_plugin.cpp'), (Join-Path $Root 'common\sbc\sbc.c'))
    Build-UserExe 'pna2dp-codechost' $srcs @("/I$(Join-Path $Root 'codec-host\include')") @('setupapi.lib','cfgmgr32.lib','advapi32.lib','ole32.lib') @()
}

# ---------------------------------------------------------------------------
# GUI executables (control panel, installer): sources + .rc, Windows subsystem
# ---------------------------------------------------------------------------
$CommonSrcs = @((Join-Path $Root 'tools\common\pnctl.cpp'), (Join-Path $Root 'tools\common\pndrv.cpp'))
$CommonIncs = @("/I$(Join-Path $Root 'tools\common')", "/I$(Join-Path $Root 'codec-host\include')")
$CommonLibs = @('setupapi.lib','newdev.lib','cfgmgr32.lib','advapi32.lib','shell32.lib','crypt32.lib','ole32.lib','comctl32.lib','comdlg32.lib','user32.lib','gdi32.lib','uuid.lib')

function Build-WinExe([string]$name, [string[]]$sources, [string]$rcFile, [string[]]$extraIncs, [string[]]$extraLibs, [string[]]$rcIncs) {
    $objd = Join-Path $Obj $name
    New-Item -ItemType Directory -Force $objd | Out-Null
    $opt = if ($Config -eq 'Debug') { @('/Od','/MTd','/D_DEBUG') } else { @('/O2','/MT','/DNDEBUG') }
    $common = @('/nologo','/c','/W4','/WX-','/Zi','/EHsc','/std:c++17','/permissive-','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/D_CRT_SECURE_NO_WARNINGS','/DNOMINMAX-',"/Fd$objd\$name.pdb") + $opt
    $common = $common | Where-Object { $_ -ne '/DNOMINMAX-' }
    $incs = @("/I$(Join-Path $Root 'driver\pna2dp\inc')", "/I$(Join-Path $Root 'common')") + $CommonIncs + ($UmIncludes | ForEach-Object { "/I$_" }) + $extraIncs
    $objs = @()
    foreach ($s in $sources) {
        $f = Get-Item $s
        $o = Join-Path $objd ($f.BaseName + '.obj')
        Invoke-Tool $CL ($common + $incs + @("/Fo$o", $f.FullName))
        $objs += $o
    }
    $res = Join-Path $objd "$name.res"
    $rcDir = Split-Path (Resolve-Path $rcFile)
    $rcArgs = @('/nologo', "/I$rcDir", "/I$(Join-Path $Root 'driver\pna2dp\inc')", "/I$SdkRoot\Include\$KitVer\um", "/I$SdkRoot\Include\$KitVer\shared") + $rcIncs + @("/fo$res", (Resolve-Path $rcFile).Path)
    Invoke-Tool $RC $rcArgs
    $exe = Join-Path $Out "$name.exe"
    $libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }
    Invoke-Tool $LINK (@('/nologo','/DEBUG','/INCREMENTAL:NO','/SUBSYSTEM:WINDOWS','/MANIFEST:NO','/DYNAMICBASE','/NXCOMPAT',"/OUT:$exe","/PDB:$Out\$name.pdb") + $libs + $objs + @($res) + $CommonLibs + $extraLibs)
    Write-Host "built $exe"
}

function Build-Gui {
    $srcs = @((Join-Path $Root 'tools\pna2dp-panel\panel.cpp'), (Join-Path $Root 'tools\pna2dp-panel\bootdlg.cpp'), (Join-Path $Root 'tools\common\pnhealth.cpp'), (Join-Path $Root 'tools\common\pnboot.cpp'), (Join-Path $Root 'tools\common\pnsigning.cpp'), (Join-Path $Root 'tools\common\pnaudio.cpp'), (Join-Path $Root 'tools\common\pnlatency.cpp')) + $CommonSrcs
    Build-WinExe 'pna2dp-panel' $srcs (Join-Path $Root 'tools\pna2dp-panel\panel.rc') @() @() @()
}

function Build-Plugins {
    $markers = @{
        ldac = 'third_party\ldacBT\libldac\inc\ldacBT.h'
        aptx = 'third_party\libfreeaptx\freeaptx.h'
        aac  = 'third_party\fdk-aac\libAACenc\include\aacenc_lib.h'
    }
    foreach ($p in @('ldac','aptx','aac')) {
        $m = Join-Path $Root $markers[$p]
        if ($p -eq 'aac' -and -not (Test-Path $m)) { $m = Join-Path $Root 'third_party\fdk-aac\cpp\libAACenc\include\aacenc_lib.h' }
        if (-not (Test-Path $m)) { Write-Warning "submodule for the $p plugin is not checked out (git submodule update --init --recursive); skipping it"; continue }
        & (Join-Path $Root "codec-host\plugins\$p\build.ps1") -Config $Config
    }
}

# The installer embeds every deliverable found in out\<Config> as a PAYLOAD resource. A text
# index (name|file|subdir|component) tells the installer what each blob is.
function Build-Setup {
    $objd = Join-Path $Obj 'pna2dp-setup'
    New-Item -ItemType Directory -Force $objd | Out-Null
    $pkg = if ($DriverPackage) { $DriverPackage } else { Join-Path $Out 'package' }
    if ($Production -and (Test-Path -LiteralPath (Join-Path $pkg 'pna2dp-test.cer'))) { throw 'Production packages must not contain the development test certificate.' }
    if ($Production) { Invoke-Tool $INFVERIF @('/w','/v',(Join-Path $pkg 'pna2dp.inf')) }
    $items = @(
        @{ file = (Join-Path $pkg 'pna2dp.inf');            sub = 'driver';  comp = 'driver';   required = $true  },
        @{ file = (Join-Path $pkg 'pna2dp.sys');            sub = 'driver';  comp = 'driver';   required = $true  },
        @{ file = (Join-Path $pkg 'pna2dp.cat');            sub = 'driver';  comp = 'driver';   required = $false },
        @{ file = (Join-Path $pkg 'pna2dp-test.cer');       sub = 'driver';  comp = 'driver';   required = $false },
        @{ file = (Join-Path $Out 'pna2dp-codechost.exe');  sub = '';        comp = 'host';     required = $true  },
        @{ file = (Join-Path $Out 'pna2dp-codec-ldac.dll'); sub = '';        comp = 'ldac';     required = $false },
        @{ file = (Join-Path $Out 'pna2dp-codec-aptx.dll'); sub = '';        comp = 'aptx';     required = $false },
        @{ file = (Join-Path $Out 'freeaptx.dll');          sub = '';        comp = 'aptx';     required = $false },
        @{ file = (Join-Path $Out 'pna2dp-codec-aac.dll');  sub = '';        comp = 'aac';      required = $false },
        @{ file = (Join-Path $Out 'pna2dp-panel.exe');      sub = '';        comp = 'panel';    required = $true  },
        @{ file = (Join-Path $Out 'pna2dpctl.exe');         sub = '';        comp = 'cli';      required = $true  },
        @{ file = (Join-Path $Root 'LICENSE');              sub = '';        comp = 'core';     required = $true  },
        @{ file = (Join-Path $Root 'THIRD-PARTY-NOTICES.md'); sub = '';      comp = 'core';     required = $true  },
        @{ file = (Join-Path $Root 'docs\13-windows-mode.md'); sub = ''; comp = 'core'; required = $true },
        @{ file = (Join-Path $Root 'docs\14-audio-diagnostics.md'); sub = ''; comp = 'core'; required = $true },
        @{ file = (Join-Path $Root 'README.md');            sub = '';        comp = 'core';     required = $false }
    )
    if ($WindowsMode) { $items = @($items | Where-Object { $_.comp -in @('panel','cli','core') }) }
    else { $items += @(Get-CodecRedistributionItems $Out) }
    # The payload files are copied next to the generated .rc under ASCII names and referenced
    # relatively (rc.exe resolves them through /I$objd), so the checkout path never appears in
    # the resource script and non-ASCII paths work.
    $payloadDir = Join-Path $objd 'payload'
    Remove-BuildDirectory $payloadDir
    New-Item -ItemType Directory -Force $payloadDir | Out-Null
    $rcLines = @('// generated by build.ps1 - do not edit')
    $index = @()
    $n = 0
    foreach ($i in $items) {
        if (-not (Test-Path $i.file)) {
            if ($i.required) { throw "installer payload missing: $($i.file) (build the other targets first)" }
            Write-Warning "installer: optional payload not found, skipped: $($i.file)"
            continue
        }
        $n++
        $id = "PAYLOAD_$n"
        $staged = "payload\$id.bin"
        Copy-Item $i.file (Join-Path $objd $staged) -Force
        $rcLines += "$id PAYLOAD `"$($staged.Replace('\', '\\'))`""
        $index += "$id|$([IO.Path]::GetFileName($i.file))|$($i.sub)|$($i.comp)|$((Get-Item $i.file).Length)"
    }
    $indexFile = Join-Path $objd 'payload\index.txt'
    [IO.File]::WriteAllText($indexFile, ($index -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)
    $rcLines += 'PAYLOAD_INDEX PAYLOAD "payload\\index.txt"'
    $versionMatch = [regex]::Match((Get-Content (Join-Path $Root 'tools\pna2dp-panel\version.h') -Raw), 'PN_PANEL_VERSION_TEXT\s+"(\d+\.\d+\.\d+\.\d+)"')
    if (!$versionMatch.Success) { throw 'Application release version is unavailable.' }
    $version = $versionMatch.Groups[1].Value
    if ((Get-Item (Join-Path $Out 'pna2dp-panel.exe')).VersionInfo.ProductVersion -ne $version) {
        throw 'Control panel version does not match setup. Build -Target gui before packaging existing binaries.'
    }
    [IO.File]::WriteAllText((Join-Path $objd 'payload.rc'), ($rcLines -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)
    $srcs = @((Join-Path $Root 'tools\pna2dp-setup\setup.cpp'), (Join-Path $Root 'tools\common\pnsigning.cpp')) + $CommonSrcs
    Build-WinExe 'pna2dp-setup' $srcs (Join-Path $Root 'tools\pna2dp-setup\setup.rc') @() @() @("/I$objd")
    if ($Production) {
        $report = Join-Path $Out 'production-preflight.txt'
        $process = Start-Process -FilePath (Join-Path $Out 'pna2dp-setup.exe') -ArgumentList @('--check','--report', ('"' + $report + '"')) -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Production signature verification timed out.' }
        if ($process.ExitCode -ne 0) { throw "Production package rejected. See $report" }
        Invoke-Tool $SIGNTOOL @('verify','/kp','/v','/c',(Join-Path $pkg 'pna2dp.cat'),(Join-Path $pkg 'pna2dp.sys'))
        Invoke-Tool $SIGNTOOL @('verify','/kp','/v','/c',(Join-Path $pkg 'pna2dp.cat'),(Join-Path $pkg 'pna2dp.inf'))
        # This PnP driver is validated through its Microsoft-signed catalog. Do not
        # additionally require the first embedded SYS signature to pass /kp:
        # valid dual-signed drivers can have a publisher signature first and a
        # Microsoft signature second; SignTool /kp checks only the first one.
        # Both catalog membership checks above remain mandatory.
    }
    if ($WindowsMode) { Copy-Item -LiteralPath (Join-Path $Out 'pna2dp-setup.exe') -Destination (Join-Path $Out 'pna2dp-windows-setup.exe') -Force }
    Write-Host "installer embeds $n files (version $version)"
}

function Build-Dist {
    $dist = Join-Path $Out $(if ($WindowsMode) { 'dist-windows' } else { 'dist' })
    Remove-BuildDirectory $dist
    New-Item -ItemType Directory -Force $dist, (Join-Path $dist 'portable') | Out-Null
    if (!$WindowsMode) { New-Item -ItemType Directory -Force (Join-Path $dist 'portable\driver') | Out-Null }
    Copy-Item (Join-Path $Out 'pna2dp-setup.exe') $dist -Force
    $pkg = if ($DriverPackage) { $DriverPackage } else { Join-Path $Out 'package' }
    foreach ($name in $(if ($WindowsMode) { @() } else { @('pna2dp.inf','pna2dp.sys','pna2dp.cat','pna2dp-test.cer') })) {
        $file = Join-Path $pkg $name
        if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination (Join-Path $dist 'portable\driver') -Force }
    }
    Copy-Item -LiteralPath (Join-Path $Out 'pna2dp-setup.exe') -Destination (Join-Path $dist 'portable') -Force
    foreach ($f in $(if ($WindowsMode) { @('pna2dp-panel.exe','pna2dpctl.exe') } else { @('pna2dp-codechost.exe','pna2dp-panel.exe','pna2dpctl.exe','freeaptx.dll') })) { $p = Join-Path $Out $f; if (Test-Path $p) { Copy-Item $p (Join-Path $dist 'portable') -Force } }
    if (!$WindowsMode) { Get-ChildItem $Out -Filter 'pna2dp-codec-*.dll' | Copy-Item -Destination (Join-Path $dist 'portable') -Force }
    if (!$WindowsMode) {
        foreach ($item in @(Get-CodecRedistributionItems $Out)) {
            Copy-Item -LiteralPath $item.file -Destination (Join-Path $dist 'portable') -Force
        }
    }
    foreach ($f in @('LICENSE','THIRD-PARTY-NOTICES.md','README.md')) { $p = Join-Path $Root $f; if (Test-Path $p) { Copy-Item $p (Join-Path $dist 'portable') -Force } }
    Copy-Item -LiteralPath (Join-Path $Root 'docs\13-windows-mode.md') -Destination (Join-Path $dist 'portable') -Force
    Copy-Item -LiteralPath (Join-Path $Root 'docs\14-audio-diagnostics.md') -Destination (Join-Path $dist 'portable') -Force
    if (!$WindowsMode) { Copy-Item (Join-Path $Root 'build\install.ps1'), (Join-Path $Root 'build\update-device-driver.ps1'), (Join-Path $Root 'build\switch-device.ps1') (Join-Path $dist 'portable') -Force }
    $zip = Join-Path $dist 'pna2dp-portable.zip'
    # Entries are written one by one with '/' separators: Windows PowerShell's Compress-Archive and
    # .NET Framework's ZipFile.CreateFromDirectory store backslashes, which other unzippers mishandle.
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $portable = (Resolve-Path (Join-Path $dist 'portable')).Path
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $portable -Recurse -File) {
            $name = $f.FullName.Substring($portable.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "distribution in $dist"
}

switch ($Target) {
    'windows' { Build-Cli; Build-Gui; Build-Setup; Build-Dist }
    'driver'  { Build-Driver }
    'tests'   { Build-Tests }
    'security-tests' { Build-SecurityTests }
    'core-tests' { Build-CoreTests }
    'plugin-tests' { Build-UserExe 'plugin_test' @((Join-Path $Root 'codec-host\test\plugin_test.cpp')) @("/I$(Join-Path $Root 'codec-host\include')") @() @() }
    'quality-tests' { Build-QualityTests }
    'service-tests' { Build-ServiceTests }
    'process-tests' { Build-ProcessTests }
    'health-tests' { Build-HealthTests }
    'latency-tests' { Build-LatencyTests }
    'cli'     { Build-Cli }
    'host'    { Build-Host }
    'gui'     { Build-Gui }
    'plugins' { Build-Plugins }
    'setup'   { Build-Setup }
    'dist'    { Build-Setup; Build-Dist }
    'package' { Build-Tests; Build-Driver; Build-Cli; Build-Host; Build-Gui; Build-Plugins; Build-Setup; Build-Dist }
    'all'     { Build-Tests; Build-Driver; Build-Cli; Build-Host; Build-Gui }
}
Write-Host "done."
