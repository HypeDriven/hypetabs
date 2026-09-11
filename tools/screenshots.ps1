# Captures README screenshots from an isolated host fed with synthetic profiles.
# Output: docs\screenshots\search.png, options.png, about.png. Opens windows briefly on the current desktop.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native-probe-ui.ps1')
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace HypeTabsShot -Name Native -MemberDefinition '[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);'
function Write-Frame($pipe, $value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($value | ConvertTo-Json -Compress)); $size = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $pipe.Write($size, 0, 4); $pipe.Write($bytes, 0, $bytes.Length); $pipe.Flush()
}
function Read-Frame($pipe) {
    $header = New-Object byte[] 4; $task = $pipe.ReadAsync($header, 0, 4); if (!$task.Wait(3000)) { throw 'Pipe response timed out.' }
    $length = [BitConverter]::ToUInt32($header, 0); $bytes = New-Object byte[] $length; $offset = 0
    while ($offset -lt $length) { $task = $pipe.ReadAsync($bytes, $offset, $length - $offset); if (!$task.Wait(3000)) { throw 'Pipe response timed out.' }; $offset += $task.Result }
    return [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
}
function Shot($hwnd, $path) {
    $r = New-Object HypeTabsNativeProbe+Rect; [void][HypeTabsNativeProbe]::GetWindowRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.right - $r.left), ($r.bottom - $r.top); $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.left, $r.top, 0, 0, $bmp.Size); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
# Physical pixels for window rectangles and screen copies on mixed-DPI desktops.
[void][HypeTabsNativeProbe]::SetThreadDpiAwarenessContext([IntPtr](-4))
if (@(Get-Process HypeTabs -ErrorAction SilentlyContinue).Count) { throw 'Exit HypeTabs before capturing screenshots.' }
$output = Join-Path $PSScriptRoot '..\docs\screenshots'; [void](New-Item -ItemType Directory -Path $output -Force)
$dir = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-shots-' + [guid]::NewGuid().ToString('N')); [void](New-Item -ItemType Directory -Path $dir)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\HypeTabs.exe') -Destination $dir
$app = Start-Process -FilePath (Join-Path $dir 'HypeTabs.exe') -ArgumentList @('--data-dir', ('"' + (Join-Path $dir 'data') + '"')) -PassThru
$pipes = @()
try {
    $window = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $window -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $window = [HypeTabsNativeProbe]::Find([uint32]$app.Id) }
    if ($window -eq [IntPtr]::Zero) { throw 'Host window did not appear.' }
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $profiles = @(
        @{id='11111111-1111-4111-8111-111111111111'; label='Work'; tabs=@(
            @{id=1; window=10; title='Quarterly roadmap — Google Docs'; url='https://docs.google.com/document/d/roadmap'; used=900},
            @{id=2; window=10; title='HypeDriven — Pull requests'; url='https://github.com/HypeDriven/hypetabs/pulls'; used=980},
            @{id=3; window=11; title='Windows App SDK release notes'; url='https://learn.microsoft.com/windows/apps/windows-app-sdk/release-notes'; used=700},
            @{id=4; window=11; title='Inbox (3) — Outlook'; url='https://outlook.office.com/mail/'; used=990})},
        @{id='22222222-2222-4222-8222-222222222222'; label='Personal'; tabs=@(
            @{id=1; window=20; title='Weekend hiking routes'; url='https://www.alltrails.com/explore'; used=500},
            @{id=2; window=20; title='Sourdough starter guide'; url='https://www.kingarthurbaking.com/recipes/sourdough-starter-recipe'; used=650},
            @{id=3; window=21; title='Windows 11 taskbar tips'; url='https://support.microsoft.com/windows/taskbar'; used=400})})
    foreach ($profile in $profiles) {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', ('HypeTabs.v1.' + $sid), [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
        $pipe.Connect(3000); $pipes += $pipe
        Write-Frame $pipe @{v=1;type='hello';profile=$profile.id;label=$profile.label}
        [void](Read-Frame $pipe)
        Write-Frame $pipe @{v=1;type='begin'}
        foreach ($tab in $profile.tabs) { Write-Frame $pipe @{v=1;type='upsert';id=$tab.id;window=$tab.window;title=$tab.title;url=$tab.url;used=$tab.used;incognito=$false} }
        Write-Frame $pipe @{v=1;type='end'}
    }
    Write-Frame $pipes[1] @{v=1;type='close';id=3;closed=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - 600000)}
    Start-Sleep -Milliseconds 500
    [void][HypeTabsNativeProbe]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0); Start-Sleep -Milliseconds 500
    [void][HypeTabsNativeProbe]::Query([HypeTabsNativeProbe]::GetDlgItem($window, 301), 'win')
    Start-Sleep -Milliseconds 700
    [void][HypeTabsShot.Native]::SetForegroundWindow($window); Start-Sleep -Milliseconds 300
    Shot $window (Join-Path $output 'search.png')
    [void][HypeTabsNativeProbe]::PostMessage($window, 0x0111, [IntPtr]102, [IntPtr]0); Start-Sleep -Milliseconds 2500
    $options = [HypeTabsShot.Native]::FindWindowW('HypeTabsOptions', [NullString]::Value)
    if ($options -ne [IntPtr]::Zero) { Shot $options (Join-Path $output 'options.png'); [void][HypeTabsNativeProbe]::PostMessage($options, 0x0010, [IntPtr]0, [IntPtr]0); Start-Sleep -Milliseconds 300 }
    [void][HypeTabsNativeProbe]::PostMessage($window, 0x0111, [IntPtr]105, [IntPtr]0); Start-Sleep -Milliseconds 2000
    $about = [HypeTabsShot.Native]::FindWindowW('HypeTabsAbout', [NullString]::Value)
    if ($about -ne [IntPtr]::Zero) { Shot $about (Join-Path $output 'about.png') }
    Write-Output ('Screenshots written to ' + (Resolve-Path $output))
} finally {
    foreach ($pipe in $pipes) { $pipe.Dispose() }
    if (!$app.HasExited) { [void][HypeTabsNativeProbe]::PostMessage($window, 0x0111, [IntPtr]104, [IntPtr]0); if (!$app.WaitForExit(5000)) { Stop-Process -Id $app.Id -Force } }
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
