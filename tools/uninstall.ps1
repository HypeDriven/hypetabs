[CmdletBinding(SupportsShouldProcess = $true)]
param([switch]$KeepData)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uninstall-core.ps1')
Remove-HypeTabsInstallation -DataDirectory (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'HypeTabs') `
    -NativeKey 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.hypetabs.bridge' `
    -SettingsKey 'HKCU:\Software\HypeTabs' -RunKey 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
    -KeepData:$KeepData -WhatIf:$WhatIfPreference
