<# Negative package regressions against the production release gate. Only copies
   under out are modified; no installed binary or source file is changed. #>
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\out\Release'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'codec-distribution.ps1')
$source = (Resolve-Path -LiteralPath $OutputDirectory).Path
$fixture = Join-Path $CodecDistributionRoot ('out\verification\codec-guards-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$count = 0
foreach ($codec in @('aac','aptx','ldac')) {
    $spec = Get-CodecDistributionSpec $codec
    $folder = Join-Path $fixture $codec
    New-Item -ItemType Directory -Path $folder | Out-Null
    $names = @($spec.Binaries) + @($spec.Notices.Keys) + @("sources-$codec.zip","sources-$codec.json")
    foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $folder }
    $items = @(Get-CodecRedistributionItems $folder)
    if ($items.Count -ne $spec.Notices.Count + 2) { throw "Incomplete $codec payload selection." }
    foreach ($name in @($spec.Notices.Keys) + @("sources-$codec.zip","sources-$codec.json")) {
        $path = Join-Path $folder $name
        Remove-Item -LiteralPath $path
        $rejected = $false
        try { $null = @(Get-CodecRedistributionItems $folder) } catch { $rejected = $true }
        finally { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $path }
        if (!$rejected) { throw "Package accepted missing $name" }
        $count++
    }
    foreach ($name in $spec.Binaries) {
        $path = Join-Path $folder $name
        [IO.File]::AppendAllText($path, 'stale-binary-fixture')
        $rejected = $false
        try { $null = @(Get-CodecRedistributionItems $folder) } catch { $rejected = $true }
        finally { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $path -Force }
        if (!$rejected) { throw "Package accepted changed $name" }
        $count++
    }
    Write-Host "PASS ${codec}: complete payload accepted; missing and stale artifacts rejected."
}
Write-Host "PASS $count negative package cases."
