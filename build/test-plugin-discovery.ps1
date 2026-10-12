<# Exercises real host discovery in a disposable directory, without registering a
   service or opening the installed audio device. Uses only a built AAC plugin. #>
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\out\Release'))
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fixture = Join-Path $root ('out\verification\plugin-discovery-' + [guid]::NewGuid().ToString('N'))
$app = Join-Path $fixture 'app'
$data = Join-Path $fixture 'data'
$cwd = Join-Path $fixture 'cwd'
$dataPlugins = Join-Path $data 'PN-A2DP\plugins'
New-Item -ItemType Directory -Force $app,$dataPlugins,$cwd,(Join-Path $app 'plugins') | Out-Null
Copy-Item -LiteralPath (Join-Path $OutputDirectory 'pna2dp-codechost.exe') -Destination $app
$plugin = Join-Path $OutputDirectory 'pna2dp-codec-aac.dll'
Copy-Item -LiteralPath $plugin -Destination (Join-Path $dataPlugins 'pna2dp-codec-untrusted-data.dll')
Copy-Item -LiteralPath $plugin -Destination (Join-Path $cwd 'pna2dp-codec-untrusted-cwd.dll')
Copy-Item -LiteralPath $plugin -Destination (Join-Path $app 'pna2dp-codec-local.dll')
Copy-Item -LiteralPath $plugin -Destination (Join-Path $app 'plugins\pna2dp-codec-local-subdir.dll')
$oldData = $env:ProgramData
try {
    $env:ProgramData = $data
    Push-Location -LiteralPath $cwd
    try { $result = & (Join-Path $app 'pna2dp-codechost.exe') --list-plugins 2>&1 | Out-String; $code = $LASTEXITCODE }
    finally { Pop-Location }
} finally { $env:ProgramData = $oldData }
Write-Host $result
if ($code -ne 0) { throw "Host discovery failed: $code" }
if ($result -match 'untrusted-data|untrusted-cwd') { throw 'Host loaded a plugin from ProgramData or the current directory.' }
foreach ($name in @('pna2dp-codec-local.dll','pna2dp-codec-local-subdir.dll')) {
    if (!$result.Contains("from $name,")) { throw "Expected installed plugin was not loaded: $name" }
}
if ($result -notmatch 'built-in, codecs') { throw 'Built-in SBC was not discovered.' }
Write-Host 'PASS discovery: installed plugins and built-in SBC found; ProgramData and working directory ignored.'
