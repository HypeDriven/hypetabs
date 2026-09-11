. (Join-Path $PSScriptRoot 'uninstall-core.ps1')
function Install-HypeTabsFiles {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$DataDirectory)
    $ErrorActionPreference = 'Stop'
    $artifacts = @('HypeTabs.exe', 'HypeTabs.Bridge.exe', 'extension\manifest.json', 'extension\worker.js')
    $scripts = @('uninstall.ps1', 'uninstall-core.ps1')
    $files = @('App\native-host.json') + @($artifacts + $scripts | ForEach-Object { 'App\' + $_ })
    $root = Get-ValidatedHypeTabsRoot -DataDirectory $DataDirectory -Files $files
    $destination = Join-Path $root 'App'
    foreach ($relative in $artifacts + $scripts) {
        $path = Join-Path $(if ($relative -in $scripts) { $PSScriptRoot } else { $Source }) $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing build artifact: $relative. Build the native host and extension first." }
        if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Expected a regular build artifact: $relative" }
    }
    foreach ($process in @(Get-Process HypeTabs,HypeTabs.Bridge -ErrorAction SilentlyContinue)) {
        if ($process.Path -and $process.Path.StartsWith($destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Exit HypeTabs and disable its Chrome extension before updating this installation.' }
    }
    [void](New-Item -ItemType Directory -Force -Path (Join-Path $destination 'extension'))
    foreach ($relative in $artifacts + $scripts) {
        $path = Join-Path $(if ($relative -in $scripts) { $PSScriptRoot } else { $Source }) $relative
        Copy-Item -LiteralPath $path -Destination (Join-Path $destination $relative) -Force
    }
    return $destination
}
