# Downloads the Windows Driver Kit as a NuGet package (no administrator rights needed)
# and extracts the kernel headers, KMDF, libraries and tools to a short path.
#
#   .\build\get-wdk.ps1                      -> %USERPROFILE%\wdk\c
#   .\build\get-wdk.ps1 -Dest D:\wdk         -> D:\wdk\c
#
# The path must be short: NuGet layouts contain file names that exceed MAX_PATH when
# placed in a deep directory.
param(
    [string]$Dest = (Join-Path $env:USERPROFILE 'wdk'),
    [string]$Version = '10.0.26100.6584',
    [ValidateSet('x64','arm64')][string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Dest | Out-Null
$pkg = "microsoft.windows.wdk.$Arch"
$url = "https://api.nuget.org/v3-flatcontainer/$pkg/$Version/$pkg.$Version.nupkg"
$file = Join-Path $Dest "$pkg.$Version.nupkg"
if (-not (Test-Path $file)) {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $file
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($file)
$n = 0
foreach ($e in $zip.Entries) {
    if ($e.FullName.StartsWith('c/') -and $e.Name) {
        $target = Join-Path $Dest ($e.FullName -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        $n++
    }
}
$zip.Dispose()
Write-Host "Extracted $n files to $Dest\c"
Write-Host "Set PNA2DP_WDK_ROOT=$Dest\c if it is not the default location."
