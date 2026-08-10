param(
    [string]$Godot = 'C:\Users\lucin\Documents\Developement\Godot\Godot_v4.7.1-stable_win64_console.exe',
    [string]$Project = 'C:\Users\lucin\Documents\Farlands',
    [int]$Runs = 3,
    [int]$QuitAfter = 1800,
    [int]$FreezeTimeMs = 8000,
    [int]$MaxRunTimeSec = 120
)

$logBase = 'C:\Users\lucin\AppData\Local\Temp\opencode\farlands_freeze'

New-Item -ItemType Directory -Force -Path $logBase | Out-Null

$freezes = 0
$ok = 0
$timeouts = 0
for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $logBase ("run_{0}.log" -f $i)
    Remove-Item $log, "$log.err" -Force -ErrorAction SilentlyContinue
    $started = Get-Date
    $proc = Start-Process -FilePath $godot -ArgumentList @('--path', $project, '--quit-after', $QuitAfter, '--resolution', '640x360') -PassThru -RedirectStandardOutput $log -RedirectStandardError (Join-Path $logBase ("run_{0}.err" -f $i))
    $frozen = $false
    $lastCpuTime = [TimeSpan]::Zero
    $lastSample = Get-Date
    $lastLogSize = 0
    $quietMs = 0
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 500
        $proc.Refresh()
        if ($proc.HasExited) { break }
        $elapsed = (Get-Date) - $lastSample
        $cpuDelta = $proc.TotalProcessorTime - $lastCpuTime
        $lastCpuTime = $proc.TotalProcessorTime
        $lastSample = Get-Date
        $totalSec = ((Get-Date) - $started).TotalSeconds
        $logSize = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
        if ($cpuDelta.TotalMilliseconds -lt 100) { $quietMs += $elapsed.TotalMilliseconds } else { $quietMs = 0 }
        if (($totalSec % 10) -lt 1) {
            Write-Output ("  sample t={0:N0}s cpuTotal={1:N2}s quiet={2:N0}s log={3}" -f $totalSec, $proc.TotalProcessorTime.TotalSeconds, ($quietMs/1000), $logSize)
        }
        if ($quietMs -ge $FreezeTimeMs) {
            $frozen = $true
            $freezes++
            Write-Output ("RUN {0}: FREEZE at {1:N1}s (CPU {2:N2}s total)" -f $i, $totalSec, $proc.TotalProcessorTime.TotalSeconds)
            Stop-Process -Id $proc.Id -Force
            break
        }
        if ($totalSec -ge $MaxRunTimeSec) {
            $timeouts++
            Write-Output ("RUN {0}: still running at {1:N1}s (CPU {2:N1}s total)" -f $i, $totalSec, $proc.TotalProcessorTime.TotalSeconds)
            Stop-Process -Id $proc.Id -Force
            break
        }
    }
    if (-not $frozen) {
        if ($proc.HasExited) {
            $code = $proc.ExitCode
            if ($code -eq 0) {
                $ok++
                Write-Output ("RUN {0}: OK (exit 0) in {1:N1}s" -f $i, ((Get-Date) - $started).TotalSeconds)
            } else {
                $timeouts++
                Write-Output ("RUN {0}: exited with code {1}" -f $i, $code)
            }
        }
    }
    if ($frozen) { Write-Output ("  frozen log: {0} bytes" -f (Get-Item $log).Length) }
    Start-Sleep -Seconds 2
}

Write-Output ("=== RESULT: {0} runs, {1} freeze, {2} ok, {3} other ===" -f $Runs, $freezes, $ok, $timeouts)
