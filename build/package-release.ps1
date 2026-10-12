<# Stage existing, verified binaries for a GitHub prerelease. Does not rebuild or
   sign a driver, install anything, change trust/boot settings, or upload files.
   Build Windows mode, then full dist with -DriverPackage before running this. #>
param([ValidateSet('Release','Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
if ($Config -ne 'Release') { throw 'GitHub packages must use the Release configuration.' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root "out\$Config"
$versionText = Get-Content (Join-Path $root 'tools\pna2dp-panel\version.h') -Raw
$versionMatch = [regex]::Match($versionText, 'PN_PANEL_VERSION_TEXT\s+"([0-9.]+)"')
if (!$versionMatch.Success) { throw 'Application version is missing.' }
$version = $versionMatch.Groups[1].Value
$driverMatch = [regex]::Match((Get-Content (Join-Path $bin 'dist\portable\driver\pna2dp.inf') -Raw), '(?m)^DriverVer\s*=\s*[^,]+,([0-9.]+)')
if (!$driverMatch.Success) { throw 'Driver version is missing.' }
$guidePath = Join-Path $root 'docs\17-test-mode-release-guide.md'
if (!(Get-Content -LiteralPath $guidePath -Raw).Contains("PN-A2DP $version")) { throw 'Release guide version is stale.' }
foreach ($kind in @('dist','dist-windows')) {
    foreach ($file in @('pna2dp-setup.exe','portable\pna2dp-panel.exe')) {
        if ((Get-Item -LiteralPath (Join-Path $bin "$kind\$file")).VersionInfo.ProductVersion -ne $version) {
            throw "Stale version in $kind/$file"
        }
    }
}
$certPath = Join-Path $bin 'dist\portable\driver\pna2dp-test.cer'
$cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certPath)
if ($cert.HasPrivateKey -or $cert.NotAfter -le (Get-Date) -or $cert.NotBefore -gt (Get-Date)) { throw 'Invalid public development certificate.' }
if (!(Get-Content -LiteralPath $guidePath -Raw).Contains($cert.Thumbprint)) { throw 'Guide certificate identifier does not match the package.' }
foreach ($file in @('pna2dp.sys','pna2dp.cat')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $bin "dist\portable\driver\$file")
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
        throw "Invalid or unexpected development signature: $file. Use a trusted packaging/test machine."
    }
}
& (Join-Path $PSScriptRoot 'test-distribution-compliance.ps1') -Folder (Join-Path $bin 'dist\portable')
if ($LASTEXITCODE) { throw 'Codec redistribution verification failed.' }

$stage = Join-Path $root ('out\releases\v' + $version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $stage) { throw "Staging folder already exists: $stage" }
New-Item -ItemType Directory -Path $stage | Out-Null
$mapping = [ordered]@{
    "PN-A2DP-$version-windows-setup.exe" = 'dist-windows\pna2dp-setup.exe'
    "PN-A2DP-$version-windows-portable.zip" = 'dist-windows\pna2dp-portable.zip'
    "PN-A2DP-$version-testmode-setup.exe" = 'dist\pna2dp-setup.exe'
    "PN-A2DP-$version-testmode-portable.zip" = 'dist\pna2dp-portable.zip'
    'PN-A2DP-Test-Certificate.cer' = 'dist\portable\driver\pna2dp-test.cer'
}
foreach ($name in $mapping.Keys) {
    Copy-Item -LiteralPath (Join-Path $bin $mapping[$name]) -Destination (Join-Path $stage $name)
}
Copy-Item -LiteralPath $guidePath -Destination (Join-Path $stage 'TEST-MODE-GUIDE.md')

# Inspect actual ZIP entries before upload; never publish build symbols, private
# keys, local diagnostics or an unexpected path through a directory-wide glob.
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($kind in @('windows','testmode')) {
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $stage "PN-A2DP-$version-$kind-portable.zip"))
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName })
        foreach ($name in $names) {
            if ($name -match '(^/|\\|(^|/)\.\.(/|$)|:|\.(pfx|p12|key|pem|pdb|log)$|(^|/)\.git(/|$))') { throw "Unexpected release ZIP entry: $name" }
        }
        if ($kind -eq 'windows' -and @($names | Where-Object { $_ -match '\.(sys|inf|cat|cer|dll)$|pna2dp-codechost' }).Count) {
            throw 'Windows-mode archive contains driver/codec components.'
        }
        $folder = Join-Path $bin $(if ($kind -eq 'windows') { 'dist-windows\portable' } else { 'dist\portable' })
        $expected = @(Get-ChildItem -LiteralPath $folder -Recurse -File)
        if ($names.Count -ne $expected.Count) { throw "Incomplete $kind portable archive." }
        foreach ($file in $expected) {
            $relative = $file.FullName.Substring($folder.Length+1).Replace('\','/')
            $entries = @($zip.Entries | Where-Object { $_.FullName -ceq $relative })
            if ($entries.Count -ne 1) { throw "Missing/duplicate ZIP entry: $relative" }
            $hash = [Security.Cryptography.SHA256]::Create(); $stream = $entries[0].Open()
            try { $digest = [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
            finally { $stream.Dispose(); $hash.Dispose() }
            if ($digest -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) { throw "ZIP content mismatch: $relative" }
        }
    } finally { $zip.Dispose() }
}
$revision = (git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Cannot read source revision.' }
[ordered]@{
    schema=1; application=$version; driver=$driverMatch.Groups[1].Value; architecture='x64'
    sourceRevision=$revision; experimental=$true; driverSigning='self-signed development certificate; Test Mode required'
    certificateThumbprint=$cert.Thumbprint; certificateHasPrivateKey=$false
    assets=@(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
        [ordered]@{name=$_.Name; bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'release-manifest.json') -Encoding UTF8
$sums = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name
})
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $sums, [Text.UTF8Encoding]::new($false))
Write-Host "Release assets verified and staged: $stage"
