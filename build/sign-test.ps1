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
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$CertName" } | Select-Object -First 1
if (-not $cert) {
    Write-Host "creating self-signed test certificate '$CertName'"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$CertName" -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$pfx = Join-Path $Package 'pna2dp-test.cer'
Export-Certificate -Cert $cert -FilePath $pfx -Force | Out-Null

# 2. catalog
if (Test-Path $INF2CAT) {
    & $INF2CAT /driver:$Package /os:10_X64,10_NI_X64,10_RS5_X64 /verbose | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "inf2cat failed ($LASTEXITCODE); continuing with the .sys signature only" }
}

# 3. sign
$targets = @($sys)
if (Test-Path $cat) { $targets += $cat }
foreach ($t in $targets) {
    & $SIGNTOOL sign /v /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 $t
    if ($LASTEXITCODE -ne 0) {
        # no network: sign without timestamp
        & $SIGNTOOL sign /v /fd SHA256 /sha1 $cert.Thumbprint $t
        if ($LASTEXITCODE -ne 0) { throw "signtool failed for $t" }
    }
}
Write-Host ""
Write-Host "Signed. To load on this PC (development only):"
Write-Host "  1. bcdedit /set testsigning on   (elevated, then reboot)"
Write-Host "  2. certutil -addstore Root `"$pfx`"  and  certutil -addstore TrustedPublisher `"$pfx`""
Write-Host "  3. .\build\install.ps1 -Package $Package"
