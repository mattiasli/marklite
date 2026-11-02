# Signs a file with the certificate in SIGNING_CERT_PFX_BASE64 and SIGNING_CERT_PASSWORD, using
# signtool from the Windows SDK and an RFC 3161 timestamp. release.yml runs it when the repository
# variable SIGN_RELEASES is true.
param([Parameter(Mandatory)] [string] $File)
$ErrorActionPreference = 'Stop'

if (-not $env:SIGNING_CERT_PFX_BASE64) { throw "SIGNING_CERT_PFX_BASE64 is not set" }

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1
if (-not $signtool) { throw "signtool.exe was not found" }

$pfx = Join-Path $env:RUNNER_TEMP "signing.pfx"
try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:SIGNING_CERT_PFX_BASE64))
    & $signtool.FullName sign /f $pfx /p $env:SIGNING_CERT_PASSWORD /fd SHA256 `
        /tr http://timestamp.digicert.com /td SHA256 /d "MarkLite" /du "https://marklite.vercel.app" $File
    if ($LASTEXITCODE) { throw "signtool failed with exit code $LASTEXITCODE" }
    & $signtool.FullName verify /pa $File
    if ($LASTEXITCODE) { throw "the signature on $File did not verify" }
} finally {
    Remove-Item $pfx -Force -ErrorAction SilentlyContinue
}
