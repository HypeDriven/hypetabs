$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HypeTabsSmoke {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    [DllImport("user32.dll", EntryPoint="GetClientRect")] private static extern bool NativeClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public static bool GetClientRect(IntPtr window, out Rect rect) {
        // LB_GETITEMHEIGHT returns physical pixels. Match that coordinate space
        // instead of comparing it with a DPI-virtualized PowerShell rectangle.
        IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try { return NativeClientRect(window, out rect); }
        finally { if (previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
    }
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct MonitorInfo { public int size; public Rect monitor, work; public uint flags; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    public static string Layout(IntPtr window, IntPtr list) {
        Rect client, rows; GetClientRect(window, out client); GetClientRect(list, out rows);
        MonitorInfo info = new MonitorInfo(); info.size = Marshal.SizeOf(typeof(MonitorInfo));
        GetMonitorInfo(MonitorFromWindow(window, 2), ref info);
        return "dpi=" + GetDpiForWindow(window) + " clientHeight=" + client.bottom + " listHeight=" + rows.bottom +
            " rowHeight=" + SendMessage(list, 0x01A1, IntPtr.Zero, IntPtr.Zero) + " workHeight=" + (info.work.bottom-info.work.top);
    }
    [StructLayout(LayoutKind.Sequential)] public struct GuiInfo {
        public int size, flags;
        public IntPtr active, focus, capture, menuOwner, moveSize, caret;
        public Rect caretRect;
    }
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GuiInfo info);
    public static IntPtr FocusedControl(IntPtr window) {
        uint process; uint thread = GetWindowThreadProcessId(window, out process);
        GuiInfo info = new GuiInfo(); info.size = Marshal.SizeOf(typeof(GuiInfo));
        return GetGUIThreadInfo(thread, ref info) ? info.focus : IntPtr.Zero;
    }
    public static int VisibleRows(IntPtr list) {
        Rect rect; if (!GetClientRect(list, out rect)) return -1;
        int height = SendMessage(list, 0x01A1, IntPtr.Zero, IntPtr.Zero).ToInt32();
        return height > 0 ? (rect.bottom - rect.top) / height : -1;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern IntPtr SendBounded(IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    public static IntPtr SendMessage(IntPtr window, uint message, IntPtr w, IntPtr l) {
        IntPtr result;
        if (SendBounded(window, message, w, l, 2, 2000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Window message timed out or failed: " + message.ToString("X") + " error=" + Marshal.GetLastWin32Error());
        return result;
    }
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SetControlText(IntPtr window, uint message, IntPtr w, string text);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    public static IntPtr FindOptionsWindow() { return FindWindow("HypeTabsOptions", null); }
    public static string ListText(IntPtr list, int index) {
        int length = SendMessage(list, 0x018A, (IntPtr)index, IntPtr.Zero).ToInt32();
        if (length < 0 || length > 20000) return "";
        IntPtr buffer = Marshal.AllocHGlobal((length + 1) * 2);
        try { SendMessage(list, 0x0189, (IntPtr)index, buffer); return Marshal.PtrToStringUni(buffer); }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    public static IntPtr FindSearchWindow() { return FindWindow("HypeTabsSearch", null); }
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
}
'@
$exe = Join-Path $PSScriptRoot '..\build\HypeTabs.exe'
if ([HypeTabsSmoke]::FindSearchWindow() -ne [IntPtr]::Zero) { throw 'An existing HypeTabs instance must be closed before this isolated smoke check.' }
$smokeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("HypeTabs-smoke-" + [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $smokeDirectory)
Copy-Item -LiteralPath $exe -Destination (Join-Path $smokeDirectory "HypeTabs.exe")
$exe = Join-Path $smokeDirectory "HypeTabs.exe"
function Connect-TestProfile {
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', ("HypeTabs.v1." + $sid), [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
    $pipe.Connect(3000)
    return $pipe
}
function Write-Frame($pipe, $value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($value | ConvertTo-Json -Compress))
    $size = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $pipe.Write($size, 0, 4); $pipe.Write($bytes, 0, $bytes.Length); $pipe.Flush()
}
function Read-Exact($pipe, [int]$length) {
    $bytes = New-Object byte[] $length
    $offset = 0
    while ($offset -lt $length) {
        $read = $pipe.ReadAsync($bytes, $offset, $length - $offset)
        if (!$read.Wait(3000)) { $pipe.Dispose(); throw 'Pipe response timed out.' }
        if ($read.Result -eq 0) { throw 'Pipe disconnected.' }
        $offset += $read.Result
    }
    return ,$bytes
}
function Read-Frame($pipe) {
    $header = Read-Exact $pipe 4
    $length = [BitConverter]::ToUInt32($header, 0)
    if ($length -eq 0 -or $length -gt 65536) { throw 'Invalid response frame size.' }
    return [Text.Encoding]::UTF8.GetString((Read-Exact $pipe $length)) | ConvertFrom-Json
}
$script:preparations = 0
function Read-Activation($pipe) {
    $message = Read-Frame $pipe
    if ($message.type -eq 'prepare') {
        ++$script:preparations
        if ($script:preparations % 2) {
            Write-Frame $pipe @{v=1;type='prepared';request=$message.request;title='Synthetic taskbar fallback';left=-99999;top=-99999;width=1;height=1}
        } else {
            Write-Frame $pipe @{v=1;type='result';request=$message.request;status='unavailable'}
        }
        $activation = Read-Frame $pipe
        if ($activation.type -ne 'activate' -or $activation.id -ne $message.id -or $activation.request -eq $message.request) {
            throw 'Preparation fallback did not send a newly correlated activation for the same tab.'
        }
        return $activation
    }
    return $message
}
$connections = @()
$app = Start-Process -FilePath $exe -ArgumentList @('--data-dir', ('"' + (Join-Path $smokeDirectory 'data') + '"')) -PassThru -NoNewWindow
try {
    $window = [IntPtr]::Zero
    for ($i = 0; $i -lt 300; $i++) {
        Start-Sleep -Milliseconds 100
        if ($app.HasExited) { throw "Application exited during startup with code $($app.ExitCode)." }
        $window = [HypeTabsSmoke]::FindSearchWindow()
        if ($window -ne [IntPtr]::Zero) { break }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Search window was not created.' }
    [uint32]$owner = 0
    [void][HypeTabsSmoke]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -ne $app.Id) { throw 'Unexpected window owner.' }
    if ([HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Overlay must start hidden.' }
    $app.Refresh()
    if ($app.PriorityClass -ne 'Idle') { throw "Unexpected priority: $($app.PriorityClass)" }
    $second = Start-Process -FilePath $exe -PassThru -NoNewWindow
    if (!$second.WaitForExit(3000)) { $second.Kill(); throw 'Second instance did not exit.' }
    for ($profileIndex = 0; $profileIndex -lt 2; $profileIndex++) {
        $pipe = Connect-TestProfile; $connections += $pipe
        Write-Frame $pipe @{v=1;type='hello';profile=("00000000-0000-0000-0000-00000000000" + $profileIndex);label=("Test profile " + $profileIndex)}
        $reply = Read-Frame $pipe
        if ($reply.type -ne 'collect' -or !$reply.enabled) { throw 'Missing collection handshake.' }
        Write-Frame $pipe @{v=1;type='begin'}
        Write-Frame $pipe @{v=1;type='upsert';id=1;window=10;title='Shared test title';url='https://example.test/';used=(100 + $profileIndex);incognito=$false}
        Write-Frame $pipe @{v=1;type='end'}
    }
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 300
    if (![HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Shortcut message did not show overlay.' }
    $list = [HypeTabsSmoke]::GetDlgItem($window, 302)
    # Screen-reader exposure is checked from another process with the native UI Automation client.
    $accessibilityProbe = Join-Path $PSScriptRoot '..\build\accessibility_probe.exe'
    if (!(Test-Path -LiteralPath $accessibilityProbe)) { throw 'Build tools\build-accessibility-probe.cmd before the smoke test.' }
    $accessibilityResult = & { $ErrorActionPreference = 'Continue'; & $accessibilityProbe ([string][int64]$window) 2>&1 }
    $accessibilityExit = $LASTEXITCODE
    if ($accessibilityExit -ne 0) { throw ('Search overlay accessibility exposure failed: ' + (($accessibilityResult | ForEach-Object { [string]$_ }) -join ' ')) }
    $accessibilityResult | ForEach-Object { Write-Output ([string]$_) }
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        $visibleRows = [HypeTabsSmoke]::VisibleRows($list)
        if ($visibleRows -eq 8) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($visibleRows -ne 8) { throw ("Initial search layout does not show eight result rows (observed $visibleRows). " + [HypeTabsSmoke]::Layout($window, $list)) }
    $count = 0
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $count = [HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
        if ($count -eq 2) { break }
        Start-Sleep -Milliseconds 50
    }
    if ($count -ne 2) { throw "Expected two profile-scoped results, found $count." }
    for ($id = 20; $id -lt 30; $id++) {
        Write-Frame $connections[0] @{v=1;type='upsert';id=$id;window=10;title=('Scroll test ' + $id);url='https://example.test/scroll';used=1;incognito=$false}
    }
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -eq 12) { break }
        Start-Sleep -Milliseconds 50
    }
    if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 12) { throw 'Additional scroll results were lost.' }
    [void][HypeTabsSmoke]::PostMessage($list, 0x0100, [IntPtr]35, [IntPtr]0)
    Start-Sleep -Milliseconds 100
    if ([HypeTabsSmoke]::SendMessage($list, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32() -ne 11 -or
        [HypeTabsSmoke]::SendMessage($list, 0x018E, [IntPtr]0, [IntPtr]0).ToInt32() -le 0) { throw 'End did not reveal a result beyond the first eight rows.' }
    [void][HypeTabsSmoke]::SendMessage($list, 0x0197, [IntPtr]2, [IntPtr]0)
    [void][HypeTabsSmoke]::SendMessage($window, 0x0111, [IntPtr](301 -bor (0x0300 -shl 16)), [IntPtr]0)
    if ([HypeTabsSmoke]::SendMessage($list, 0x018E, [IntPtr]0, [IntPtr]0).ToInt32() -ne 2 -or
        [HypeTabsSmoke]::SendMessage($list, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32() -ne 11) { throw 'An unchanged result refresh disturbed scrolling or selection.' }
    [void][HypeTabsSmoke]::SendMessage($window, 0x0111, [IntPtr](302 -bor (1 -shl 16)), $list)
    if (![HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Selection notification opened a result without a click or Enter.' }
    for ($id = 20; $id -lt 30; $id++) { Write-Frame $connections[0] @{v=1;type='remove';id=$id} }
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -eq 2) { break }
        Start-Sleep -Milliseconds 50
    }
    if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 2) { throw 'Scroll test results were not removed.' }
    foreach ($key in @(35, 34, 33, 36)) {
        [void][HypeTabsSmoke]::PostMessage($list, 0x0100, [IntPtr]$key, [IntPtr]0)
        Start-Sleep -Milliseconds 50
        if (![HypeTabsSmoke]::IsWindowVisible($window)) { throw 'A list navigation key activated a tab without Enter.' }
    }
    if ([HypeTabsSmoke]::SendMessage($list, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32() -ne 0) { throw 'Home did not return selection to the first result.' }
    [void][HypeTabsSmoke]::SendMessage($window, 0x0111, [IntPtr]103, [IntPtr]0)
    foreach ($connection in $connections) {
        $pausedReply = Read-Frame $connection
        if ($pausedReply.type -ne 'collect' -or $pausedReply.enabled) { throw 'Pause did not stop collection in both profiles.' }
    }
    $input = [HypeTabsSmoke]::GetDlgItem($window, 301)
    [void][HypeTabsSmoke]::PostMessage($input, 0x0100, [IntPtr]9, [IntPtr]0)
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ([HypeTabsSmoke]::FocusedControl($window) -eq $list) { break }
        Start-Sleep -Milliseconds 50
    }
    if ([HypeTabsSmoke]::FocusedControl($window) -ne $list) { throw 'Tab did not focus the results list.' }
    [void][HypeTabsSmoke]::PostMessage($list, 0x0100, [IntPtr]9, [IntPtr]0)
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ([HypeTabsSmoke]::FocusedControl($window) -eq $input) { break }
        Start-Sleep -Milliseconds 50
    }
    if ([HypeTabsSmoke]::FocusedControl($window) -ne $input) { throw 'Tab did not return focus to the search input.' }
    [void][HypeTabsSmoke]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0)
    $activation = Read-Activation $connections[1]
    if ($activation.type -ne 'activate' -or $activation.id -ne 1) { throw 'Activation was not routed to the selected profile.' }
    Write-Frame $connections[1] @{v=1;type='result';request=$activation.request;status='ok'}
    [void][HypeTabsSmoke]::SendMessage($window, 0x0111, [IntPtr]103, [IntPtr]0)
    foreach ($connection in $connections) {
        $resumedReply = Read-Frame $connection
        if ($resumedReply.type -ne 'collect' -or !$resumedReply.enabled) { throw 'Resume did not enable collection in both profiles.' }
    }
    Write-Output 'PASS: two profile snapshots, duplicate tab IDs, and profile-scoped activation while collection is paused.'
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 100
    $rowHeight = [HypeTabsSmoke]::SendMessage($list, 0x01A1, [IntPtr]0, [IntPtr]0).ToInt32()
    $point = [IntPtr](10 -bor (([int]($rowHeight / 2)) -shl 16))
    [void][HypeTabsSmoke]::PostMessage($list, 0x0201, [IntPtr]1, $point)
    [void][HypeTabsSmoke]::PostMessage($list, 0x0202, [IntPtr]0, $point)
    $mouseActivation = Read-Activation $connections[1]
    if ($mouseActivation.type -ne 'activate' -or $mouseActivation.id -ne 1) { throw 'Clicking a result did not activate the intended tab.' }
    Write-Frame $connections[1] @{v=1;type='result';request=$mouseActivation.request;status='focus'}
    # Exercise the shell's notification-click callback without clicking an
    # unrelated desktop notification. It must open search, never toggle it shut.
    Start-Sleep -Milliseconds 150
    [void][HypeTabsSmoke]::PostMessage($window, 0x8001, [IntPtr]1, [IntPtr]0x0405)
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ([HypeTabsSmoke]::IsWindowVisible($window)) { break }
        Start-Sleep -Milliseconds 100
    }
    if (![HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Notification click did not reopen search after foreground failure.' }
    [void][HypeTabsSmoke]::SendMessage($window, 0x8001, [IntPtr]1, [IntPtr]0x0405)
    if (![HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Notification click hid an already visible search.' }
    [void][HypeTabsSmoke]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0)
    $retryActivation = Read-Activation $connections[1]
    if ($retryActivation.type -ne 'activate' -or $retryActivation.id -ne 1 -or $retryActivation.request -eq $mouseActivation.request) { throw 'Explicit retry did not revalidate and activate the selected profile tab.' }
    Write-Frame $connections[1] @{v=1;type='result';request=$retryActivation.request;status='ok'}
    Write-Output 'PASS: notification recovery opens search and explicit retry routes to the selected profile.'
    Write-Output 'PASS: keyboard focus, scrolling beyond eight rows, selection without activation, and mouse activation.'
    [void][HypeTabsSmoke]::PostMessage($window, 0x0111, [IntPtr]102, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    $options = [HypeTabsSmoke]::FindOptionsWindow()
    $profilesList = [HypeTabsSmoke]::GetDlgItem($options, 206)
    if ([HypeTabsSmoke]::SendMessage($profilesList, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 2) { throw 'Options did not list both profiles.' }
    if ([HypeTabsSmoke]::IsWindowEnabled([HypeTabsSmoke]::GetDlgItem($options, 210))) { throw 'Isolated smoke mode must disable real sign-in changes.' }
    [void][HypeTabsSmoke]::SetControlText([HypeTabsSmoke]::GetDlgItem($options, 207), 0x000C, [IntPtr]0, 'Office')
    [void][HypeTabsSmoke]::SendMessage($options, 0x0111, [IntPtr]208, [IntPtr]0)
    $rename = Read-Frame $connections[0]
    if ($rename.type -ne 'setLabel' -or $rename.label -ne 'Office') { throw ('Rename did not reach the selected profile: ' + ($rename | ConvertTo-Json -Compress)) }
    Write-Frame $connections[0] @{v=1;type='label';label='Office'}
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ([HypeTabsSmoke]::ListText($profilesList, 0).Contains('Office')) { break }
        Start-Sleep -Milliseconds 50
    }
    if (![HypeTabsSmoke]::ListText($profilesList, 0).Contains('Office')) { throw 'Acknowledged name was not shown in Options.' }
    [void][HypeTabsSmoke]::SendMessage($options, 0x0010, [IntPtr]0, [IntPtr]0)
    if ($app.HasExited) { throw 'Closing Options stopped the tray app.' }
    Write-Output 'PASS: connected profiles, profile-scoped renaming, and Options lifecycle.'

    $closedAt = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    Write-Frame $connections[1] @{v=1;type='close';id=1;closed=$closedAt}
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ([HypeTabsSmoke]::ListText($list, 1).Contains('Closed')) { break }
        Start-Sleep -Milliseconds 50
    }
    if (![HypeTabsSmoke]::ListText($list, 1).Contains('Closed')) { throw 'Closed tab was not searchable.' }
    [void][HypeTabsSmoke]::SendMessage($list, 0x0186, [IntPtr]1, [IntPtr]0)
    [void][HypeTabsSmoke]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0)
    $restore = Read-Frame $connections[1]
    if ($restore.type -ne 'restore' -or $restore.url -ne 'https://example.test/') { throw 'Restoration was not routed to the original profile.' }
    [void][HypeTabsSmoke]::SendMessage($window, 0x0113, [IntPtr]2, [IntPtr]0)
    Write-Frame $connections[1] @{v=1;type='upsert';id=2;window=10;title='Restored test tab';url='https://example.test/';used=$closedAt;incognito=$false}
    Write-Frame $connections[1] @{v=1;type='result';request=$restore.request;status='restored'}
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 2) { throw 'Consumed closed entry remained after restoration.' }
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Write-Frame $connections[1] @{v=1;type='close';id=2;closed=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())}
    Write-Output 'PASS: closed-tab search, same-profile restoration, and consumed-entry removal.'

    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 200

    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    if ([HypeTabsSmoke]::IsWindowVisible($window)) { throw 'Repeated shortcut did not hide overlay.' }
    $app.Refresh()
    Write-Output "PASS: hidden startup, Idle priority, singleton, overlay toggle. Private bytes: $($app.PrivateMemorySize64)."
    [void][HypeTabsSmoke]::SendMessage($window, 0x0111, [IntPtr]102, [IntPtr]0)
    $options = [HypeTabsSmoke]::FindOptionsWindow()
    $motion = [HypeTabsSmoke]::GetDlgItem($options, 220)
    if ($motion -eq [IntPtr]::Zero) { throw 'Reduced-motion option is missing.' }
    if ([HypeTabsSmoke]::SendMessage($motion, 0x00F0, [IntPtr]0, [IntPtr]0).ToInt32() -ne 0) { throw 'Unexpected initial outline preference.' }
    [void][HypeTabsSmoke]::SendMessage($motion, 0x00F1, [IntPtr]1, [IntPtr]0)
    [void][HypeTabsSmoke]::SendMessage($options, 0x0111, [IntPtr]202, [IntPtr]0)
    if ([HypeTabsSmoke]::FindOptionsWindow() -ne [IntPtr]::Zero) { throw 'Saving reduced-motion preference did not close Options.' }
    [void][HypeTabsSmoke]::PostMessage($window, 0x0111, [IntPtr]104, [IntPtr]0)
    if (!$app.WaitForExit(3000)) { throw 'Exit command did not stop host.' }
    Write-Output 'PASS: clean exit.'
    Write-Output ('Preparation fallback paths exercised: ' + $script:preparations)
    foreach ($connection in $connections) { $connection.Dispose() }; $connections = @()
    $historyFile = Join-Path $smokeDirectory 'data\closed.dat'
    if (!(Test-Path -LiteralPath $historyFile)) { throw 'Closed history was not saved on exit.' }
    if ([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($historyFile)).Contains('example.test')) { throw 'Unencrypted history on disk.' }
    $app = Start-Process -FilePath $exe -ArgumentList @('--data-dir', ('"' + (Join-Path $smokeDirectory 'data') + '"')) -PassThru -NoNewWindow
    Write-Output 'CHECK: restarted application process launched.'
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $window = [HypeTabsSmoke]::FindSearchWindow(); if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 50
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Restart did not create a search window.' }
    Write-Output 'CHECK: restarted search window found.'
    [void][HypeTabsSmoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    $list = [HypeTabsSmoke]::GetDlgItem($window, 302)
    if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 1) { throw 'Restart did not retain only the closed tab.' }
    [void][HypeTabsSmoke]::PostMessage($window, 0x0111, [IntPtr]102, [IntPtr]0)
    Start-Sleep -Milliseconds 200
    $options = [HypeTabsSmoke]::FindOptionsWindow()
    if ($options -eq [IntPtr]::Zero) { throw 'Options did not open.' }
    if ([HypeTabsSmoke]::SendMessage([HypeTabsSmoke]::GetDlgItem($options, 220), 0x00F0, [IntPtr]0, [IntPtr]0).ToInt32() -ne 1) { throw 'Reduced-motion preference did not survive restart.' }
    Write-Output 'PASS: reduced-motion option saves and survives restart.'
    [void][HypeTabsSmoke]::PostMessage($options, 0x0111, [IntPtr]205, [IntPtr]0)
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if (!(Test-Path -LiteralPath $historyFile)) { break }
        Start-Sleep -Milliseconds 50
    }
    if (Test-Path -LiteralPath $historyFile) { throw 'Clear saved data left a history file.' }
    if ([HypeTabsSmoke]::SendMessage($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() -ne 0) { throw 'Clear saved data left searchable metadata.' }
    [void][HypeTabsSmoke]::PostMessage($window, 0x0111, [IntPtr]104, [IntPtr]0)
    if (!$app.WaitForExit(3000)) { throw 'Restarted instance did not exit.' }
    if (Test-Path -LiteralPath $historyFile) { throw 'Exit recreated cleared history.' }
    Write-Output 'PASS: encrypted persistence across restart and Clear saved data without resurrection.'

} finally {
    foreach ($connection in $connections) { $connection.Dispose() }
    if (!$app.HasExited) { $app.Kill(); $app.WaitForExit() }
    Remove-Item -LiteralPath $exe
    if (Test-Path (Join-Path $smokeDirectory 'data')) { Remove-Item -LiteralPath (Join-Path $smokeDirectory 'data') -Recurse }
    Remove-Item -LiteralPath $smokeDirectory
}
