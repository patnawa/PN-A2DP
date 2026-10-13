# Builds pna2dp-codec-ldac.dll from third_party/ldacBT (Apache-2.0) + ldac_plugin.c
#
# libldac is a single-translation-unit library: ldaclib.c #includes every other
# encoder source, ldacBT.c #includes the Bluetooth wrapper. Only those two files are
# compiled: bitrate adaptation is the driver's (EQMID through reconfigure), so
# libldac's ABR helper (ldacBT_abr.c) is not linked.
param([ValidateSet('Release','Debug')][string]$Config = 'Release', [switch]$FixedPoint, [switch]$SkipSourceBundle)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $Root 'build\toolchain.ps1')
$ldac = Join-Path $Root 'third_party\ldacBT'
if (-not (Test-Path (Join-Path $ldac 'libldac\inc\ldacBT.h'))) { throw "run: git submodule update --init --recursive" }
$out = Join-Path $Root "out\$Config"
$obj = Join-Path $out 'obj\ldac'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$incs = @("/I$PSScriptRoot", "/I$ldac\libldac\inc", "/I$ldac\libldac\src",
          "/I$(Join-Path $Root 'codec-host\include')", "/I$(Join-Path $Root 'driver\pna2dp\inc')")
$incs += ($UmIncludes | ForEach-Object { "/I$_" })
$opt = if ($Config -eq 'Debug') { @('/Od','/MTd') } else { @('/O2','/MT','/DNDEBUG','/fp:fast') }
if ($FixedPoint) { $opt += '/D_32BIT_FIXED_POINT' }
$flags = @('/nologo','/c','/W3','/Zi','/D_CRT_SECURE_NO_WARNINGS','/DWIN32_LEAN_AND_MEAN',"/FI$(Join-Path $PSScriptRoot 'msvc_compat.h')",'/TC','/wd4244','/wd4267','/wd4305') + $opt + $incs

$srcs = @(
    (Join-Path $ldac 'libldac\src\ldaclib.c'),
    (Join-Path $ldac 'libldac\src\ldacBT.c'),
    (Join-Path $PSScriptRoot 'ldac_plugin.c')
)
$objs = @()
foreach ($s in $srcs) {
    $f = Get-Item $s
    $o = Join-Path $obj ($f.BaseName + '.obj')
    & $CL @flags "/Fo$o" $f.FullName
    if ($LASTEXITCODE) { throw "cl failed: $($f.Name)" }
    $objs += $o
}
$libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }
& $LINK /nologo /DLL /DEBUG "/OUT:$out\pna2dp-codec-ldac.dll" "/PDB:$out\pna2dp-codec-ldac.pdb" @libs @objs
if ($LASTEXITCODE) { throw "link failed" }
Write-Host "built $out\pna2dp-codec-ldac.dll"
if (!$SkipSourceBundle) {
    . (Join-Path $Root 'build\codec-distribution.ps1')
    New-CodecSourceBundle -Codec ldac -OutputDirectory $out -BuildConfig $Config -BuildOptions $(if ($FixedPoint) { '-FixedPoint' } else { '' })
}
