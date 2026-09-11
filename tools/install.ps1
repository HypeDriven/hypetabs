param(
    [ValidatePattern('^[a-p]{32}$')][string]$ExtensionId
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'install-core.ps1')
$source = Join-Path $PSScriptRoot '..\build'
$root = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HypeTabs'
$destination = Install-HypeTabsFiles -Source $source -DataDirectory $root
if ($ExtensionId) {
    $origin = "chrome-extension://$ExtensionId/"
    $manifest = @{
        name = 'com.hypetabs.bridge'
        description = 'HypeTabs local browser bridge'
        path = (Join-Path $destination 'HypeTabs.Bridge.exe')
        type = 'stdio'
        allowed_origins = @($origin)
    } | ConvertTo-Json
    $manifestPath = Join-Path $destination 'native-host.json'
    [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object Text.UTF8Encoding($false)))
    $key = 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.hypetabs.bridge'
    [void](New-Item -Path $key -Force)
    Set-Item -Path $key -Value $manifestPath
    [void](New-Item -Path 'HKCU:\Software\HypeTabs' -Force)
    [void](New-ItemProperty -Path 'HKCU:\Software\HypeTabs' -Name 'ExtensionOrigin' -Value $origin -PropertyType String -Force)
    Write-Output 'Registered the native bridge for the specified extension only.'
}
Write-Output "Application: $destination\HypeTabs.exe"
Write-Output "Unpacked extension: $destination\extension"
if (!$ExtensionId) { Write-Output 'Load that extension folder in Chrome, then rerun this script with -ExtensionId and the ID shown by Chrome.' }
