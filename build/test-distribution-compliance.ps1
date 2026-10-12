<# Checks redistribution artifacts in an extracted installer or portable folder.
   This verifies packaging, not patent clearance, certification or legal approval. #>
param([string]$Setup = '', [string]$Folder = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ((!$Setup -and !$Folder) -or ($Setup -and $Folder)) { throw 'Specify exactly one of -Setup or -Folder.' }
if ($Setup) {
    $Folder = Join-Path $root ('out\verification\redistribution-' + [guid]::NewGuid().ToString('N'))
    $process = Start-Process -FilePath (Resolve-Path -LiteralPath $Setup).Path -ArgumentList @('--extract', ('"' + $Folder + '"')) -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(30000)) { throw 'Installer extraction timed out.' }
    if ($process.ExitCode -ne 0) { throw "Installer extraction failed: $($process.ExitCode)" }
}
$Folder = (Resolve-Path -LiteralPath $Folder).Path
Add-Type -AssemblyName System.IO.Compression.FileSystem
$specs = @(
    @{Codec='aac'; Binaries=@('pna2dp-codec-aac.dll'); Licenses=@(@{Name='LICENSE-FDK-AAC.txt'; Source='third_party\fdk-aac\NOTICE'}); Member='third_party/fdk-aac/NOTICE'},
    @{Codec='aptx'; Binaries=@('pna2dp-codec-aptx.dll','freeaptx.dll'); Licenses=@(@{Name='LICENSE-LGPL-2.1.txt'; Source='third_party\libfreeaptx\COPYING'}); Member='third_party/libfreeaptx/freeaptx.c'},
    @{Codec='ldac'; Binaries=@('pna2dp-codec-ldac.dll'); Licenses=@(@{Name='LICENSE-LDAC.txt'; Source='third_party\ldacBT\libldac\LICENSE'},@{Name='NOTICE-LDAC.txt'; Source='third_party\ldacBT\libldac\NOTICE'}); Member='third_party/ldacBT/libldac/src/ldaclib.c'}
)
$failures = [Collections.Generic.List[string]]::new()
$checked = 0
foreach ($spec in $specs) {
    $present = @($spec.Binaries | Where-Object { Test-Path -LiteralPath (Join-Path $Folder $_) -PathType Leaf })
    if (!$present.Count) { continue }
    $checked++
    try {
        foreach ($binary in $spec.Binaries) {
            if (!(Test-Path -LiteralPath (Join-Path $Folder $binary) -PathType Leaf)) { throw "Missing codec dependency: $binary" }
        }
        foreach ($license in $spec.Licenses) {
            $path = Join-Path $Folder $license.Name
            if (!(Test-Path -LiteralPath $path)) { throw "Missing full license/notice: $($license.Name)" }
            if ((Get-FileHash -LiteralPath $path).Hash -ne (Get-FileHash -LiteralPath (Join-Path $root $license.Source)).Hash) { throw "License/notice modified: $($license.Name)" }
        }
        $manifest = Get-Content -LiteralPath (Join-Path $Folder ("sources-$($spec.Codec).json")) -Raw | ConvertFrom-Json
        if ($manifest.schema -ne 1 -or $manifest.codec -ne $spec.Codec) { throw 'Invalid source manifest.' }
        $required = @($spec.Binaries) + @($spec.Licenses | ForEach-Object { $_.Name }) + @("sources-$($spec.Codec).zip")
        foreach ($name in $required) {
            $entry = @($manifest.artifacts | Where-Object { $_.file -eq $name })
            if ($entry.Count -ne 1 -or $entry[0].sha256 -ne (Get-FileHash -LiteralPath (Join-Path $Folder $name)).Hash) { throw "Missing or mismatched artifact hash: $name" }
        }
        $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $Folder ("sources-$($spec.Codec).zip")))
        try {
            foreach ($member in @($spec.Member,'LICENSE','build/toolchain.ps1',"codec-host/plugins/$($spec.Codec)/build.ps1",'BUILD-SOURCES.txt','source-manifest.json')) {
                if (!$zip.GetEntry($member)) { throw "Corresponding source archive lacks $member" }
            }
            if (@($zip.Entries | Where-Object { $_.FullName -match '(^|/)\.git(/|$)|(^|/)\.\.(\/|$)|^[A-Za-z]:|^/' }).Count) { throw 'Unsafe or private Git metadata in source archive.' }
            $reader = [IO.StreamReader]::new($zip.GetEntry('source-manifest.json').Open())
            try { $sourceManifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            foreach ($file in $sourceManifest.files) {
                $entry = $zip.GetEntry($file.path)
                if (!$entry) { throw "Source archive lacks $($file.path)" }
                $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
                try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
                finally { $stream.Dispose(); $sha.Dispose() }
                if ($hash -ne $file.sha256) { throw "Source archive hash mismatch: $($file.path)" }
            }
        } finally { $zip.Dispose() }
        Write-Host "PASS $($spec.Codec): full notices, matching binary hashes and offline source/build instructions."
    } catch { $failures.Add("$($spec.Codec): $($_.Exception.Message)"); Write-Host "FAIL $($failures[-1])" }
}
if (!$checked) { Write-Host 'PASS application-only/SBC package: no external codec binaries.' }
Write-Host "Package: $Folder"
if ($failures.Count) { throw "$($failures.Count) codec redistribution checks failed." }
