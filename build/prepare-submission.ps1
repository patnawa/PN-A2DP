<#
.SYNOPSIS
  Prepares a CAB for Partner Center from an existing driver build. Does not
  upload, purchase a certificate, change Windows settings, or install anything.
  Without -CertificateThumbprint the CAB is explicitly marked unsigned.
#>
param(
    [string]$Package = (Join-Path $PSScriptRoot '..\out\Release\package'),
    [string]$Symbols = (Join-Path $PSScriptRoot '..\out\Release\pna2dp.pdb'),
    [string]$OutputDirectory = '',
    [string]$CertificateThumbprint = '',
    [switch]$MachineCertificateStore,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'toolchain.ps1')
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$symbolsPath = (Resolve-Path -LiteralPath $Symbols).Path
foreach ($name in @('pna2dp.inf','pna2dp.sys','pna2dp.cat')) {
    if (!(Test-Path -LiteralPath (Join-Path $packagePath $name) -PathType Leaf)) { throw "Missing $name" }
}
& $INFVERIF /w /v (Join-Path $packagePath 'pna2dp.inf')
if ($LASTEXITCODE) { throw 'INF verification failed.' }
$versionMatch = [regex]::Match((Get-Content (Join-Path $packagePath 'pna2dp.inf') -Raw), 'DriverVer\s*=\s*[^,]+,\s*([\d\.]+)')
if (!$versionMatch.Success) { throw 'DriverVer is missing.' }
$version = $versionMatch.Groups[1].Value
if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot ("..\out\submission-" + $version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) { throw 'Use a new output directory to preserve earlier submission artifacts.' }
New-Item -ItemType Directory -Path (Join-Path $outputPath 'PNA2DP') -Force | Out-Null
foreach ($name in @('pna2dp.inf','pna2dp.sys','pna2dp.cat')) {
    Copy-Item -LiteralPath (Join-Path $packagePath $name) -Destination (Join-Path $outputPath 'PNA2DP')
}
Copy-Item -LiteralPath $symbolsPath -Destination (Join-Path $outputPath 'PNA2DP\pna2dp.pdb')
$cabName = if ($CertificateThumbprint) { 'pna2dp-submission.cab' } else { 'pna2dp-submission-UNSIGNED.cab' }
$ddf = @(
    '.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=MSZIP',
    '.Set CabinetFileCountThreshold=0', '.Set FolderFileCountThreshold=0', '.Set FolderSizeThreshold=0',
    '.Set MaxCabinetSize=0', '.Set MaxDiskSize=0', '.Set DiskDirectoryTemplate=.',
    ".Set CabinetNameTemplate=$cabName", '.Set DestinationDir=PNA2DP',
    'PNA2DP\pna2dp.inf', 'PNA2DP\pna2dp.sys', 'PNA2DP\pna2dp.cat', 'PNA2DP\pna2dp.pdb'
)
[IO.File]::WriteAllLines((Join-Path $outputPath 'submission.ddf'), $ddf, [Text.Encoding]::ASCII)
Push-Location $outputPath
try {
    & "$env:SystemRoot\System32\makecab.exe" /f submission.ddf
    if ($LASTEXITCODE) { throw 'CAB creation failed.' }
} finally { Pop-Location }
$cabPath = Join-Path $outputPath $cabName
if ($CertificateThumbprint) {
    $signArgs = @('sign','/fd','SHA256','/sha1',$CertificateThumbprint,'/tr',$TimestampUrl,'/td','SHA256')
    if ($MachineCertificateStore) { $signArgs += '/sm' }
    & $SIGNTOOL @signArgs $cabPath
    if ($LASTEXITCODE) { throw 'Certificate signing failed. No upload occurred.' }
    & $SIGNTOOL verify /pa /v $cabPath
    if ($LASTEXITCODE) { throw 'CAB signature verification failed.' }
}
$files = @(Get-ChildItem -LiteralPath (Join-Path $outputPath 'PNA2DP') -File) + (Get-Item -LiteralPath $cabPath)
$manifest = [ordered]@{
    DriverVersion = $version
    Status = if ($CertificateThumbprint) { 'CAB signed; Partner Center submission and Microsoft driver signing still required' } else { 'UNSIGNED CAB; EV/account preparation and CAB signing still required' }
    HardwareCertification = 'Not performed by this script; use WHCP/HLK for a retail release.'
    Files = @($files | ForEach-Object { [ordered]@{ Name = $_.Name; SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash; Bytes = $_.Length } })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'submission-manifest.json') -Encoding utf8
Write-Host $manifest.Status
Write-Host "CAB: $cabPath"
Write-Host 'Follow docs/11-production-signing-th.md. Microsoft must return the signed driver package before normal-mode installation can be tested.'
