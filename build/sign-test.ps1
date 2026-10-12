<#
.SYNOPSIS
  Test-signs the driver package with a self-signed certificate for development machines.

  Production releases must be attestation-signed through the Microsoft Partner Center
  (EV certificate required); see docs/04-build-sign-install.md. Test-signed drivers only
  load when test signing is enabled:   bcdedit /set testsigning on   (reboot)

.USAGE
  .\build\sign-test.ps1 -Package .\out\Release\package
#>
param(
    [Parameter(Mandatory)][string]$Package,
    [string]$CertName = 'PN-A2DP Test Certificate'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'toolchain.ps1')

$inf = Join-Path $Package 'pna2dp.inf'
$sys = Join-Path $Package 'pna2dp.sys'
$cat = Join-Path $Package 'pna2dp.cat'
if (-not (Test-Path $inf) -or -not (Test-Path $sys)) { throw "package incomplete: $Package" }

# 1. certificate (created once in the current user's Personal store)
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$CertName" -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) } | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host "creating self-signed test certificate '$CertName'"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$CertName" -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$pfx = Join-Path $Package 'pna2dp-test.cer'
Remove-Item $cat, $pfx -Force -ErrorAction SilentlyContinue

function Invoke-Sign([string]$file) {
    & $SIGNTOOL sign /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 $file | Out-Null
    if ($LASTEXITCODE -ne 0) {
        # no network: sign without timestamp
        & $SIGNTOOL sign /fd SHA256 /sha1 $cert.Thumbprint $file | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
    }
    $s = Get-AuthenticodeSignature $file
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "$file is not signed with $CertName" }
    Write-Host "signed $file"
}

# 2. embedded signature on the driver binary (required by /INTEGRITYCHECK and kernel code integrity)
Invoke-Sign $sys

# 3. catalog for the package; the OS list matches the INF's model decorations
#    (NTamd64.10.0...19041 = 10_VB, ...22000 = 10_CO and later)
if (-not (Test-Path $INF2CAT)) { throw "Inf2Cat.exe not found: $INF2CAT" }
# /uselocaltime: stampinf writes the local date, which is "tomorrow" in UTC after local midnight here
$log = & $INF2CAT /driver:$Package /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64 /uselocaltime /verbose 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $cat)) { $log | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }; throw "inf2cat failed ($LASTEXITCODE): a driver package without a catalog cannot be installed" }
Invoke-Sign $cat

# 4. the public certificate, for the machine trust stores (added after the catalog so it is not part of the package hash list)
Export-Certificate -Cert $cert -FilePath $pfx -Force | Out-Null
Write-Host ""
Write-Host "Signed. To load on this PC (development only):"
Write-Host "  1. bcdedit /set testsigning on   (elevated, then reboot)"
Write-Host "  2. certutil -addstore Root `"$pfx`"  and  certutil -addstore TrustedPublisher `"$pfx`""
Write-Host "  3. .\build\install.ps1 -Package $Package"
