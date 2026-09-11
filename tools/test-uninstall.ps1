$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uninstall-core.ps1')
$id = [guid]::NewGuid().ToString('N')
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-uninstall-test-' + $id)
$registry = 'HKCU:\Software\HypeTabs.Tests\Uninstall-' + $id
$origin = 'chrome-extension://' + ('a' * 32) + '/'
$links = @()
$ownedProcess = $null
function Expect-Refusal([scriptblock]$Action, [string]$Reason) {
    $refused = $false
    try { & $Action } catch {
        if (!$_.Exception.Message.Contains($Reason)) { throw }
        $refused = $true
    }
    if (!$refused) { throw "Uninstall did not refuse: $Reason" }
}
try {
    foreach ($scenario in @('owned', 'keep', 'foreign', 'preview')) {
        $root = Join-Path $directory $scenario
        $app = Join-Path $root 'App'
        [void](New-Item -ItemType Directory -Path (Join-Path $app 'extension') -Force)
        foreach ($file in @('App\HypeTabs.exe', 'App\HypeTabs.Bridge.exe', 'App\extension\manifest.json', 'App\extension\worker.js', 'settings.bin', 'closed.dat', 'unrelated.txt')) {
            [IO.File]::WriteAllText((Join-Path $root $file), 'synthetic test data')
        }
        $native = $registry + '\' + $scenario + '\Native'
        $settings = $registry + '\' + $scenario + '\Settings'
        $run = $registry + '\' + $scenario + '\Run'
        foreach ($key in @($native, $settings, $run)) { [void](New-Item -Path $key -Force) }
        $manifest = Join-Path $app 'native-host.json'
        @{path=(Join-Path $app 'HypeTabs.Bridge.exe');allowed_origins=@($origin)} | ConvertTo-Json | Set-Content -LiteralPath $manifest
        Set-Item -LiteralPath $native -Value $(if ($scenario -eq 'foreign') { 'C:\DifferentApp\native-host.json' } else { $manifest })
        [void](New-ItemProperty -LiteralPath $native -Name Unrelated -Value 'preserve')
        [void](New-ItemProperty -LiteralPath $settings -Name ExtensionOrigin -Value $origin)
        [void](New-ItemProperty -LiteralPath $settings -Name Unrelated -Value 'preserve')
        $startup = if ($scenario -eq 'foreign') { '"C:\DifferentApp\HypeTabs.exe"' } else { '"' + (Join-Path $app 'HypeTabs.exe') + '"' }
        [void](New-ItemProperty -LiteralPath $run -Name HypeTabs -Value $startup)
        Remove-HypeTabsInstallation -DataDirectory $root -NativeKey $native -SettingsKey $settings -RunKey $run -KeepData:($scenario -eq 'keep') -WhatIf:($scenario -eq 'preview')
        $preview = $scenario -eq 'preview'
        if ((Test-Path -LiteralPath (Join-Path $app 'HypeTabs.exe')) -ne $preview) { throw "$scenario application removal failed." }
        if ((Test-Path -LiteralPath (Join-Path $root 'closed.dat')) -ne ($preview -or $scenario -eq 'keep')) { throw "$scenario data retention failed." }
        if (!(Test-Path -LiteralPath (Join-Path $root 'unrelated.txt'))) { throw 'Removed an unknown file.' }
        if ((Get-ItemProperty -LiteralPath $native).Unrelated -ne 'preserve') { throw 'Removed an unrelated native-host registry value.' }
        $remainingStartup = (Get-ItemProperty -LiteralPath $run -Name HypeTabs -ErrorAction SilentlyContinue).HypeTabs
        $remainingOrigin = (Get-ItemProperty -LiteralPath $settings -Name ExtensionOrigin -ErrorAction SilentlyContinue).ExtensionOrigin
        if ($scenario -in @('foreign', 'preview')) {
            if ($remainingStartup -ne $startup -or $remainingOrigin -ne $origin) { throw 'Changed another registration or preview state.' }
        } elseif ($remainingStartup -or $remainingOrigin -or (Get-Item -LiteralPath $native).GetValue('')) { throw 'Owned registry values remain.' }
        if ($scenario -eq 'owned') {
            Remove-HypeTabsInstallation -DataDirectory $root -NativeKey $native -SettingsKey $settings -RunKey $run
            if (!(Test-Path -LiteralPath (Join-Path $root 'unrelated.txt'))) { throw 'Repeated uninstall removed an unknown file.' }
        }
    }
    # Junctions require no administrator privileges. Every target is synthetic
    # and every link is removed explicitly before recursive fixture cleanup.
    $target = Join-Path $directory 'link-target'
    [void](New-Item -ItemType Directory -Path $target)
    $sentinel = Join-Path $target 'unrelated.txt'
    [IO.File]::WriteAllText($sentinel, 'must survive every refusal')
    foreach ($placement in @('root', 'app', 'extension', 'ancestor')) {
        $root = Join-Path $directory ('redirect-' + $placement)
        $link = $root
        if ($placement -eq 'app') { [void](New-Item -ItemType Directory -Path $root); $link = Join-Path $root 'App' }
        if ($placement -eq 'extension') { [void](New-Item -ItemType Directory -Path (Join-Path $root 'App') -Force); $link = Join-Path $root 'App\extension' }
        [void](New-Item -ItemType Junction -Path $link -Target $target)
        $links += $link
        if ($placement -eq 'ancestor') { $root = Join-Path $link 'nested-installation' }
        Expect-Refusal { Remove-HypeTabsInstallation -DataDirectory $root -NativeKey ($registry+'\GuardNative') -SettingsKey ($registry+'\GuardSettings') -RunKey ($registry+'\GuardRun') } 'installation directory is redirected'
        if ([IO.File]::ReadAllText($sentinel) -ne 'must survive every refusal') { throw 'Redirected target changed.' }
    }
    Expect-Refusal { Remove-HypeTabsInstallation -DataDirectory ([IO.Path]::GetPathRoot($directory)) -NativeKey ($registry+'\GuardNative') -SettingsKey ($registry+'\GuardSettings') -RunKey ($registry+'\GuardRun') } 'drive root'
    Expect-Refusal { Remove-HypeTabsInstallation -DataDirectory $directory -NativeKey 'HKLM:\Software\HypeTabs.Tests' -SettingsKey ($registry+'\GuardSettings') -RunKey ($registry+'\GuardRun') } 'current-user registry'
    Write-Output 'PASS: redirected root, parent, App, and extension directories, drive roots, and non-user registry paths are refused.'

    if (@(Get-Process HypeTabs,HypeTabs.Bridge -ErrorAction SilentlyContinue).Count) { throw 'Exit existing HypeTabs processes before the running-process removal test.' }
    $root = Join-Path $directory 'running'
    $app = Join-Path $root 'App'
    [void](New-Item -ItemType Directory -Path $app -Force)
    $executable = Join-Path $app 'HypeTabs.exe'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\HypeTabs.exe') -Destination $executable
    $ownedProcess = Start-Process -FilePath $executable -ArgumentList @('--data-dir', ('"'+$root+'"')) -PassThru
    if ($ownedProcess.WaitForExit(500)) { throw 'The isolated application exited before the running-process check.' }
    Expect-Refusal { Remove-HypeTabsInstallation -DataDirectory $root -NativeKey ($registry+'\GuardNative') -SettingsKey ($registry+'\GuardSettings') -RunKey ($registry+'\GuardRun') } 'Exit HypeTabs'
    $ownedProcess.Refresh()
    if ($ownedProcess.HasExited -or !(Test-Path -LiteralPath $executable)) { throw 'Removal changed the running application.' }
    Write-Output 'PASS: removal refuses a running production tray host and preserves its executable.'
    Write-Output 'PASS: uninstall ownership, data retention, unknown-file preservation, and WhatIf using isolated files and registry keys.'
} finally {
    if ($ownedProcess -and !$ownedProcess.HasExited) { $ownedProcess.Kill(); $ownedProcess.WaitForExit() }
    foreach ($link in $links) { if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link) } }
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    if (Test-Path -LiteralPath $registry) { Remove-Item -LiteralPath $registry -Recurse -Force }
}
