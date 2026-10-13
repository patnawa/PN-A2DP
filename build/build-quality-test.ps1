<#
.SYNOPSIS
  Builds the end-to-end audio quality harness (tools\audio-test\quality_test.cpp) against the
  production sources and, with -Run, runs it through build\test-audio-quality.py.

.DESCRIPTION
  Standalone: does not invoke build.ps1 and writes only into -OutDir. Compiles with the MSVC
  toolset that build\toolchain.ps1 locates (same flags as build.ps1's Build-UserExe):
    production: driver\pna2dp\src\pcm.cpp, a2dp_codec.cpp, rtp.cpp, common\sbc\sbc.c,
                codec-host\plugins\sbc\sbc_plugin.cpp (built-in SBC plugin)
    harness   : tools\audio-test\quality_test.cpp
  The AAC / aptX / LDAC plugins are loaded at run time from -PluginDir (default out\Release),
  exactly as codec-host\test\plugin_test.cpp loads them. Needs ffmpeg.exe on PATH (or -Ffmpeg)
  with the sbc, aac (LOAS), aptx and aptx_hd decoders. Nothing is installed; no device is used.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File build\build-quality-test.ps1 -OutDir $env:TEMP\pnq -Run
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$PluginDir = '',
    [string]$Ffmpeg = '',
    [string]$Filter = '',
    [int]$Threads = 0,
    [switch]$DebugBuild,
    [switch]$Run,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'toolchain.ps1')
if (-not $PluginDir) { $PluginDir = Join-Path $Root 'out\Release' }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$obj = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

function Invoke-Checked([string]$exe, [string[]]$toolArgs) {
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed with exit code $LASTEXITCODE" }
}

$opt = if ($DebugBuild) { @('/Od', '/MTd', '/D_DEBUG') } else { @('/O2', '/MT', '/DNDEBUG') }
$common = @('/nologo', '/c', '/W4', '/WX-', '/Zi', '/EHsc', '/std:c++17', '/permissive-', '/DUNICODE', '/D_UNICODE',
            '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS', "/Fd$obj\quality_test.pdb") + $opt
$incs = @("/I$(Join-Path $Root 'driver\pna2dp\inc')", "/I$(Join-Path $Root 'common')", "/I$(Join-Path $Root 'codec-host\include')") +
        ($UmIncludes | ForEach-Object { "/I$_" })
$sources = @(
    (Join-Path $Root 'tools\audio-test\quality_test.cpp'),
    (Join-Path $Root 'driver\pna2dp\src\pcm.cpp'),
    (Join-Path $Root 'driver\pna2dp\src\a2dp_codec.cpp'),
    (Join-Path $Root 'driver\pna2dp\src\rtp.cpp'),
    (Join-Path $Root 'common\sbc\sbc.c'),
    (Join-Path $Root 'codec-host\plugins\sbc\sbc_plugin.cpp')
)
$objs = @()
foreach ($s in $sources) {
    $f = Get-Item $s
    $o = Join-Path $obj ($f.BaseName + '.obj')
    $c = $common
    if ($f.Extension -eq '.c') { $c = @($common | Where-Object { $_ -notmatch '^/std:|^/permissive|^/EHsc' }) + '/TC' }
    Invoke-Checked $CL ($c + $incs + @("/Fo$o", $f.FullName))
    $objs += $o
}
$exe = Join-Path $OutDir 'quality_test.exe'
$libs = $UmLibDirs | ForEach-Object { "/LIBPATH:$_" }
Invoke-Checked $LINK (@('/nologo', '/DEBUG', '/INCREMENTAL:NO', '/SUBSYSTEM:CONSOLE', "/OUT:$exe", "/PDB:$OutDir\quality_test.pdb") + $libs + $objs)
Write-Host "built $exe"

if ($Run) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command py -ErrorAction Stop }
    $pyArgs = @('-I', (Join-Path $PSScriptRoot 'test-audio-quality.py'), '--exe', $exe, '--plugins', $PluginDir, '--work', (Join-Path $OutDir 'work'))
    if ($Ffmpeg) { $pyArgs += @('--ffmpeg', $Ffmpeg) }
    if ($Filter) { $pyArgs += @('--filter', $Filter) }
    if ($Threads) { $pyArgs += @('--threads', "$Threads") }
    if ($Keep) { $pyArgs += '--keep' }
    & $py.Source @pyArgs
    exit $LASTEXITCODE
}
