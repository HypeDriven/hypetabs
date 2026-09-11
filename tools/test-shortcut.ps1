# Synthetic Win+W check for the keyboard-hook shortcut path: starts an isolated
# host, injects Win+W twice through SendInput, and expects the search widget to
# show and then hide while the foreground stays with HypeTabs (no Start menu or
# Widgets board). Injects keyboard input; do not run while the user is typing.
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class HypeTabsKeys {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public uint pad0; public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; public long pad; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static string Class(IntPtr h) { var c = new StringBuilder(64); GetClassName(h, c, 64); return c.ToString(); }
  public static void Key(ushort vk, bool up) { var i = new INPUT[1]; i[0].type = 1; i[0].wVk = vk; i[0].dwFlags = up ? 2u : 0u; SendInput(1, i, Marshal.SizeOf(typeof(INPUT))); }
  public static void Chord(ushort modifier, ushort key) { Key(modifier, false); System.Threading.Thread.Sleep(40); Key(key, false); System.Threading.Thread.Sleep(40); Key(key, true); System.Threading.Thread.Sleep(40); Key(modifier, true); }
}
"@
$ErrorActionPreference = 'Stop'
if (Get-Process HypeTabs -ErrorAction SilentlyContinue) { throw 'Close the running HypeTabs instance first.' }
$dir = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-shortcut-' + [guid]::NewGuid().ToString('N')); [void](New-Item -ItemType Directory -Path $dir)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\HypeTabs.exe') -Destination $dir
$app = Start-Process -FilePath (Join-Path $dir 'HypeTabs.exe') -ArgumentList @('--data-dir', ('"' + (Join-Path $dir 'data') + '"')) -PassThru
try {
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 60 -and $window -eq [IntPtr]::Zero; $attempt++) { Start-Sleep -Milliseconds 50; $window = [HypeTabsKeys]::FindWindow('HypeTabsSearch', [NullString]::Value) }
    if ($window -eq [IntPtr]::Zero) { throw 'Search window not created.' }
    if ([HypeTabsKeys]::FindWindow('#32770', 'HypeTabs') -ne [IntPtr]::Zero) { throw 'The host reported a shortcut conflict instead of hooking Win+W.' }
    Start-Sleep -Milliseconds 500
    [HypeTabsKeys]::Chord(0x5B, 0x57); Start-Sleep -Milliseconds 700
    if (![HypeTabsKeys]::IsWindowVisible($window)) { throw 'Win+W did not show the search widget.' }
    if ([HypeTabsKeys]::GetForegroundWindow() -ne $window) { throw ('Win+W left the foreground with ' + [HypeTabsKeys]::Class([HypeTabsKeys]::GetForegroundWindow()) + ' instead of the widget.') }
    [HypeTabsKeys]::Chord(0x5B, 0x57); Start-Sleep -Milliseconds 700
    if ([HypeTabsKeys]::IsWindowVisible($window)) { throw 'A second Win+W did not hide the search widget.' }
    Write-Output 'PASS: Win+W shows and hides the search widget through the keyboard hook without opening Start.'
} finally {
    if (!$app.HasExited) { $app.Kill(); $app.WaitForExit() }
    Remove-Item -LiteralPath $dir -Recurse -Force
}
