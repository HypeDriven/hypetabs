function Get-ValidatedHypeTabsRoot {
    param([Parameter(Mandatory)][string]$DataDirectory, [string[]]$Files = @())
    $root = [IO.Path]::GetFullPath($DataDirectory).TrimEnd('\')
    if (!$root -or $root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { throw 'Refusing to use a drive root.' }
    $directories = @((Join-Path $root 'App'), (Join-Path $root 'App\extension'))
    for ($ancestor = $root; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) { $directories += $ancestor }
    foreach ($path in $directories) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'An installation directory is redirected. Remove the link manually after checking its target.' }
            if (!$item.PSIsContainer) { throw 'Expected an installation directory.' }
        }
    }
    foreach ($relative in $Files) {
        $path = Join-Path $root $relative
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Expected a regular application file: $relative" }
        }
    }
    return $root
}
function Remove-HypeTabsInstallation {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory)][string]$DataDirectory,
        [Parameter(Mandatory)][string]$NativeKey,
        [Parameter(Mandatory)][string]$SettingsKey,
        [Parameter(Mandatory)][string]$RunKey,
        [switch]$KeepData
    )
    $ErrorActionPreference = 'Stop'
    foreach ($keyPath in @($NativeKey, $SettingsKey, $RunKey)) {
        if (!$keyPath.StartsWith('HKCU:\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Only current-user registry paths are supported.' }
    }
    $root = Get-ValidatedHypeTabsRoot -DataDirectory $DataDirectory
    $app = Join-Path $root 'App'
    foreach ($process in @(Get-Process HypeTabs,HypeTabs.Bridge -ErrorAction SilentlyContinue)) {
        if ($process.Path -and $process.Path.StartsWith($app + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Exit HypeTabs and remove its extension from every Chrome profile before uninstalling.'
        }
    }
    $manifest = Join-Path $app 'native-host.json'
    $exe = Join-Path $app 'HypeTabs.exe'
    $nativeOwned = $false
    if (Test-Path -LiteralPath $NativeKey) {
        $nativeOwned = (Get-Item -LiteralPath $NativeKey).GetValue('') -ieq $manifest
    }
    $originOwned = $false
    if ($nativeOwned -and (Test-Path -LiteralPath $manifest)) {
        try {
            $registration = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
            $origin = (Get-ItemProperty -LiteralPath $SettingsKey -Name ExtensionOrigin -ErrorAction SilentlyContinue).ExtensionOrigin
            $originOwned = $registration.path -ieq (Join-Path $app 'HypeTabs.Bridge.exe') -and
                @($registration.allowed_origins).Count -eq 1 -and $origin -ceq $registration.allowed_origins[0]
        } catch { $originOwned = $false }
    }
    $runOwned = $false
    if (Test-Path -LiteralPath $RunKey) {
        $runOwned = (Get-ItemProperty -LiteralPath $RunKey -Name HypeTabs -ErrorAction SilentlyContinue).HypeTabs -ieq ('"' + $exe + '"')
    }
    $files = @('App\HypeTabs.exe', 'App\HypeTabs.Bridge.exe', 'App\native-host.json',
        'App\extension\manifest.json', 'App\extension\worker.js', 'App\uninstall.ps1', 'App\uninstall-core.ps1')
    if (!$KeepData) { $files += @('settings.bin', 'settings.bin.tmp', 'closed.dat', 'closed.dat.pending') }
    [void](Get-ValidatedHypeTabsRoot -DataDirectory $root -Files $files)
    if (!$PSCmdlet.ShouldProcess($root, 'Remove HypeTabs application files and owned registrations')) { return }
    if ($runOwned) { Remove-ItemProperty -LiteralPath $RunKey -Name HypeTabs }
    if ($originOwned) { Remove-ItemProperty -LiteralPath $SettingsKey -Name ExtensionOrigin }
    if ($nativeOwned) {
        $writable = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($NativeKey.Substring(6), $true)
        try { $writable.DeleteValue('') } finally { if ($writable) { $writable.Dispose() } }
        $key = Get-Item -LiteralPath $NativeKey
        if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) { Remove-Item -LiteralPath $NativeKey }
    }
    foreach ($relative in $files) {
        $path = Join-Path $root $relative
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    # Preserve unknown files and unrelated registry values; never recurse.
    foreach ($path in @((Join-Path $app 'extension'), $app, $root)) {
        if ((Test-Path -LiteralPath $path) -and @(Get-ChildItem -LiteralPath $path -Force).Count -eq 0) { Remove-Item -LiteralPath $path }
    }
    if ((Test-Path -LiteralPath $SettingsKey)) {
        $key = Get-Item -LiteralPath $SettingsKey
        if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) { Remove-Item -LiteralPath $SettingsKey }
    }
    Write-Output $(if ($KeepData) { 'Removed HypeTabs. Saved settings and closed tabs were retained.' } else { 'Removed HypeTabs and its saved settings and closed tabs.' })
}
