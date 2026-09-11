param([switch]$LatencyOnly, [switch]$VariedQueries, [string]$Executable = (Join-Path $PSScriptRoot '..\build\HypeTabs.exe'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native-probe-ui.ps1')
if (@(Get-Process HypeTabs,HypeTabs.Bridge -ErrorAction SilentlyContinue).Count) { throw 'Exit existing HypeTabs processes before this isolated measurement.' }
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HypeTabs-measure-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $directory)
$exe = Join-Path $directory 'HypeTabs.exe'
Copy-Item -LiteralPath $Executable -Destination $exe
$app = $null; $window = [IntPtr]::Zero; $connections = @()
$priorWindow = [HypeTabsNativeProbe]::GetForegroundWindow()
function Write-Frame($pipe, $value) {
    $data = [Text.Encoding]::UTF8.GetBytes(($value | ConvertTo-Json -Compress))
    $frame = New-Object byte[] ($data.Length + 4)
    [BitConverter]::GetBytes([uint32]$data.Length).CopyTo($frame, 0); $data.CopyTo($frame, 4)
    $task = $pipe.WriteAsync($frame, 0, $frame.Length)
    if (!$task.Wait(5000)) { $pipe.Dispose(); throw 'Timed out feeding synthetic metadata.' }
}
function Read-Exact($pipe, [int]$length) {
    $data = New-Object byte[] $length; $offset = 0
    while ($offset -lt $length) {
        $task = $pipe.ReadAsync($data, $offset, $length-$offset)
        if (!$task.Wait(5000)) { $pipe.Dispose(); throw 'Timed out reading collection handshake.' }
        if ($task.Result -eq 0) { throw 'Native connection closed.' }
        $offset += $task.Result
    }
    return ,$data
}
function Ui-Message([uint32]$message, [int]$parameter = 0) {
    $result = [IntPtr]::Zero
    if ([HypeTabsNativeProbe]::Send($window, $message, [IntPtr]$parameter, [IntPtr]0, 2, 3000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Native UI message failed or timed out.' }
}
function Private-WorkingSet {
    $sample = @(Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter ('IDProcess=' + $app.Id) -OperationTimeoutSec 10)
    if ($sample.Count -ne 1) { throw 'No unique private-working-set counter for the test process.' }
    return [uint64]$sample[0].WorkingSetPrivate
}
function Percentile95($values) { return @($values | Sort-Object)[[int][Math]::Ceiling($values.Count * 0.95)-1] }
try {
    $app = Start-Process -FilePath $exe -ArgumentList @('--data-dir', ('"'+(Join-Path $directory 'data')+'"')) -PassThru
    for ($attempt=0; $attempt -lt 100; $attempt++) {
        $window = [HypeTabsNativeProbe]::Find([uint32]$app.Id)
        if ($window -ne [IntPtr]::Zero) { break }
        if ($app.HasExited) { throw 'Test host exited during startup.' }
        Start-Sleep -Milliseconds 50
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Test host did not create its UI.' }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $stamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $labels = @('Work', 'Personal', 'Research', 'Travel', 'Family')
    for ($profile=0; $profile -lt 5; $profile++) {
        $pipe = New-Object IO.Pipes.NamedPipeClientStream('.', ('HypeTabs.v1.'+$sid), [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
        $pipe.Connect(5000); $connections += $pipe
        Write-Frame $pipe @{v=1;type='hello';profile=('00000000-0000-0000-0000-00000000000'+$profile);label=$labels[$profile]}
        $size = [BitConverter]::ToUInt32((Read-Exact $pipe 4), 0)
        if ($size -eq 0 -or $size -gt 65536) { throw 'Invalid collection reply.' }
        $reply = [Text.Encoding]::UTF8.GetString((Read-Exact $pipe $size)) | ConvertFrom-Json
        if ($reply.type -ne 'collect' -or !$reply.enabled) { throw 'Expected collection handshake.' }
        Write-Frame $pipe @{v=1;type='begin'}
        for ($tab=0; $tab -lt 200; $tab++) {
            Write-Frame $pipe @{v=1;type='upsert';id=$tab;window=($profile*2+$tab%2);title=('Chrome documentation '+$tab);url=('https://example.test/open/'+$profile+'/'+$tab);used=($stamp-$tab);incognito=$false}
        }
        Write-Frame $pipe @{v=1;type='end'}
        for ($tab=0; $tab -lt 200; $tab++) {
            Write-Frame $pipe @{v=1;type='recent';session=('closed-'+$tab);title=('Retained documentation '+$tab);url=('https://example.test/closed/'+$profile+'/'+$tab);closed=($stamp-$tab-1000);incognito=$false}
        }
    }
    Ui-Message 0x0312 1
    $input = [HypeTabsNativeProbe]::GetDlgItem($window, 301)
    $list = [HypeTabsNativeProbe]::GetDlgItem($window, 302)
    for ($attempt=0; $attempt -lt 100; $attempt++) {
        if ([HypeTabsNativeProbe]::Count($list) -eq 2000) { break }
        Start-Sleep -Milliseconds 100
    }
    if ([HypeTabsNativeProbe]::Count($list) -ne 2000) { throw 'The full 2,000-record workload was not indexed.' }
    $queryTimes = @(); $overlayTimes = @()
    $queries = @('documentation travel', 'chrome travel', 'missingword travel')
    $expectedCounts = @(400, 200, 0)
    for ($trial=0; $trial -lt 105; $trial++) {
        $queryIndex = if ($VariedQueries) { $trial % $queries.Count } else { 0 }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        if (![HypeTabsNativeProbe]::Query($input, $queries[$queryIndex])) { throw 'Timed query failed.' }
        if ([HypeTabsNativeProbe]::Count($list) -ne $expectedCounts[$queryIndex]) { throw 'Timed query returned incorrect results.' }
        $timer.Stop(); if ($trial -ge 5) { $queryTimes += $timer.Elapsed.TotalMilliseconds }
        Ui-Message 0x0312 1
        $timer.Restart(); Ui-Message 0x0312 1
        if (![HypeTabsNativeProbe]::IsWindowVisible($window)) { throw 'Timed overlay remained hidden.' }
        $timer.Stop(); if ($trial -ge 5) { $overlayTimes += $timer.Elapsed.TotalMilliseconds }
    }
    Ui-Message 0x0312 1
    if ($priorWindow -ne [IntPtr]::Zero) { [void][HypeTabsNativeProbe]::SetForegroundWindow($priorWindow) }
    if (!$LatencyOnly) { Start-Sleep -Seconds 10 }
    $app.Refresh(); if ($app.PriorityClass -ne 'Idle') { throw 'Host priority changed.' }
    $workingSets = @((Private-WorkingSet))
    $cpuStart = $app.TotalProcessorTime.TotalSeconds
    $idleTimer = [Diagnostics.Stopwatch]::StartNew()
    while (!$LatencyOnly -and $idleTimer.Elapsed.TotalSeconds -lt 300) {
        Start-Sleep -Seconds 30
        $app.Refresh(); if ($app.HasExited) { throw 'Host exited during idle measurement.' }
        Write-Output ('Idle measurement: '+[int]$idleTimer.Elapsed.TotalSeconds+' seconds')
    }
    $app.Refresh(); $cpuEnd = $app.TotalProcessorTime.TotalSeconds; $idleTimer.Stop()
    $workingSets += Private-WorkingSet
    $result = [ordered]@{
        executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        mode=$(if ($LatencyOnly) { 'latency-only' } else { 'latency-and-idle' })
        queryWorkload=$(if ($VariedQueries) { 'alternating 400, 200, and 0 results' } else { 'repeated 400-result query' })
        workload='1000 open, 1000 closed, 5 synthetic profile connections, 10 window IDs'
        priority=$app.PriorityClass.ToString()
        durationSeconds=$(if (!$LatencyOnly) { $idleTimer.Elapsed.TotalSeconds } else { $null })
        cpuSeconds=$(if (!$LatencyOnly) { $cpuEnd-$cpuStart } else { $null })
        idleCpuPercentOneCore=$(if (!$LatencyOnly) { 100*($cpuEnd-$cpuStart)/$idleTimer.Elapsed.TotalSeconds } else { $null })
        privateWorkingSetBytesBeforeAfter=$workingSets
        messageToOverlayVisibleP95Ms=(Percentile95 $overlayTimes)
        queryMessageToCountVerifiedP95Ms=(Percentile95 $queryTimes)
        trials=100
        limits='Synthetic transport and window messages; excludes Chrome/bridge overhead, physical hotkey delivery, rendering completion, and peak working set.'
    }
    $result | ConvertTo-Json -Depth 3
    $report = Join-Path $PSScriptRoot $(if ($LatencyOnly) { '..\build\host-latency.json' } else { '..\build\host-measurement.json' })
    $result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $report -Encoding UTF8
} finally {
    foreach ($pipe in $connections) { $pipe.Dispose() }
    if ($app -and !$app.HasExited) {
        if ($window -ne [IntPtr]::Zero) { [void][HypeTabsNativeProbe]::PostMessage($window, 0x0111, [IntPtr]104, [IntPtr]0) }
        if (!$app.WaitForExit(5000)) { $app.Kill(); [void]$app.WaitForExit(5000) }
    }
    if ($priorWindow -ne [IntPtr]::Zero) { [void][HypeTabsNativeProbe]::SetForegroundWindow($priorWindow) }
    Remove-Item -LiteralPath $directory -Recurse -Force
}
