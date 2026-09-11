$ErrorActionPreference = 'Stop'
# Google first-party browser test tool, kept outside application/release assets.
$version = '153.0.8010.36'
$root = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-CfT-' + $version)
$archive = Join-Path $root 'chrome.zip'
$exe = Join-Path $root 'chrome-win64\chrome.exe'
# Pins the artifact fetched from Google's published HTTPS URL on 2026-09-09.
# This is reproducibility/integrity pinning, not an independent publisher signature.
$expected = '8EDFAA0923C11A30A9315A5E7E5794C5EFB60146EDEA7E3F749F7FDC2AA026CB'
[void](New-Item -ItemType Directory -Path $root -Force)
if (!(Test-Path -LiteralPath $archive)) {
    & curl.exe --fail --location --silent --show-error --connect-timeout 30 --max-time 240 --output $archive "https://storage.googleapis.com/chrome-for-testing-public/$version/win64/chrome-win64.zip"
    if ($LASTEXITCODE -ne 0) { throw 'Chrome for Testing download failed.' }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'Chrome for Testing archive does not match the pinned artifact.' }
Expand-Archive -LiteralPath $archive -DestinationPath $root -Force
$signature = Get-AuthenticodeSignature -LiteralPath $exe
if ($signature.Status -eq 'NotSigned') {
    Write-Output 'Chrome for Testing is unsigned; provenance is the official Google HTTPS download, pinned by archive SHA256.'
} elseif ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Google LLC') {
    throw 'Unexpected Chrome for Testing signature status.'
}
$setup = Start-Process -FilePath (Join-Path $root 'chrome-win64\setup.exe') -ArgumentList ('--configure-browser-in-directory="' + (Join-Path $root 'chrome-win64') + '"') -PassThru -NoNewWindow -Wait
# Chromium InstallStatus::CONFIGURE_APP_CONTAINER_SANDBOX_SUCCESS is 78.
if ($setup.ExitCode -ne 78) { throw ('Chrome sandbox directory configuration failed with exit code: ' + $setup.ExitCode) }
Write-Output ('Prepared isolated test browser with sandbox directory permissions: ' + $exe)
