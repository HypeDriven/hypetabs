param([switch]$Native, [switch]$WindowBounds, [switch]$WasmBenchmark, [switch]$PrepareProbe, [switch]$TaskbarFlow, [ValidateSet('normal','minimized','maximized')][string]$WindowState = 'normal', [ValidateSet('normal','pinned','grouped','collapsed')][string]$TabLayout = 'normal', [switch]$ExtraWindow, [ValidateSet('primary','secondary')][string]$Monitor = 'primary', [switch]$Incognito, [switch]$ReducedMotion)
$ErrorActionPreference = 'Stop'
if ($TaskbarFlow -and (!$Native -or !$WindowBounds)) { throw 'TaskbarFlow requires Native and WindowBounds.' }
if ($TaskbarFlow -and !(Test-Path -LiteralPath (Join-Path $PSScriptRoot '..\build\guidance_probe.exe'))) { throw 'Build tools\build-guidance-probe.cmd before TaskbarFlow.' }
if ($WindowState -ne 'normal' -and (!$Native -or !$WindowBounds)) { throw 'WindowState requires Native and WindowBounds.' }
# Guided mode now routes minimized targets through the taskbar cue, so plain Enter cannot bring them forward.
if ($WindowState -eq 'minimized' -and !$TaskbarFlow) { throw 'WindowState minimized requires TaskbarFlow.' }
if ($TabLayout -ne 'normal' -and (!$Native -or !$WindowBounds)) { throw 'TabLayout requires Native and WindowBounds.' }
if ($ExtraWindow -and (!$TaskbarFlow -or $WindowState -ne 'normal')) { throw 'ExtraWindow requires TaskbarFlow with a normal window state.' }
if ($Monitor -ne 'primary' -and (!$WindowBounds -or $ExtraWindow)) { throw 'Monitor secondary requires WindowBounds and is not combined with ExtraWindow.' }
# Fixture window origin in Chrome's DIP space; a secondary monitor offsets it into that display's work area (resolved from the worker).
$originX = 120; $originY = 140
if ($PrepareProbe -and !$Native) { throw 'PrepareProbe requires the isolated native connection.' }
if ($Incognito -and !$Native) { throw 'Incognito requires the isolated native connection.' }
if ($ReducedMotion -and !$TaskbarFlow) { throw 'ReducedMotion requires TaskbarFlow to check the cue shape.' }
$script:cdpRequestId = 0
function Invoke-TestWorker([string]$Expression) {
    $request = ++$script:cdpRequestId
    $cancel.CancelAfter(10000)
    $message = @{id=$request;method='Runtime.evaluate';params=@{expression=$Expression;returnByValue=$true;awaitPromise=$true}} | ConvertTo-Json -Depth 4 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($message)
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList (, $bytes)
    [void]$socket.SendAsync($segment, [Net.WebSockets.WebSocketMessageType]::Text, $true, $cancel.Token).GetAwaiter().GetResult()
    $buffer = New-Object byte[] 65536
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList (, $buffer)
    $stream = New-Object IO.MemoryStream
    try {
        do {
            $stream.SetLength(0)
            do {
                $received = $socket.ReceiveAsync($segment, $cancel.Token).GetAwaiter().GetResult()
                if ($received.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text -or $stream.Length + $received.Count -gt 65536) { throw 'Unexpected DevTools response.' }
                $stream.Write($buffer, 0, $received.Count)
            } while (!$received.EndOfMessage)
            $response = [Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json
        } while ($response.id -ne $request)
    } finally { $stream.Dispose() }
    if ($response.error -or $response.result.exceptionDetails) { throw 'Live worker evaluation failed.' }
    return $response.result.result.value
}
function Invoke-BrowserCdp([string]$Method, [hashtable]$Params) {
    # Browser-endpoint DevTools call, used only to create an incognito context the extension is not allowed into.
    $request = ++$script:cdpRequestId
    $cancel.CancelAfter(10000)
    $message = @{id=$request;method=$Method;params=$Params} | ConvertTo-Json -Depth 4 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($message)
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList (, $bytes)
    [void]$browserSocket.SendAsync($segment, [Net.WebSockets.WebSocketMessageType]::Text, $true, $cancel.Token).GetAwaiter().GetResult()
    $buffer = New-Object byte[] 65536
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList (, $buffer)
    $stream = New-Object IO.MemoryStream
    try {
        do {
            $stream.SetLength(0)
            do {
                $received = $browserSocket.ReceiveAsync($segment, $cancel.Token).GetAwaiter().GetResult()
                if ($received.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text -or $stream.Length + $received.Count -gt 65536) { throw 'Unexpected DevTools response.' }
                $stream.Write($buffer, 0, $received.Count)
            } while (!$received.EndOfMessage)
            $response = [Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json
        } while ($response.id -ne $request)
    } finally { $stream.Dispose() }
    if ($response.error) { throw ('Browser DevTools call failed: ' + $Method) }
    return $response.result
}
$nativeKey = 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.hypetabs.bridge'
$settingsKey = 'HKCU:\Software\HypeTabs'
$registered = $false; $originWritten = $false; $app = $null; $appWindow = [IntPtr]::Zero
$site = $null
$priorWindow = [IntPtr]::Zero
if ($Native -or $WindowBounds) {
    . (Join-Path $PSScriptRoot 'native-probe-ui.ps1')
    $priorWindow = [HypeTabsNativeProbe]::GetForegroundWindow()
}
if ($Native) {
    if ((Test-Path -LiteralPath $nativeKey) -or ((Test-Path -LiteralPath $settingsKey) -and (Get-Item -LiteralPath $settingsKey).GetValueNames() -contains 'ExtensionOrigin')) {
        throw 'Existing HypeTabs registration found; this isolated test refuses to overwrite it.'
    }
    if (@(Get-Process HypeTabs,HypeTabs.Bridge -ErrorAction SilentlyContinue).Count) { throw 'Exit existing HypeTabs processes before the isolated native test.' }
}
$chrome = Join-Path ([IO.Path]::GetTempPath()) 'HypeTabs-CfT-153.0.8010.36\chrome-win64\chrome.exe'
if (!(Test-Path -LiteralPath $chrome)) { throw 'Run tools\prepare-chrome-test.ps1 first.' }
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-cft-probe-' + [guid]::NewGuid().ToString('N'))
$extension = Join-Path $directory 'extension'
$profile = Join-Path $directory 'profile'
[void](New-Item -ItemType Directory -Path $extension -Force)
foreach ($file in @('manifest.json', 'worker.js')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('..\build\extension\' + $file)) -Destination (Join-Path $extension $file)
}
if ($WasmBenchmark) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\wasm-experiment\url.wasm') -Destination (Join-Path $extension 'url.wasm')
    $testManifestPath = Join-Path $extension 'manifest.json'
    $testManifest = Get-Content -LiteralPath $testManifestPath -Raw | ConvertFrom-Json
    $testManifest | Add-Member -NotePropertyName content_security_policy -NotePropertyValue @{extension_pages="script-src 'self' 'wasm-unsafe-eval'; object-src 'self';"}
    [IO.File]::WriteAllText($testManifestPath, ($testManifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    $benchmarkSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'benchmark-wasm-worker.js'))
    [IO.File]::AppendAllText((Join-Path $extension 'worker.js'), ("`n" + $benchmarkSource), (New-Object Text.UTF8Encoding($false)))
}
if ($TabLayout -eq 'grouped' -or $TabLayout -eq 'collapsed') {
    # Test-only permission so the harness can title/collapse the group; production keeps minimal permissions.
    $testManifestPath = Join-Path $extension 'manifest.json'
    $testManifest = Get-Content -LiteralPath $testManifestPath -Raw | ConvertFrom-Json
    $testManifest.permissions = @($testManifest.permissions) + 'tabGroups'
    [IO.File]::WriteAllText($testManifestPath, ($testManifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
}
$browser = $null
$socket = $null
$extraBrowser = $null; $extraSocket = $null; $firstSocket = $null; $browserSocket = $null
$cancel = New-Object Threading.CancellationTokenSource(30000)
try {
    # Both browsers are visible: Chrome for Testing 153 exits when an extension calls chrome.system.display.getInfo in --headless=new.
# Chrome's stdio goes to files: children that outlive Stop-Process (crash handler) must not hold this script's output handles open.
    $browser = Start-Process -FilePath $chrome -ArgumentList @(('--user-data-dir="' + $profile + '"'),
        ('--load-extension="' + $extension + '"'), ('--disable-extensions-except="' + $extension + '"'),
        '--window-size=900,600', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
        '--no-first-run', '--disable-background-networking', 'about:blank') -PassThru -NoNewWindow `
            -RedirectStandardOutput (Join-Path $directory 'chrome-stdout.log') -RedirectStandardError (Join-Path $directory 'chrome-stderr.log')
    $portFile = Join-Path $profile 'DevToolsActivePort'
    $target = $null
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        # Chrome relaunches itself (and lets the launcher exit) when started inside a job object, e.g. under WSL interop.
        if ($browser.HasExited -and !(Test-Path -LiteralPath $portFile) -and $attempt -gt 50) { throw 'Chrome for Testing exited before extension verification.' }
        if (Test-Path -LiteralPath $portFile) {
            $port = [int]([IO.File]::ReadAllLines($portFile)[0])
            if ($port -lt 1 -or $port -gt 65535) { throw 'Invalid temporary DevTools port.' }
            try {
                $targets = Invoke-RestMethod -Uri "http://127.0.0.1:$port/json/list" -TimeoutSec 2
                $target = @($targets | Where-Object { $_.type -eq 'service_worker' -and $_.url -match '^chrome-extension://[a-p]{32}/worker\.js$' })
                if ($target.Count -eq 1) { break }
            } catch { }
        }
        Start-Sleep -Milliseconds 100
    }
    if (@($target).Count -ne 1) { throw 'No unique extension service worker appeared.' }
    if ($browser.HasExited) {
        $owner = @(Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" | Where-Object { $_.CommandLine -like ('*' + $profile + '*') -and $_.CommandLine -notlike '*--type=*' })
        if ($owner.Count -ne 1) { throw 'Could not identify the relaunched Chrome browser process.' }
        $browser = Get-Process -Id $owner[0].ProcessId
        Write-Output ('Chrome relaunched itself; browser process ' + $browser.Id)
    }
    $socket = New-Object Net.WebSockets.ClientWebSocket
    [void]$socket.ConnectAsync([Uri]$target[0].webSocketDebuggerUrl, $cancel.Token).GetAwaiter().GetResult()
    $value = $null
    for ($attempt = 0; $attempt -lt 50 -and !$value; $attempt++) {
        # The worker target is listed slightly before its globals exist; retry briefly instead of failing on a ReferenceError.
        try { $value = Invoke-TestWorker '({name:chrome.runtime.getManifest().name,version:chrome.runtime.getManifest().version,id:chrome.runtime.id})' } catch { Start-Sleep -Milliseconds 100 }
    }
    if ($value.name -ne 'HypeTabs' -or $value.version -ne '0.1.0' -or $value.id -notmatch '^[a-p]{32}$') {
        throw 'The live extension worker did not return the expected manifest.'
    }
    Write-Output 'PASS: live Chrome for Testing loaded the built HypeTabs service worker and executed its Chrome API manifest query.'
    if ($WasmBenchmark) {
        $measurement = Invoke-TestWorker 'benchmarkWasmUrl()'
        $report = $measurement | ConvertTo-Json -Depth 8
        [IO.File]::WriteAllText((Join-Path $PSScriptRoot '..\build\wasm-chrome-benchmark.json'), $report, (New-Object Text.UTF8Encoding($false)))
        Write-Output $report
    }
    if ($WindowBounds) {
        if ($Monitor -eq 'secondary') {
            $secondaryDisplay = Invoke-TestWorker 'chrome.system.display.getInfo().then(ds=>{const d=ds.find(x=>!x.isPrimary);return d?{left:d.workArea.left,top:d.workArea.top,dpi:d.dpiX}:null})'
            if (!$secondaryDisplay) { throw 'No secondary display is attached.' }
            $originX = [int]$secondaryDisplay.left + 120; $originY = [int]$secondaryDisplay.top + 140
            Write-Output ('Secondary display (Chrome DIP work area origin): ' + ($secondaryDisplay | ConvertTo-Json -Compress))
        }
        $bounds = Invoke-TestWorker ('chrome.windows.getAll().then(ws=>Promise.all(ws.map(w=>chrome.windows.update(w.id,{left:' + $originX + ',top:' + $originY + ',width:900,height:600})))).then(ws=>ws.map(w=>({left:w.left,top:w.top,width:w.width,height:w.height,state:w.state})))')
        if (@($bounds)[0].left -ne $originX -or @($bounds)[0].top -ne $originY) { throw 'Chrome did not place the fixture window at the requested origin.' }
        Write-Output ('Chrome bounds: ' + ($bounds | ConvertTo-Json -Compress))
        $windowsBounds = [HypeTabsNativeProbe]::WindowBounds([uint32]$browser.Id)
        Write-Output $windowsBounds
    }
    if ($Native) {
        $snapshotTab = Invoke-TestWorker "chrome.tabs.create({url:'data:text/html;charset=utf-8,'+encodeURIComponent('<title>HypeTabs initial snapshot \u00c9COLE \u6771\u4eac</title>'),active:false}).then(t=>({id:t.id}))"
        # Ensure metadata has settled before registration or host startup, so
        # this check cannot succeed through a later title-change event alone.
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            $snapshotReady = Invoke-TestWorker ('chrome.tabs.get(' + [int]$snapshotTab.id + ').then(t=>t.title === "HypeTabs initial snapshot \u00c9COLE \u6771\u4eac" && t.status === "complete")')
            if ($snapshotReady -eq $true) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($snapshotReady -ne $true) { throw 'Initial snapshot fixture did not finish loading before host startup.' }
        foreach ($file in @('HypeTabs.exe', 'HypeTabs.Bridge.exe')) {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('..\build\' + $file)) -Destination (Join-Path $directory $file)
        }
        $origin = 'chrome-extension://' + $value.id + '/'
        $manifestPath = Join-Path $directory 'native-host.json'
        $manifest = @{name='com.hypetabs.bridge';description='HypeTabs isolated native test';type='stdio';path=(Join-Path $directory 'HypeTabs.Bridge.exe');allowed_origins=@($origin)} | ConvertTo-Json
        [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object Text.UTF8Encoding($false)))
        [void](New-Item -Path $nativeKey -Force); $registered = $true
        Set-Item -LiteralPath $nativeKey -Value $manifestPath
        [void](New-Item -Path $settingsKey -Force)
        [void](New-ItemProperty -LiteralPath $settingsKey -Name ExtensionOrigin -Value $origin -PropertyType String); $originWritten = $true
        if ($ReducedMotion) {
            # Version 4 settings record with the explicit outline preference; shortcut 0 keeps the default.
            [void](New-Item -ItemType Directory -Path (Join-Path $directory 'data') -Force)
            $record = [byte[]](0x32,0x50,0x54,0x48, 0,0, 7, 4, 0,0,0,0,0,0,0,0, 5,0,0,0, 1, 1, 0,0)
            [IO.File]::WriteAllBytes((Join-Path $directory 'data\settings.bin'), $record)
        }
        $app = Start-Process -FilePath (Join-Path $directory 'HypeTabs.exe') -ArgumentList @('--data-dir', ('"' + (Join-Path $directory 'data') + '"')) -PassThru -NoNewWindow `
            -RedirectStandardOutput (Join-Path $directory 'host-stdout.log') -RedirectStandardError (Join-Path $directory 'host-stderr.log')
        for ($attempt = 0; $attempt -lt 60; $attempt++) {
            $appWindow = [HypeTabsNativeProbe]::Find([uint32]$app.Id)
            if ($appWindow -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($appWindow -eq [IntPtr]::Zero) { throw 'The isolated native host did not create its search window.' }
        [void](Invoke-TestWorker "chrome.storage.local.set({label:'LiveAlpha'}).then(()=>connect()).then(()=>true)")
        if ($PrepareProbe) {
            $prepared = Invoke-TestWorker @'
(async () => {
    const foreground = await chrome.windows.getLastFocused();
    const background = await chrome.windows.create({url:'about:blank',focused:false});
    const originalSend = send;
    let reply;
    try {
        const tab = await chrome.tabs.create({windowId:background.id,url:'about:blank',active:false});
        await chrome.windows.update(foreground.id,{focused:true});
        const before = await chrome.windows.get(background.id);
        send = message => { if (message.request === 800001) reply = message; originalSend(message); };
        await command({v:1,type:'prepare',request:800001,id:tab.id},port);
        const after = await chrome.windows.get(background.id);
        const selected = await chrome.tabs.get(tab.id);
        return {beforeFocused:before.focused,afterFocused:after.focused,active:selected.active,
            response:reply?.type,connected:!!port};
    } finally {
        send = originalSend;
        await chrome.windows.remove(background.id);
    }
})()
'@
            if ($prepared.beforeFocused -ne $false -or $prepared.afterFocused -ne $false -or
                $prepared.active -ne $true -or $prepared.response -ne 'prepared' -or $prepared.connected -ne $true) {
                throw ('Background tab preparation failed: ' + ($prepared | ConvertTo-Json -Compress))
            }
            Write-Output 'PASS: live Chrome preparation selects a background-window tab without focusing the window.'
        }
        $created = Invoke-TestWorker "chrome.tabs.create({url:'data:text/html,%3Ctitle%3EHypeTabs%20live%20browser%20tab%3C/title%3E',active:false}).then(t=>({id:t.id,active:t.active}))"
        if ($created.active -ne $false -or $created.id -lt 0) { throw 'Synthetic Chrome tab was not created inactive.' }
        if ($TabLayout -eq 'pinned') {
            $layout = Invoke-TestWorker ('chrome.tabs.update(' + [int]$created.id + ',{pinned:true}).then(t=>({pinned:t.pinned,active:t.active}))')
            if ($layout.pinned -ne $true -or $layout.active -ne $false) { throw 'Chrome did not pin the inactive fixture tab.' }
        } elseif ($TabLayout -ne 'normal') {
            $collapse = if ($TabLayout -eq 'collapsed') { 'true' } else { 'false' }
            $layout = Invoke-TestWorker ('chrome.tabs.group({tabIds:[' + [int]$created.id + ']}).then(g=>chrome.tabGroups.update(g,{title:"HypeTabs group",color:"blue",collapsed:' + $collapse + '})).then(g=>chrome.tabs.get(' + [int]$created.id + ').then(t=>({group:t.groupId===g.id,collapsed:g.collapsed,active:t.active})))')
            if ($layout.group -ne $true -or $layout.active -ne $false -or [bool]$layout.collapsed -ne ($TabLayout -eq 'collapsed')) { throw 'Chrome did not place the inactive fixture tab in the requested tab group state.' }
        }
        if ($TabLayout -ne 'normal') { Write-Output ('Tab layout: ' + $TabLayout + ' ' + ($layout | ConvertTo-Json -Compress)) }
        if ($Incognito) {
            # An incognito page created outside the extension's reach must never appear in native search.
            $version = Invoke-RestMethod -Uri "http://127.0.0.1:$port/json/version" -TimeoutSec 2
            $browserSocket = New-Object Net.WebSockets.ClientWebSocket
            [void]$browserSocket.ConnectAsync([Uri]$version.webSocketDebuggerUrl, $cancel.Token).GetAwaiter().GetResult()
            $context = Invoke-BrowserCdp 'Target.createBrowserContext' @{}
            $incognitoTarget = Invoke-BrowserCdp 'Target.createTarget' @{url='data:text/html,%3Ctitle%3EHypeTabs%20incognito%20secret%3C/title%3E';browserContextId=$context.browserContextId;newWindow=$true}
            $incognitoSeen = $false
            for ($attempt = 0; $attempt -lt 50; $attempt++) {
                $targets = Invoke-BrowserCdp 'Target.getTargets' @{}
                $incognitoInfo = @($targets.targetInfos | Where-Object { $_.targetId -eq $incognitoTarget.targetId })
                if ($incognitoInfo.Count -eq 1 -and $incognitoInfo[0].title -eq 'HypeTabs incognito secret') { $incognitoSeen = $true; break }
                Start-Sleep -Milliseconds 100
            }
            if (!$incognitoSeen) { throw 'The incognito fixture page did not load.' }
            $extensionSees = Invoke-TestWorker "chrome.tabs.query({}).then(ts=>ts.some(t=>t.incognito||(t.title||'').includes('incognito secret')))"
            if ($extensionSees -ne $false) { throw 'The extension observed an incognito tab; the manifest must keep incognito disallowed.' }
            Write-Output 'Incognito fixture: loaded in a separate browser context; the extension does not observe it.'
        }
        if ($ExtraWindow) {
            # A second visible window in the same profile shares Chrome's taskbar identity, so the grouped button must not be used.
            $extra = Invoke-TestWorker "chrome.windows.create({url:'about:blank',focused:false,left:1050,top:140,width:700,height:500}).then(w=>({id:w.id,focused:w.focused,left:w.left,top:w.top,width:w.width,height:w.height}))"
            if ($extra.focused -ne $false) { throw ('Chrome did not create the unfocused extra window: ' + ($extra | ConvertTo-Json -Compress)) }
            if ($extra.width -ne 700 -or $extra.left -ne 1050) {
                $extra = Invoke-TestWorker ('chrome.windows.update(' + [int]$extra.id + ',{left:1050,top:140,width:700,height:500}).then(w=>({id:w.id,focused:w.focused,left:w.left,top:w.top,width:w.width,height:w.height}))')
                if ($extra.width -ne 700 -or $extra.left -ne 1050) { throw ('Chrome did not place the extra window: ' + ($extra | ConvertTo-Json -Compress)) }
            }
            Write-Output ('Extra window: ' + ($extra | ConvertTo-Json -Compress))
        }
        [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0)
        $input = [HypeTabsNativeProbe]::GetDlgItem($appWindow, 301)
        $list = [HypeTabsNativeProbe]::GetDlgItem($appWindow, 302)
        $snapshotQuery = 'HypeTabs initial snapshot ' + [char]0x00e9 + 'cole ' + [char]0x6771 + [char]0x4eac
        if (![HypeTabsNativeProbe]::Query($input, $snapshotQuery)) { throw 'Could not query the initial snapshot fixture.' }
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1) { throw 'The tab loaded before native registration was missing from the initial snapshot.' }
        $japaneseQuery = [string][char]0x6771 + [char]0x4eac
        if (![HypeTabsNativeProbe]::Query($input, $japaneseQuery) -or [HypeTabsNativeProbe]::Count($list) -ne 1) { throw 'Japanese-only input did not identify the fixture tab.' }
        $absentQuery = [string][char]0x5927 + [char]0x962a
        if (![HypeTabsNativeProbe]::Query($input, $absentQuery) -or [HypeTabsNativeProbe]::Count($list) -ne 0) { throw 'An absent Japanese term became an empty search or matched an unrelated tab.' }
        Write-Output 'PASS: a tab loaded before native registration appears in the initial snapshot and matches accented case-insensitive and Japanese query terms.'
        if ($Incognito) {
            Start-Sleep -Milliseconds 500
            if (![HypeTabsNativeProbe]::Query($input, 'incognito secret') -or [HypeTabsNativeProbe]::Count($list) -ne 0) { throw 'An incognito tab reached native search.' }
            [void](Invoke-BrowserCdp 'Target.closeTarget' @{targetId=$incognitoTarget.targetId})
            Start-Sleep -Milliseconds 500
            if (![HypeTabsNativeProbe]::Query($input, 'incognito secret') -or [HypeTabsNativeProbe]::Count($list) -ne 0) { throw 'A closed incognito tab reached native search as a recently closed entry.' }
            [void](Invoke-BrowserCdp 'Target.disposeBrowserContext' @{browserContextId=$context.browserContextId})
            Write-Output 'PASS: an actual incognito tab is absent from native search while open and after closing.'
        }
        if (![HypeTabsNativeProbe]::Query($input, 'HypeTabs live browser tab')) { throw 'Could not set the native search query.' }
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1) { throw 'The live Chrome tab did not reach the native search UI.' }
        if ($WindowState -ne 'normal') {
            $stateExpression = 'chrome.tabs.get(' + [int]$created.id + ').then(t=>chrome.windows.update(t.windowId,{state:"' + $WindowState + '"})).then(w=>({state:w.state,left:w.left,top:w.top,width:w.width,height:w.height}))'
            $changedWindow = Invoke-TestWorker $stateExpression
            if ($changedWindow.state -ne $WindowState) { throw 'Chrome did not enter the requested test window state.' }
            Write-Output ('Test window: ' + ($changedWindow | ConvertTo-Json -Compress))
            Write-Output ([HypeTabsNativeProbe]::WindowBounds([uint32]$browser.Id))
        }
        if ($TaskbarFlow) {
            # Hide search before the helper establishes its owned foreground fixture.
            if ([HypeTabsNativeProbe]::IsWindowVisible($appWindow)) { [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0); Start-Sleep -Milliseconds 100 }
            $guidanceProbe = Join-Path $directory 'guidance_probe.exe'
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\build\guidance_probe.exe') -Destination $guidanceProbe
            [void](Invoke-TestWorker "globalThis.guidanceTrace=[];globalThis.savedGuidanceCommand=command;globalThis.savedGuidanceSend=send;command=async(m,p)=>{guidanceTrace.length<128&&guidanceTrace.push({direction:'in',type:m.type});return savedGuidanceCommand(m,p)};send=m=>{if(guidanceTrace.length<128&&['prepared','located','result'].includes(m.type))guidanceTrace.push({direction:'out',type:m.type,status:m.status});savedGuidanceSend(m)};true")
            # Native stderr must not become a terminating error before the exit code is read.
            $probeFlags = @(); if ($ExtraWindow) { $probeFlags += 'fallback' }; if ($ReducedMotion) { $probeFlags += 'outline' }
            $guidanceOutput = & { $ErrorActionPreference = 'Continue'; & $guidanceProbe $app.Id $browser.Id @probeFlags 2>&1 }
            $guidanceExit = $LASTEXITCODE
            $guidanceOutput | ForEach-Object { Write-Output ([string]$_) }
            $guidanceTrace = Invoke-TestWorker 'command=savedGuidanceCommand;send=savedGuidanceSend;guidanceTrace'
            if ($guidanceExit -ne 0) {
                Write-Output ('Guidance protocol trace: ' + ($guidanceTrace | ConvertTo-Json -Compress))
                throw 'Physical taskbar guidance probe failed.'
            }
        } else { [void][HypeTabsNativeProbe]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0) }
        $active = $false
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            $active = Invoke-TestWorker ('chrome.tabs.get(' + [int]$created.id + ').then(t=>t.active)')
            if ($active -eq $true) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($active -ne $true) { throw 'Native search selection did not activate the actual Chrome tab.' }
        Write-Output 'PASS: real Chrome tab metadata crossed the production native bridge into search, and Enter activated the actual tab.'
        if ($WindowState -ne 'normal') {
            for ($attempt = 0; $attempt -lt 50; $attempt++) {
                $finalWindow = Invoke-TestWorker ('chrome.tabs.get(' + [int]$created.id + ').then(t=>chrome.windows.get(t.windowId)).then(w=>({state:w.state,focused:w.focused}))')
                if ($finalWindow.state -ne 'minimized' -and $finalWindow.focused -and [HypeTabsNativeProbe]::ForegroundChrome([uint32]$browser.Id)) { break }
                Start-Sleep -Milliseconds 100
            }
            if ($finalWindow.state -eq 'minimized' -or !$finalWindow.focused -or ![HypeTabsNativeProbe]::ForegroundChrome([uint32]$browser.Id)) { throw 'Activation did not bring the target Chrome window into view.' }
            if ($WindowState -eq 'maximized' -and $finalWindow.state -ne 'maximized') { throw 'Activation unexpectedly changed the maximized window state.' }
            Write-Output ('PASS: activation from ' + $WindowState + ' brings the target window into the Windows foreground; final state=' + $finalWindow.state)
        }

        if ($WindowBounds -and !$TaskbarFlow) {
            $cueSeen = $false
            for ($attempt = 0; $attempt -lt 30; $attempt++) {
                if ([HypeTabsNativeProbe]::CueVisible([uint32]$app.Id)) { $cueSeen = $true; break }
                Start-Sleep -Milliseconds 100
            }
            if (!$cueSeen) { throw 'No verified tab cue appeared on the visible test browser (requires an unambiguous window and a matched display layout).' }
            Write-Output 'PASS: the visible Chrome tab received a native cue after window bounds verification.'
        }
        $firstSocket = $socket
        $secondProfile = Join-Path $directory 'second-profile'
        $extraBrowser = Start-Process -FilePath $chrome -ArgumentList @(('--user-data-dir="' + $secondProfile + '"'),
            ('--load-extension="' + $extension + '"'), ('--disable-extensions-except="' + $extension + '"'),
            '--window-size=700,500', '--window-position=1050,140', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
            '--no-first-run', '--disable-background-networking', 'about:blank') -PassThru -NoNewWindow `
                -RedirectStandardOutput (Join-Path $directory 'chrome2-stdout.log') -RedirectStandardError (Join-Path $directory 'chrome2-stderr.log')
        $secondPortFile = Join-Path $secondProfile 'DevToolsActivePort'
        $secondTarget = @()
        for ($attempt = 0; $attempt -lt 300; $attempt++) {
            # Chrome may relaunch itself and let the launcher exit; the profile's DevTools port file is the reliable signal.
            if ($extraBrowser.HasExited -and !(Test-Path -LiteralPath $secondPortFile) -and $attempt -gt 50) { throw 'Second Chrome instance exited before verification.' }
            if (Test-Path -LiteralPath $secondPortFile) {
                $secondPort = [int]([IO.File]::ReadAllLines($secondPortFile)[0])
                if ($secondPort -lt 1 -or $secondPort -gt 65535) { throw 'Invalid second DevTools port.' }
                try {
                    $secondTargets = Invoke-RestMethod -Uri "http://127.0.0.1:$secondPort/json/list" -TimeoutSec 2
                    $secondTarget = @($secondTargets | Where-Object { $_.type -eq 'service_worker' -and $_.url -eq ('chrome-extension://' + $value.id + '/worker.js') })
                    if ($secondTarget.Count -eq 1) { break }
                } catch { }
            }
            Start-Sleep -Milliseconds 100
        }
        if ($secondTarget.Count -ne 1) {
            $listing = if ($secondTargets) { ($secondTargets | ForEach-Object { $_.type + ':' + $_.url }) -join ' | ' } else { 'no target list' }
            throw ('Second profile did not load the same extension. Targets: ' + $listing + ' exited=' + $extraBrowser.HasExited)
        }
        if ($extraBrowser.HasExited) {
            # Re-resolve the browser process that owns the second profile after a self-relaunch.
            $owner = @(Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" | Where-Object { $_.CommandLine -like ('*' + $secondProfile + '*') -and $_.CommandLine -notlike '*--type=*' })
            if ($owner.Count -ne 1) { throw 'Could not identify the relaunched second Chrome browser process.' }
            $extraBrowser = Get-Process -Id $owner[0].ProcessId
            Write-Output ('Second Chrome relaunched itself; browser process ' + $extraBrowser.Id)
        }
        $extraSocket = New-Object Net.WebSockets.ClientWebSocket
        $cancel.CancelAfter(10000)
        [void]$extraSocket.ConnectAsync([Uri]$secondTarget[0].webSocketDebuggerUrl, $cancel.Token).GetAwaiter().GetResult()
        $socket = $extraSocket
        # A worker target can appear before its asynchronous hello/collect handshake.
        # Do not inject a fixture label while connect() is still reading storage.
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            # The target can also be listed before its script has run; a ReferenceError here is not a failure yet.
            try { $ready = Invoke-TestWorker 'Boolean(port && collecting)' } catch { $ready = $false }
            if ($ready -eq $true) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($ready -ne $true) { throw 'Second profile did not complete the native collection handshake.' }
        [void](Invoke-TestWorker "command({v:1,type:'setLabel',label:'LiveBeta'},port).then(()=>true)")
        $secondTab = Invoke-TestWorker "chrome.tabs.create({url:'data:text/html,%3Ctitle%3EHypeTabs%20live%20browser%20tab%3C/title%3E',active:false}).then(t=>({id:t.id,active:t.active}))"
        if ($secondTab.active -ne $false -or $secondTab.id -lt 0) { throw 'Second synthetic tab was not created inactive.' }
        [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0)
        if (![HypeTabsNativeProbe]::Query($input, 'HypeTabs live browser tab')) { throw 'Could not search duplicate titles.' }
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 2) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 2) { throw 'Duplicate real Chrome titles were not retained as separate results.' }
        if (![HypeTabsNativeProbe]::Query($input, 'HypeTabs live browser tab LiveBeta')) { throw 'Could not filter by the second profile.' }
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1) { throw 'Profile filtering did not identify the second tab.' }
        [void][HypeTabsNativeProbe]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0)
        $active = $false
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            $active = Invoke-TestWorker ('chrome.tabs.get(' + [int]$secondTab.id + ').then(t=>t.active)')
            if ($active -eq $true) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($active -ne $true) { throw 'Native search did not route activation to the second Chrome profile.' }
        Write-Output 'PASS: two live Chrome profiles preserve duplicate titles and route native search activation to the selected profile.'
        $site = New-Object HypeTabsTestSite
        $siteUrl = 'http://127.0.0.1:' + $site.Port + '/restore'
        $closedTab = Invoke-TestWorker ("chrome.tabs.create({url:'" + $siteUrl + "',active:false}).then(t=>t.id)")
        [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0)
        if (![HypeTabsNativeProbe]::Query($input, 'HypeTabs live restoration LiveBeta')) { throw 'Could not search the restoration fixture.' }
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1) { throw 'The restoration fixture was not indexed while open.' }
        [void](Invoke-TestWorker ('chrome.tabs.remove(' + [int]$closedTab + ').then(()=>true)'))
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1 -and [HypeTabsNativeProbe]::FirstRowClosed($list)) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1 -or ![HypeTabsNativeProbe]::FirstRowClosed($list)) { throw 'The actual Chrome closure did not become one closed search result.' }
        [void][HypeTabsNativeProbe]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0)
        $restored = @()
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            $restored = @(Invoke-TestWorker ("chrome.tabs.query({}).then(tabs=>tabs.filter(t=>t.url==='" + $siteUrl + "').map(t=>({id:t.id,active:t.active})))"))
            if ($restored.Count -eq 1 -and $restored[0].active) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($restored.Count -ne 1 -or !$restored[0].active) { throw 'Native selection did not reopen exactly one tab in the original profile.' }
        $socket = $firstSocket
        $wrongProfileCount = Invoke-TestWorker ("chrome.tabs.query({}).then(tabs=>tabs.filter(t=>t.url==='" + $siteUrl + "').length)")
        if ($wrongProfileCount -ne 0) { throw 'Restoration also opened a tab in the wrong profile.' }
        [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0)
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1 -and ![HypeTabsNativeProbe]::FirstRowClosed($list)) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1 -or [HypeTabsNativeProbe]::FirstRowClosed($list)) { throw 'Restoration left a duplicate closed result.' }
        Write-Output 'PASS: a real closed Chrome tab restores once in its original profile and becomes an open result without a retained duplicate.'
        # Stopped profile: close the restored tab again, then exit that whole profile. The closed entry must stay searchable,
        # selection must explain the limitation, and nothing may open in the other running profile.
        $socket = $extraSocket
        [void](Invoke-TestWorker ('chrome.tabs.remove(' + [int]$restored[0].id + ').then(()=>true)'))
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1 -and [HypeTabsNativeProbe]::FirstRowClosed($list)) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1 -or ![HypeTabsNativeProbe]::FirstRowClosed($list)) { throw 'The second closure did not become a closed search result.' }
        Stop-Process -Id $extraBrowser.Id -Force; $extraBrowser.WaitForExit(5000) | Out-Null
        $extraSocket.Dispose(); $extraSocket = $null
        for ($attempt = 0; $attempt -lt 100; $attempt++) {
            if (@(Get-Process HypeTabs.Bridge -ErrorAction SilentlyContinue).Count -le 1) { break }
            Start-Sleep -Milliseconds 100
        }
        Write-Output ('Bridges after profile exit: ' + @(Get-Process HypeTabs.Bridge -ErrorAction SilentlyContinue).Count)
        Start-Sleep -Milliseconds 1500
        if (![HypeTabsNativeProbe]::IsWindowVisible($appWindow)) { [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0312, [IntPtr]1, [IntPtr]0); Start-Sleep -Milliseconds 200 }
        if (![HypeTabsNativeProbe]::Query($input, 'HypeTabs live restoration LiveBeta')) { throw 'Could not search after the profile stopped.' }
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            if ([HypeTabsNativeProbe]::Count($list) -eq 1 -and [HypeTabsNativeProbe]::FirstRowClosed($list)) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([HypeTabsNativeProbe]::Count($list) -ne 1 -or ![HypeTabsNativeProbe]::FirstRowClosed($list)) { throw 'The stopped profile closed entry did not remain searchable.' }
        [void][HypeTabsNativeProbe]::PostMessage($input, 0x0100, [IntPtr]13, [IntPtr]0); Start-Sleep -Milliseconds 500
        $status = [HypeTabsNativeProbe]::Text([HypeTabsNativeProbe]::GetDlgItem($appWindow, 303))
        if ($status -ne 'Open this Chrome profile and reconnect its extension, then try again.') { throw ('Stopped-profile selection did not explain the limitation: [' + $status + '] visible=' + [HypeTabsNativeProbe]::IsWindowVisible($appWindow) + ' rows=' + [HypeTabsNativeProbe]::Count($list)) }
        if (![HypeTabsNativeProbe]::IsWindowVisible($appWindow)) { throw 'Search closed instead of keeping the explanation visible.' }
        $socket = $firstSocket
        Start-Sleep -Milliseconds 1000
        $wrongProfileCount = Invoke-TestWorker ("chrome.tabs.query({}).then(tabs=>tabs.filter(t=>t.url==='" + $siteUrl + "').length)")
        if ($wrongProfileCount -ne 0) { throw 'A stopped-profile tab was reopened in a different running profile.' }
        Write-Output 'PASS: a closed tab from a stopped profile stays searchable, selection explains how to retry, and no other profile opens it.'
    }
} finally {
    foreach ($connection in @($socket, $firstSocket, $extraSocket, $browserSocket)) { if ($connection) { $connection.Dispose() } }; $cancel.Dispose()
    foreach ($instance in @($browser, $extraBrowser)) {
        if ($instance -and !$instance.HasExited) { Stop-Process -Id $instance.Id -Force; $instance.WaitForExit(5000) | Out-Null }
    }
    if ($site) { $site.Dispose() }
    if ($priorWindow -ne [IntPtr]::Zero) { [void][HypeTabsNativeProbe]::SetForegroundWindow($priorWindow) }
    if ($app -and !$app.HasExited) {
        if ($appWindow -ne [IntPtr]::Zero) { [void][HypeTabsNativeProbe]::PostMessage($appWindow, 0x0111, [IntPtr]104, [IntPtr]0) }
        if (!$app.WaitForExit(5000)) { Stop-Process -Id $app.Id -Force; $app.WaitForExit(5000) | Out-Null }
    }
    if ($registered -and (Test-Path -LiteralPath $nativeKey) -and (Get-Item -LiteralPath $nativeKey).GetValue('') -eq $manifestPath) { Remove-Item -LiteralPath $nativeKey }
    if ($originWritten -and (Get-ItemProperty -LiteralPath $settingsKey -Name ExtensionOrigin -ErrorAction SilentlyContinue).ExtensionOrigin -eq $origin) {
        Remove-ItemProperty -LiteralPath $settingsKey -Name ExtensionOrigin
        $key = Get-Item -LiteralPath $settingsKey
        if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) { Remove-Item -LiteralPath $settingsKey }
    }
    try { Remove-Item -LiteralPath $directory -Recurse -Force } catch { Write-Output ('Temporary probe cleanup pending: ' + $directory) }
    Write-Output 'Harness finished.'
}
