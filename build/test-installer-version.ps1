<# Read-only release metadata regression: app/setup version is independent of the kernel version. #>
param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\out\Release\pna2dp-setup.exe'),
    [string]$Panel = (Join-Path $PSScriptRoot '..\out\Release\pna2dp-panel.exe'),
    [switch]$Installed
)
$ErrorActionPreference = 'Stop'
$panelInfo = (Get-Item -LiteralPath $Panel).VersionInfo
$setupInfo = (Get-Item -LiteralPath $Setup).VersionInfo
$expected = $panelInfo.ProductVersion
if ($expected -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Panel release version is unavailable.' }
$values = [ordered]@{
    'setup product string' = $setupInfo.ProductVersion
    'setup file string' = $setupInfo.FileVersion
    'setup numeric product' = "$($setupInfo.ProductMajorPart).$($setupInfo.ProductMinorPart).$($setupInfo.ProductBuildPart).$($setupInfo.ProductPrivatePart)"
    'setup numeric file' = "$($setupInfo.FileMajorPart).$($setupInfo.FileMinorPart).$($setupInfo.FileBuildPart).$($setupInfo.FilePrivatePart)"
}
if ($Installed) {
    $values['Apps & features version'] = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PN-A2DP').DisplayVersion
    $values['installation registration version'] = (Get-ItemProperty 'HKLM:\SOFTWARE\PN-A2DP').Version
}
$failures = 0
foreach ($entry in $values.GetEnumerator()) {
    if ($entry.Value -ne $expected) {
        $failures++
        Write-Host "FAIL $($entry.Key): $($entry.Value); expected app release $expected"
    } else { Write-Host "PASS $($entry.Key): $expected" }
}
if ($failures) { throw "$failures installer version checks failed." }
