$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class HypeTabsChromeProbe {
    public delegate bool EnumProc(IntPtr window, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder title, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr window,EnumProc callback,IntPtr param);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr window,uint message,IntPtr w,string value,uint flags,uint timeout,out IntPtr result);
    public static bool SelectDirectory(IntPtr dialog,string path) {
        var edit=GetDlgItem(dialog,1152);var button=GetDlgItem(dialog,1);var cls=new StringBuilder(64);GetClassName(edit,cls,64);
        if(edit==IntPtr.Zero || button==IntPtr.Zero || cls.ToString()!="Edit" || !IsWindowVisible(edit) || !IsWindowEnabled(edit) || !IsWindowEnabled(button)) return false;
        IntPtr result;if(SetText(edit,0xC,IntPtr.Zero,path,3,2000,out result)==IntPtr.Zero || result==IntPtr.Zero)return false;
        var text=new StringBuilder(32768);GetWindowText(edit,text,text.Capacity);
        return text.ToString()==path && PostMessage(dialog,0x111,(IntPtr)1,IntPtr.Zero);
    }
    public static void DescribeChildren(IntPtr dialog) { EnumChildWindows(dialog,(window,param)=>{var cls=new StringBuilder(64);GetClassName(window,cls,64);Console.WriteLine("Picker child class="+cls+" id="+GetDlgCtrlID(window));return true;},IntPtr.Zero); }
    public static void DescribeOwned(uint process, IntPtr browserWindow) {
        EnumWindows((window,param)=> { uint owner;GetWindowThreadProcessId(window,out owner);
            if(owner==process || GetWindow(window,4)==browserWindow) {var cls=new StringBuilder(64);GetClassName(window,cls,64);Console.WriteLine("Owned window class="+cls+" visible="+IsWindowVisible(window));}return true;},IntPtr.Zero);
    }
    public static IntPtr FindOwnedDialog(uint process, IntPtr browserWindow) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, param) => { uint owner; GetWindowThreadProcessId(window, out owner);
            var cls = new StringBuilder(64); GetClassName(window,cls,64);
            bool owned = owner == process; var parent = GetWindow(window,4); for(int depth=0;parent!=IntPtr.Zero && depth<8;depth++){ if(parent==browserWindow) owned=true; parent=GetWindow(parent,4); }
            if (owned && IsWindowVisible(window) && cls.ToString() == "#32770") { found = window; return false; } return true; }, IntPtr.Zero);
        return found;
    }
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
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$prior = [HypeTabsChromeProbe]::GetForegroundWindow()
$page = 'chrome://extensions/'
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
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($window)
    $addressCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'Address and search bar')
    $address = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $addressCondition)
    if (!$address) { throw 'No address control found in the isolated Chrome window.' }
    $address.SetFocus()
    $value = $address.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $value.SetValue($page)
    if ([HypeTabsChromeProbe]::GetForegroundWindow() -ne $window) { throw 'Focus left the isolated Chrome window; navigation cancelled.' }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    for ($ready = 0; $ready -lt 40; $ready++) {
        $elements = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
        $developer = @($elements | Where-Object { $_.Current.Name -eq 'Developer mode' })
        if ($developer.Count -gt 0) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!$developer.Count) { throw 'The isolated Extensions page did not expose Developer mode.' }
    $button = @($developer | Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button })[0]
    $button.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
    $loadCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'Load unpacked')
    $load = $null
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $load = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $loadCondition)
        if ($load -and $load.Current.IsEnabled -and !$load.Current.IsOffscreen) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!$load -or !$load.Current.IsEnabled -or $load.Current.IsOffscreen) { throw 'Developer mode did not expose a ready Load unpacked control.' }
    $extension = Join-Path $directory 'extension'
    [void](New-Item -ItemType Directory -Path $extension)
    foreach ($file in @('manifest.json', 'worker.js')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('..\build\extension\' + $file)) -Destination (Join-Path $extension $file)
    }
    $load.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    $dialog = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $dialog = [HypeTabsChromeProbe]::FindOwnedDialog([uint32]$browser.Id, $window)
        if ($dialog -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($dialog -eq [IntPtr]::Zero) { [HypeTabsChromeProbe]::DescribeOwned([uint32]$browser.Id, $window); throw 'Load unpacked did not open an owned folder dialog.' }
    $picker = [System.Windows.Automation.AutomationElement]::FromHandle($dialog)
    [void][HypeTabsChromeProbe]::SetForegroundWindow($dialog)
    Start-Sleep -Milliseconds 100
    if (![HypeTabsChromeProbe]::SelectDirectory($dialog, $extension)) {
        [HypeTabsChromeProbe]::DescribeChildren($dialog)
        throw 'The owned picker did not accept the exact extension directory.'
    }
    $cardCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'HypeTabs')
    $card = $null
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $card = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cardCondition)
        if ($card) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!$card) { throw 'Chrome did not show the loaded HypeTabs extension.' }
    Write-Output 'PASS: built HypeTabs extension loaded through Chrome UI in an isolated temporary profile; no native host registration changed.' 
    $result = 0
} finally {
    if ($window -ne [IntPtr]::Zero) { [void][HypeTabsChromeProbe]::PostMessage($window, 0x0010, [IntPtr]0, [IntPtr]0) }
    if (!$browser.WaitForExit(5000)) { Stop-Process -Id $browser.Id -Force; $browser.WaitForExit() }
    [void][HypeTabsChromeProbe]::SetForegroundWindow($prior)
    try { Remove-Item -LiteralPath $directory -Recurse -Force } catch { Write-Output ('Temporary probe cleanup pending: ' + $directory) }
}
exit $result
