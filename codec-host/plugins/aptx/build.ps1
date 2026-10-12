# Builds freeaptx.dll (LGPL-2.1+, kept as its own DLL) and pna2dp-codec-aptx.dll (MIT adapter)
param([ValidateSet('Release','Debug')][string]$Config = 'Release', [switch]$SkipSourceBundle)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $Root 'build\toolchain.ps1')
$src = Join-Path $Root 'third_party\libfreeaptx'
if (-not (Test-Path (Join-Path $src 'freeaptx.h'))) { throw "run: git submodule update --init --recursive" }
$out = Join-Path $Root "out\$Config"; $obj = Join-Path $out 'obj\aptx'
New-Item -ItemType Directory -Force $out, $obj | Out-Null
$incs = @("/I$src", "/I$(Join-Path $Root 'codec-host\include')", "/I$(Join-Path $Root 'driver\pna2dp\inc')") + ($UmIncludes | ForEach-Object { "/I$_" })
$opt = if ($Config -eq 'Debug') { @('/Od','/MTd') } else { @('/O2','/MT','/DNDEBUG') }
$libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }

# 1. freeaptx.dll: export the public API
$def = Join-Path $obj 'freeaptx.def'
@"
LIBRARY freeaptx
EXPORTS
    aptx_init
    aptx_finish
    aptx_reset
    aptx_encode
    aptx_encode_finish
    aptx_decode
    aptx_decode_sync
    aptx_decode_sync_finish
"@ | Set-Content $def -Encoding ascii
$lobjs = @()
foreach ($f in @('freeaptx.c')) {
    $o = Join-Path $obj ($f -replace '\.c$', '.obj')
    & $CL /nologo /c /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /TC @opt @incs "/Fo$o" (Join-Path $src $f); if ($LASTEXITCODE) { throw "cl failed: $f" }
    $lobjs += $o
}
& $LINK /nologo /DLL /DEBUG "/DEF:$def" "/OUT:$out\freeaptx.dll" "/IMPLIB:$obj\freeaptx.lib" "/PDB:$out\freeaptx.pdb" @libs @lobjs
if ($LASTEXITCODE) { throw "link freeaptx failed" }

# 2. adapter
$o = Join-Path $obj 'aptx_plugin.obj'
& $CL /nologo /c /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /TC @opt @incs "/Fo$o" (Join-Path $PSScriptRoot 'aptx_plugin.c'); if ($LASTEXITCODE) { throw "cl failed: aptx_plugin.c" }
& $LINK /nologo /DLL /DEBUG "/OUT:$out\pna2dp-codec-aptx.dll" "/PDB:$out\pna2dp-codec-aptx.pdb" @libs $o "$obj\freeaptx.lib"
if ($LASTEXITCODE) { throw "link plugin failed" }
Write-Host "built $out\pna2dp-codec-aptx.dll (+ freeaptx.dll)"
if (!$SkipSourceBundle) {
    . (Join-Path $Root 'build\codec-distribution.ps1')
    New-CodecSourceBundle -Codec aptx -OutputDirectory $out -BuildConfig $Config
}
