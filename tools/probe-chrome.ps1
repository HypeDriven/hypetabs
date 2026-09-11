param([switch]$Taskbar)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HypeTabsChromeProbe {
    public delegate bool EnumProc(IntPtr window, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    public static IntPtr FindOwnedWindow(uint process) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, param) => { uint owner; GetWindowThreadProcessId(window, out owner); if (owner == process && IsWindowVisible(window)) { found = window; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
$chrome = Join-Path $env:ProgramFiles 'Google\Chrome\Application\chrome.exe'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-chrome-probe-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $directory)
$probeName = if ($Taskbar) { 'taskbar_probe.exe' } else { 'locator_probe.exe' }
$probe = Join-Path $directory $probeName
Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('..\build\' + $probeName)) -Destination $probe
$prior = [HypeTabsChromeProbe]::GetForegroundWindow()
$page = 'data:text/html,' + [Uri]::EscapeDataString('<title>HypeTabs guidance probe</title><div role="tab" aria-selected="true">HypeTabs guidance probe</div><p>Isolated accessibility test.</p>')
$browser = Start-Process -FilePath $chrome -ArgumentList @(('--user-data-dir="' + (Join-Path $directory 'profile') + '"'), '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--new-window', $page) -PassThru -NoNewWindow
$window = [IntPtr]::Zero
try {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ($browser.HasExited) { throw 'The isolated Chrome process exited before creating its window.' }
        $window = [HypeTabsChromeProbe]::FindOwnedWindow([uint32]$browser.Id)
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($window -eq [IntPtr]::Zero) { throw 'No window belonging to the isolated Chrome process appeared.' }
    [void][HypeTabsChromeProbe]::SetForegroundWindow($window)
    Start-Sleep -Milliseconds 1500
    if ($Taskbar) { Write-Output ('Taskbar probe Chrome version: ' + (Get-Item -LiteralPath $chrome).VersionInfo.ProductVersion) }
    & $probe $window.ToInt64().ToString()
    $result = $LASTEXITCODE
} finally {
    if ($window -ne [IntPtr]::Zero) { [void][HypeTabsChromeProbe]::PostMessage($window, 0x0010, [IntPtr]0, [IntPtr]0) }
    if (!$browser.WaitForExit(5000)) { Stop-Process -Id $browser.Id -Force; $browser.WaitForExit() }
    [void][HypeTabsChromeProbe]::SetForegroundWindow($prior)
    try { Remove-Item -LiteralPath $directory -Recurse -Force } catch { Write-Output ('Temporary probe cleanup pending: ' + $directory) }
}
exit $result
