# Builds pna2dp-codec-aac.dll from third_party/fdk-aac (encoder libraries only) + aac_plugin.cpp
param([ValidateSet('Release','Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $Root 'build\toolchain.ps1')
$fdk = Join-Path $Root 'third_party\fdk-aac'
if (Test-Path (Join-Path $fdk 'cpp\libAACenc\include\aacenc_lib.h')) { $fdk = Join-Path $fdk 'cpp' }   # upstream moved the C++ tree
if (-not (Test-Path (Join-Path $fdk 'libAACenc\include\aacenc_lib.h'))) { throw "run: git submodule update --init --recursive" }
$out = Join-Path $Root "out\$Config"
$obj = Join-Path $out 'obj\aac'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

# encoder needs these libraries (plus a few decoder headers for shared declarations)
$encLibs = @('libAACenc','libFDK','libSYS','libMpegTPEnc','libSBRenc','libArithCoding','libDRCdec','libSACenc','libPCMutils','libMpegTPDec','libSACdec')
$incs = @("/I$(Join-Path $Root 'codec-host\include')", "/I$(Join-Path $Root 'driver\pna2dp\inc')")
foreach ($d in $encLibs + @('libSBRdec','libAACdec')) {
    if (Test-Path "$fdk\$d\include") { $incs += "/I$fdk\$d\include" }
    if (Test-Path "$fdk\$d\src") { $incs += "/I$fdk\$d\src" }
}
$incs += ($UmIncludes | ForEach-Object { "/I$_" })
$opt = if ($Config -eq 'Debug') { @('/Od','/MTd') } else { @('/O2','/MT','/DNDEBUG') }
$flags = @('/nologo','/c','/W1','/Zi','/EHsc','/D_CRT_SECURE_NO_WARNINGS','/DWIN32_LEAN_AND_MEAN','/wd4244','/wd4267','/wd4305','/wd4996') + $opt + $incs

$srcs = @()
foreach ($d in $encLibs) {
    if (Test-Path "$fdk\$d\src") { $srcs += Get-ChildItem -Path "$fdk\$d\src" -Filter '*.cpp' }
}
$srcs += Get-Item (Join-Path $PSScriptRoot 'aac_plugin.cpp')
$objs = @()
foreach ($s in $srcs) {
    $o = Join-Path $obj ($s.Directory.Parent.Name + '_' + $s.BaseName + '.obj')
    & $CL @flags "/Fo$o" $s.FullName
    if ($LASTEXITCODE) { throw "cl failed: $($s.FullName)" }
    $objs += $o
}
$libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }
& $LINK /nologo /DLL /DEBUG "/OUT:$out\pna2dp-codec-aac.dll" "/PDB:$out\pna2dp-codec-aac.pdb" @libs @objs
if ($LASTEXITCODE) { throw "link failed" }
Write-Host "built $out\pna2dp-codec-aac.dll"
