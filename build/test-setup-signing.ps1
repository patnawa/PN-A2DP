<# Read-only installer policy tests. Fixtures stay under a new verification folder. #>
param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\out\Release\pna2dp-setup.exe'),
    [string]$Package = (Join-Path $PSScriptRoot '..\out\Release\package'),
    [string]$MicrosoftPackage = ''
)
$ErrorActionPreference = 'Stop'
$setupPath = (Resolve-Path -LiteralPath $Setup).Path
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$testRoot = Join-Path $PSScriptRoot ('..\out\verification\setup-signing-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$testRoot = [IO.Path]::GetFullPath($testRoot)
New-Item -ItemType Directory -Path $testRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check-Package([string]$name, [string]$folder, [bool]$expectedSuccess, [bool]$development = $false) {
    $report = Join-Path $testRoot "$name.txt"
    $arguments = @('--verify-package', ('"' + $folder + '"'), '--report', ('"' + $report + '"'))
    if ($development) { $arguments += '--allow-test-driver' }
    $process = Start-Process -FilePath $setupPath -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw "$name timed out" }
    $passed = ($process.ExitCode -eq 0) -eq $expectedSuccess
    $results.Add([pscustomobject]@{ Check = $name; Passed = $passed; ExitCode = $process.ExitCode; Report = $report })
    if (!$passed) { throw "$name failed: $(Get-Content -LiteralPath $report -Raw)" }
}
function New-Fixture([string]$name) {
    $folder = Join-Path $testRoot $name
    New-Item -ItemType Directory -Path $folder | Out-Null
    foreach ($file in @('pna2dp.inf','pna2dp.sys','pna2dp.cat')) { Copy-Item -LiteralPath (Join-Path $packagePath $file) -Destination $folder }
    return $folder
}
try {
    # These regressions specifically exercise the trusted development package,
    # whose bundled .cer must never be used as the production classification.
    Check-Package 'normal-rejects-development' $packagePath $false
    $withoutCer = New-Fixture 'without-cer'
    Check-Package 'removing-cer-is-not-production' $withoutCer $false
    foreach ($name in @('pna2dp.inf','pna2dp.sys','pna2dp.cat')) {
        $fixture = New-Fixture ('tampered-' + $name)
        $path = Join-Path $fixture $name
        if ($name -eq 'pna2dp.inf') { [IO.File]::AppendAllText($path, "`r`n; modified after signing`r`n") }
        else { $bytes = [IO.File]::ReadAllBytes($path); $bytes[[int]($bytes.Length / 2)] = $bytes[[int]($bytes.Length / 2)] -bxor 1; [IO.File]::WriteAllBytes($path, $bytes) }
        Check-Package ('reject-tampered-' + $name) $fixture $false $true
    }
    $missing = New-Fixture 'missing-cat'
    Remove-Item -LiteralPath (Join-Path $missing 'pna2dp.cat')
    Check-Package 'reject-missing-catalog' $missing $false $true
    $destination = Join-Path $testRoot 'must-not-be-created'
    $serviceBefore = (Get-Service pna2dp-codechost -ErrorAction SilentlyContinue).Status
    $process = Start-Process -FilePath $setupPath -ArgumentList @('--silent','--no-devices','--dir', ('"' + $destination + '"')) -PassThru -WindowStyle Hidden
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Silent preflight timed out' }
    if ($process.ExitCode -eq 0 -or (Test-Path -LiteralPath $destination) -or $serviceBefore -ne (Get-Service pna2dp-codechost -ErrorAction SilentlyContinue).Status) {
        throw 'Silent normal-mode rejection changed the installation or service.'
    }
    $results.Add([pscustomobject]@{ Check = 'silent-rejects-before-installation'; Passed = $true; ExitCode = $process.ExitCode })
    if ($MicrosoftPackage) {
        $catalog = @(Get-ChildItem -LiteralPath $MicrosoftPackage -Filter '*.cat' -File)
        $inf = @(Get-ChildItem -LiteralPath $MicrosoftPackage -Filter '*.inf' -File)
        $sys = @(Get-ChildItem -LiteralPath $MicrosoftPackage -Filter '*.sys' -File)
        if ($catalog.Count -ne 1 -or $inf.Count -ne 1 -or $sys.Count -ne 1) { throw 'Use a Microsoft-signed fixture with one CAT, INF and SYS.' }
        & (Join-Path (Split-Path $setupPath) 'signing_test.exe') $catalog[0].FullName $inf[0].FullName $sys[0].FullName
        if ($LASTEXITCODE) { throw 'Microsoft-signed catalog verification failed.' }
        $results.Add([pscustomobject]@{ Check = 'real-Microsoft-signed-catalog-members'; Passed = $true })
        # A trusted third-party catalog must not authorize our unrelated driver.
        & (Join-Path (Split-Path $setupPath) 'signing_test.exe') $catalog[0].FullName $inf[0].FullName (Join-Path $packagePath 'pna2dp.sys')
        if ($LASTEXITCODE -ne 1) { throw 'Unrelated driver was not rejected by catalog membership verification.' }
        $results.Add([pscustomobject]@{ Check = 'Microsoft-catalog-rejects-unrelated-driver'; Passed = $true })
    }
    Write-Host "All $($results.Count) installer signing checks passed. Report: $testRoot"
} finally {
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testRoot 'results.json') -Encoding utf8
}
