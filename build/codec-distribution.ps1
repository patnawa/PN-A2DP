# Redistribution artifacts are generated after a codec build, then hash-bound to
# its binaries. Packaging refuses missing/stale bundles instead of silently
# distributing a DLL without its license and corresponding source.
$CodecDistributionRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-CodecDistributionSpec([string]$Codec) {
    switch ($Codec) {
        'aac' { return @{ Library='third_party\fdk-aac'; Binaries=@('pna2dp-codec-aac.dll'); Notices=@{ 'LICENSE-FDK-AAC.txt'='third_party\fdk-aac\NOTICE' } } }
        'aptx' { return @{ Library='third_party\libfreeaptx'; Binaries=@('pna2dp-codec-aptx.dll','freeaptx.dll'); Notices=@{ 'LICENSE-LGPL-2.1.txt'='third_party\libfreeaptx\COPYING' } } }
        'ldac' { return @{ Library='third_party\ldacBT'; Binaries=@('pna2dp-codec-ldac.dll'); Notices=@{ 'LICENSE-LDAC.txt'='third_party\ldacBT\libldac\LICENSE'; 'NOTICE-LDAC.txt'='third_party\ldacBT\libldac\NOTICE' } } }
        default { throw "Unknown codec: $Codec" }
    }
}

function Get-CodecSourceFiles([string]$Directory) {
    # Read the current working files, including local modifications. Recurse into
    # Git submodules (LDAC has one), without copying Git metadata or ignored output.
    $paths = @(git -C $Directory -c core.quotepath=false ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE) { throw "Cannot enumerate source under $Directory" }
    foreach ($relative in $paths | Select-Object -Unique) {
        $path = [IO.Path]::GetFullPath((Join-Path $Directory $relative))
        if (!$path.StartsWith($Directory.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source path escapes its directory.' }
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source reparse point is unsupported: $relative" }
        if ($item.PSIsContainer) { Get-CodecSourceFiles $path } else { $path }
    }
}

function New-CodecSourceBundle {
    param([ValidateSet('aac','aptx','ldac')][string]$Codec, [string]$OutputDirectory,
        [ValidateSet('Release','Debug')][string]$BuildConfig, [string]$BuildOptions = '')
    $output = [IO.Path]::GetFullPath($OutputDirectory)
    $allowed = (Join-Path $CodecDistributionRoot 'out').TrimEnd('\') + '\'
    if (!$output.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Source bundles must be written under out.' }
    $spec = Get-CodecDistributionSpec $Codec
    foreach ($binary in $spec.Binaries) {
        if (!(Test-Path -LiteralPath (Join-Path $output $binary) -PathType Leaf)) { throw "Missing built codec: $binary" }
    }
    $files = @(Get-CodecSourceFiles (Join-Path $CodecDistributionRoot $spec.Library))
    foreach ($directory in @("codec-host\plugins\$Codec",'codec-host\include','driver\pna2dp\inc')) {
        $files += @(Get-ChildItem -LiteralPath (Join-Path $CodecDistributionRoot $directory) -File -Recurse | ForEach-Object { $_.FullName })
    }
    $files += @('LICENSE','build\toolchain.ps1','build\codec-distribution.ps1') | ForEach-Object { Join-Path $CodecDistributionRoot $_ }
    $sourceManifest = @($files | Sort-Object -Unique | ForEach-Object {
        [pscustomobject]@{path=$_.Substring($CodecDistributionRoot.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_).Hash}
    })
    $revision = (git -C (Join-Path $CodecDistributionRoot $spec.Library) rev-parse HEAD).Trim()
    if ($LASTEXITCODE) { throw 'Cannot read upstream revision.' }
    $archivePath = Join-Path $output "sources-$Codec.zip"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
    $archive = [IO.Compression.ZipFile]::Open($archivePath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $sourceManifest) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,
                (Join-Path $CodecDistributionRoot $file.path), $file.path, [IO.Compression.CompressionLevel]::Optimal)
        }
        $texts = @{
            'source-manifest.json' = ([ordered]@{schema=1;codec=$Codec;upstreamRevision=$revision;config=$BuildConfig;options=$BuildOptions;files=$sourceManifest} | ConvertTo-Json -Depth 5)
            'BUILD-SOURCES.txt' = @"
Corresponding sources for the PN-A2DP $Codec plugin.
This archive contains the library, PN adapter, headers, notices and build scripts.
It includes the actual working source files at build time, not just a remote URL.
File hashes are in source-manifest.json; the outer sources-$Codec.json binds this
archive to the distributed DLLs. Git metadata and private machine paths are omitted.

Build on Windows x64 with Visual Studio 2022 C++ Build Tools, Windows SDK and WDK
10.0.26100, and PowerShell. See build/toolchain.ps1 for tool discovery/overrides.
Extract to a directory and run from its root:
  .\codec-host\plugins\$Codec\build.ps1 -Config $BuildConfig $BuildOptions -SkipSourceBundle
SkipSourceBundle builds the DLLs without requiring the original Git repositories.
For aptX, freeaptx.dll remains separate and replaceable with a compatible build.
No publisher-signature restriction is imposed on that DLL by PN-A2DP.

Each component retains its own license. These sources do not grant patent or
trademark rights or certify product interoperability. No codec-license fee is
charged for this source archive. Optional commercial terms must preserve the
third-party rights, including applicable modification/reverse-engineering rights.
"@
        }
        foreach ($name in $texts.Keys) {
            $writer = [IO.StreamWriter]::new($archive.CreateEntry($name).Open(), [Text.UTF8Encoding]::new($false))
            try { $writer.Write($texts[$name]) } finally { $writer.Dispose() }
        }
    } finally { $archive.Dispose() }
    foreach ($name in $spec.Notices.Keys) {
        Copy-Item -LiteralPath (Join-Path $CodecDistributionRoot $spec.Notices[$name]) -Destination (Join-Path $output $name) -Force
    }
    $artifacts = @(@($spec.Binaries) + @($spec.Notices.Keys) + @("sources-$Codec.zip") | ForEach-Object {
        [pscustomobject]@{file=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $output $_)).Hash}
    })
    [ordered]@{schema=1;codec=$Codec;upstreamRevision=$revision;artifacts=$artifacts} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output "sources-$Codec.json") -Encoding UTF8
    Write-Host "Prepared $Codec notices and corresponding source archive."
}

function Get-CodecRedistributionItems([string]$OutputDirectory) {
    foreach ($codec in @('aac','aptx','ldac')) {
        $spec = Get-CodecDistributionSpec $codec
        $present = @($spec.Binaries | Where-Object { Test-Path -LiteralPath (Join-Path $OutputDirectory $_) -PathType Leaf })
        if (!$present.Count) { continue }
        $manifestPath = Join-Path $OutputDirectory "sources-$codec.json"
        if (!(Test-Path -LiteralPath $manifestPath)) { throw "Missing $codec redistribution bundle. Rebuild its plugin before packaging." }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($manifest.schema -ne 1 -or $manifest.codec -ne $codec) { throw "Invalid $codec source manifest." }
        foreach ($name in @($spec.Binaries) + @($spec.Notices.Keys) + @("sources-$codec.zip")) {
            $path = Join-Path $OutputDirectory $name
            $entries = @($manifest.artifacts | Where-Object { $_.file -eq $name })
            if (!(Test-Path -LiteralPath $path -PathType Leaf) -or $entries.Count -ne 1 -or
                $entries[0].sha256 -ne (Get-FileHash -LiteralPath $path).Hash) {
                throw "Missing or stale $codec redistribution artifact: $name. Rebuild the plugin from the release source."
            }
        }
        foreach ($name in @($spec.Notices.Keys) + @("sources-$codec.zip","sources-$codec.json")) {
            @{file=(Join-Path $OutputDirectory $name);sub='';comp=$codec;required=$true}
        }
    }
}
