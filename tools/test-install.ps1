$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'install-core.ps1')
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-install-test-' + [guid]::NewGuid().ToString('N'))
$links = @()
function Expect-Refusal([scriptblock]$Action, [string]$Reason) {
    $refused = $false
    try { & $Action | Out-Null } catch { if (!$_.Exception.Message.Contains($Reason)) { throw }; $refused = $true }
    if (!$refused) { throw "Expected setup refusal: $Reason" }
}
try {
    $source = Join-Path $directory 'source'
    [void](New-Item -ItemType Directory -Path (Join-Path $source 'extension') -Force)
    $artifacts = @('HypeTabs.exe','HypeTabs.Bridge.exe','extension\manifest.json','extension\worker.js')
    foreach ($file in $artifacts) { [IO.File]::WriteAllText((Join-Path $source $file), 'synthetic '+$file) }
    $root = Join-Path $directory 'normal'
    $destination = Install-HypeTabsFiles -Source $source -DataDirectory $root
    foreach ($file in $artifacts) {
        if ([IO.File]::ReadAllText((Join-Path $destination $file)) -ne ('synthetic '+$file)) { throw 'Copied content changed.' }
    }
    foreach ($file in @('uninstall.ps1','uninstall-core.ps1')) {
        if ((Get-FileHash -LiteralPath (Join-Path $destination $file)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $file)).Hash) { throw 'Removal script was not packaged correctly.' }
    }
    $unknown = Join-Path $destination 'unrelated.txt'
    [IO.File]::WriteAllText($unknown, 'preserve')
    [void](Install-HypeTabsFiles -Source $source -DataDirectory $root)
    if ([IO.File]::ReadAllText($unknown) -ne 'preserve') { throw 'Update changed an unknown file.' }
    $target = Join-Path $directory 'target'
    [void](New-Item -ItemType Directory -Path $target)
    $sentinel = Join-Path $target 'sentinel.txt'
    [IO.File]::WriteAllText($sentinel, 'preserve')
    foreach ($placement in @('root','app','extension','ancestor')) {
        $root = Join-Path $directory ('redirect-'+$placement)
        $link = $root
        if ($placement -eq 'app') { [void](New-Item -ItemType Directory -Path $root); $link = Join-Path $root 'App' }
        if ($placement -eq 'extension') { [void](New-Item -ItemType Directory -Path (Join-Path $root 'App') -Force); $link = Join-Path $root 'App\extension' }
        [void](New-Item -ItemType Junction -Path $link -Target $target); $links += $link
        if ($placement -eq 'ancestor') { $root = Join-Path $link 'nested' }
        Expect-Refusal { Install-HypeTabsFiles -Source $source -DataDirectory $root } 'installation directory is redirected'
        if (@(Get-ChildItem -LiteralPath $target -Force).Count -ne 1 -or [IO.File]::ReadAllText($sentinel) -ne 'preserve') { throw 'Setup wrote through a redirected directory.' }
    }
    $root = Join-Path $directory 'directory-as-file'
    [void](New-Item -ItemType Directory -Path (Join-Path $root 'App\extension\worker.js') -Force)
    Expect-Refusal { Install-HypeTabsFiles -Source $source -DataDirectory $root } 'Expected a regular application file'
    if (Test-Path -LiteralPath (Join-Path $root 'App\HypeTabs.exe')) { throw 'Setup wrote files before validating all destinations.' }
    $root = Join-Path $directory 'file-as-directory'
    [void](New-Item -ItemType Directory -Path $root)
    [IO.File]::WriteAllText((Join-Path $root 'App'), 'preserve')
    Expect-Refusal { Install-HypeTabsFiles -Source $source -DataDirectory $root } 'Expected an installation directory'
    $root = Join-Path $directory 'missing-source'
    Remove-Item -LiteralPath (Join-Path $source 'extension\worker.js')
    Expect-Refusal { Install-HypeTabsFiles -Source $source -DataDirectory $root } 'Missing build artifact'
    if (Test-Path -LiteralPath $root) { throw 'Setup created an installation before source validation.' }
    Write-Output 'PASS: installation copies exact artifacts and removal scripts, preserves unknown files, and refuses redirected or malformed paths and missing sources before writing.'
} finally {
    foreach ($link in $links) { if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link) } }
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
}
