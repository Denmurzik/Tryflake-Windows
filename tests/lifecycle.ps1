# Lifecycle-тест Tryflake: эмуляция системных событий через PostMessage в главное окно.
# Запуск: powershell -NoProfile -ExecutionPolicy Bypass -File tests/lifecycle.ps1
# Не трогает мышь и чужие окна. Код выхода: 0 - все проверки пройдены, 1 - есть FAIL.
# Не проверяется PBT_POWERSETTINGCHANGE: lParam - указатель на POWERBROADCAST_SETTING,
# через границу процессов его передать нельзя.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'build\Tryflake.exe'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public delegate bool MonProc(IntPtr m, IntPtr dc, IntPtr r, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonProc p, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr proc, uint flags);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindow(string c, string t);

    static Win() { try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch { SetProcessDPIAware(); } }

    public static IntPtr MainWnd() { return FindWindow("TryflakeMain", null); }
    public static bool Alive(IntPtr h) { return IsWindow(h); }

    // Окна заданного класса процесса pid: "hwnd,visible,l,t,r,b".
    public static List<string> Windows(string cls, uint pid) {
        var res = new List<string>();
        EnumWindows((h, l) => {
            var sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (sb.ToString() != cls) return true;
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            RECT r; GetWindowRect(h, out r);
            res.Add(h.ToInt64() + "," + (IsWindowVisible(h) ? 1 : 0) + "," + r.L + "," + r.T + "," + r.R + "," + r.B);
            return true;
        }, IntPtr.Zero);
        return res;
    }

    public static List<string> Monitors() {
        var res = new List<string>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (m, dc, r, l) => {
            RECT rc = (RECT)Marshal.PtrToStructure(r, typeof(RECT));
            res.Add(rc.L + "," + rc.T + "," + rc.R + "," + rc.B);
            return true;
        }, IntPtr.Zero);
        return res;
    }
}
'@

$script:fails = 0
$script:passes = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:passes++; Write-Host "PASS  $name" -ForegroundColor Green }
    else { $script:fails++; Write-Host "FAIL  $name  $detail" -ForegroundColor Red }
}

function Get-Overlays($procId) {
    $list = @()
    foreach ($s in [Win]::Windows('TryflakeOverlay', [uint32]$procId)) {
        $p = $s.Split(',')
        $list += [pscustomobject]@{
            Hwnd = [int64]$p[0]; Visible = ($p[1] -eq '1')
            Rect = ('{0},{1},{2},{3}' -f $p[2], $p[3], $p[4], $p[5])
        }
    }
    , $list
}

function Wait-Until([scriptblock]$cond, [int]$timeoutMs = 5000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return [bool](& $cond)
}

function Post([IntPtr]$h, [uint32]$msg, [int64]$w = 0, [int64]$l = 0) {
    [void][Win]::PostMessage($h, $msg, [IntPtr]$w, [IntPtr]$l)
}

$WM_DISPLAYCHANGE = 0x7E; $WM_SETTINGCHANGE = 0x1A; $WM_POWERBROADCAST = 0x218; $WM_WTSSESSION_CHANGE = 0x2B1
$SPI_SETWORKAREA = 0x2F; $PBT_APMSUSPEND = 4; $PBT_APMRESUMEAUTOMATIC = 0x12
$WTS_SESSION_LOCK = 7; $WTS_SESSION_UNLOCK = 8
$REBUILD_WAIT = 1000

if (-not (Test-Path $exe)) { Write-Host "BLOCKED: $exe not found (build first)"; exit 2 }

Get-Process -Name Tryflake -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$proc = $null
try {
    $monitors = @([Win]::Monitors())
    Write-Host "Monitors: $($monitors.Count)  [$($monitors -join ' | ')]"

    # 1. Старт
    $proc = Start-Process -FilePath $exe -PassThru
    $procId = $proc.Id
    $hwnd = [IntPtr]::Zero
    $ok = Wait-Until { $script:hwnd = [Win]::MainWnd(); $script:hwnd -ne [IntPtr]::Zero } 10000
    Check 'start: TryflakeMain window appears' $ok
    if (-not $ok) { throw 'no main window' }
    $hwnd = [Win]::MainWnd()
    $ok = Wait-Until { (Get-Overlays $procId).Count -ge 1 } 10000
    Check 'start: TryflakeOverlay windows appear' $ok
    Start-Sleep -Milliseconds 800

    $ov = Get-Overlays $procId
    Check "start: overlay count ($($ov.Count)) == monitor count ($($monitors.Count))" ($ov.Count -eq $monitors.Count)
    Check 'start: all overlays visible' (@($ov | Where-Object { -not $_.Visible }).Count -eq 0)
    $missing = @($monitors | Where-Object { $ov.Rect -notcontains $_ })
    Check 'start: overlay rects match monitor rects' ($missing.Count -eq 0) "monitors without overlay: $($missing -join ' | '); overlays: $($ov.Rect -join ' | ')"
    foreach ($o in $ov) { Write-Host ("      overlay hwnd={0} visible={1} rect={2}" -f $o.Hwnd, $o.Visible, $o.Rect) }

    # 2. WM_DISPLAYCHANGE: окна пересоздаются
    $before = (Get-Overlays $procId).Hwnd
    Post $hwnd $WM_DISPLAYCHANGE 32 0
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'WM_DISPLAYCHANGE: process alive' (-not $proc.HasExited)
    Check 'WM_DISPLAYCHANGE: main hwnd unchanged' ([Win]::MainWnd() -eq $hwnd)
    Check 'WM_DISPLAYCHANGE: overlay count == monitors' ($ov.Count -eq $monitors.Count) "got $($ov.Count)"
    Check 'WM_DISPLAYCHANGE: overlay hwnds recreated' (@($ov | Where-Object { $before -contains $_.Hwnd }).Count -eq 0) "before=$($before -join ',') after=$($ov.Hwnd -join ',')"
    Check 'WM_DISPLAYCHANGE: overlays visible' (@($ov | Where-Object { -not $_.Visible }).Count -eq 0)
    Check 'WM_DISPLAYCHANGE: no duplicate overlays left (old hwnds gone)' ($ov.Count -le $monitors.Count)

    # 3. SPI_SETWORKAREA
    $before = (Get-Overlays $procId).Hwnd
    # WM_SETTINGCHANGE через PostMessage система не доставляет (проверено), поэтому SendMessageTimeout.
    $r = [IntPtr]::Zero
    [void][Win]::SendMessageTimeout($hwnd, $WM_SETTINGCHANGE, [IntPtr]$SPI_SETWORKAREA, [IntPtr]::Zero, 2, 2000, [ref]$r)
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'SPI_SETWORKAREA: process alive' (-not $proc.HasExited)
    Check "SPI_SETWORKAREA: overlays rebuilt (new hwnds)" (@($ov | Where-Object { $before -contains $_.Hwnd }).Count -eq 0) "before=$($before -join ',') after=$($ov.Hwnd -join ',')"
    Check 'SPI_SETWORKAREA: count == monitors, visible' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0)

    # 4. Suspend / resume
    Post $hwnd $WM_POWERBROADCAST $PBT_APMSUSPEND 0
    $ok = Wait-Until { @(Get-Overlays $procId | Where-Object { $_.Visible }).Count -eq 0 } 3000
    Check 'PBT_APMSUSPEND: overlays hidden' $ok
    Check 'PBT_APMSUSPEND: process alive' (-not $proc.HasExited)
    $suspHwnds = (Get-Overlays $procId).Hwnd
    Post $hwnd $WM_POWERBROADCAST $PBT_APMRESUMEAUTOMATIC 0
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'PBT_APMRESUMEAUTOMATIC: process alive' (-not $proc.HasExited)
    Check 'PBT_APMRESUMEAUTOMATIC: overlays visible again, count == monitors' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0) "count=$($ov.Count) visible=$(@($ov | Where-Object { $_.Visible }).Count)"
    Check 'PBT_APMRESUMEAUTOMATIC: overlays rebuilt' (@($ov | Where-Object { $suspHwnds -contains $_.Hwnd }).Count -eq 0)

    # 4b. Resume без suspend не должен ничего перестраивать
    $before = (Get-Overlays $procId).Hwnd
    Post $hwnd $WM_POWERBROADCAST $PBT_APMRESUMEAUTOMATIC 0
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'resume without suspend: ignored (hwnds unchanged)' (@($ov | Where-Object { $before -contains $_.Hwnd }).Count -eq $ov.Count -and $ov.Count -eq $before.Count)

    # 5. Lock / unlock
    Post $hwnd $WM_WTSSESSION_CHANGE $WTS_SESSION_LOCK 1
    $ok = Wait-Until { @(Get-Overlays $procId | Where-Object { $_.Visible }).Count -eq 0 } 3000
    Check 'WTS_SESSION_LOCK: overlays hidden' $ok
    Check 'WTS_SESSION_LOCK: process alive' (-not $proc.HasExited)
    $lockHwnds = (Get-Overlays $procId).Hwnd
    Post $hwnd $WM_WTSSESSION_CHANGE $WTS_SESSION_UNLOCK 1
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'WTS_SESSION_UNLOCK: process alive' (-not $proc.HasExited)
    Check 'WTS_SESSION_UNLOCK: overlays visible again, count == monitors' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0) "count=$($ov.Count) visible=$(@($ov | Where-Object { $_.Visible }).Count)"
    Check 'WTS_SESSION_UNLOCK: overlays rebuilt' (@($ov | Where-Object { $lockHwnds -contains $_.Hwnd }).Count -eq 0)

    # 5b. Lock + suspend одновременно: снимается по одному
    Post $hwnd $WM_WTSSESSION_CHANGE $WTS_SESSION_LOCK 1
    Post $hwnd $WM_POWERBROADCAST $PBT_APMSUSPEND 0
    Start-Sleep -Milliseconds 300
    Post $hwnd $WM_POWERBROADCAST $PBT_APMRESUMEAUTOMATIC 0
    Start-Sleep -Milliseconds $REBUILD_WAIT
    Check 'lock+suspend, resume only: overlays stay hidden (still locked)' (@(Get-Overlays $procId | Where-Object { $_.Visible }).Count -eq 0)
    Post $hwnd $WM_WTSSESSION_CHANGE $WTS_SESSION_UNLOCK 1
    Start-Sleep -Milliseconds $REBUILD_WAIT
    $ov = Get-Overlays $procId
    Check 'lock+suspend, then unlock: overlays visible' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0)

    # 6. TaskbarCreated
    $before = (Get-Overlays $procId).Hwnd
    $tb = [Win]::RegisterWindowMessage('TaskbarCreated')
    Post $hwnd $tb 0 0
    Start-Sleep -Milliseconds 800
    $ov = Get-Overlays $procId
    Check 'TaskbarCreated: process alive, main window present' ((-not $proc.HasExited) -and [Win]::Alive($hwnd))
    Check 'TaskbarCreated: overlays untouched' (@($ov | Where-Object { $before -contains $_.Hwnd }).Count -eq $ov.Count)

    # 7. Второй запуск
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $second = Start-Process -FilePath $exe -PassThru
    $exited = $second.WaitForExit(5000)
    Check 'second launch: exits quickly' ($exited -and $sw.ElapsedMilliseconds -lt 4000) "elapsed=$($sw.ElapsedMilliseconds)ms exited=$exited"
    if (-not $exited) { try { $second.Kill() } catch {} }
    Start-Sleep -Milliseconds 700
    $inst = @(Get-Process -Name Tryflake -ErrorAction SilentlyContinue)
    Check 'second launch: first instance survives (single process)' ((-not $proc.HasExited) -and $inst.Count -eq 1) "instances=$($inst.Count)"
    $ov = Get-Overlays $procId
    Check 'second launch: overlays still visible, count == monitors' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0)

    # 8. Утечки: 20 циклов WM_DISPLAYCHANGE (+3 прогревочных)
    function Snapshot {
        $p = Get-Process -Id $procId
        [pscustomobject]@{
            Handles = $p.HandleCount
            Gdi = [Win]::GetGuiResources($p.Handle, 0)
            User = [Win]::GetGuiResources($p.Handle, 1)
            PrivateMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
            WorkingMB = [math]::Round($p.WorkingSet64 / 1MB, 1)
            Threads = $p.Threads.Count
        }
    }
    $cycleFail = $false
    for ($i = 0; $i -lt 3; $i++) { Post $hwnd $WM_DISPLAYCHANGE 32 0; Start-Sleep -Milliseconds 700 }
    $base = Snapshot
    Write-Host ("      baseline: handles={0} gdi={1} user={2} private={3}MB working={4}MB threads={5}" -f $base.Handles, $base.Gdi, $base.User, $base.PrivateMB, $base.WorkingMB, $base.Threads)
    $series = @()
    for ($i = 1; $i -le 20; $i++) {
        Post $hwnd $WM_DISPLAYCHANGE 32 0
        Start-Sleep -Milliseconds 700
        if ($proc.HasExited) { $cycleFail = $true; break }
        if ($i % 5 -eq 0) { $series += (Snapshot) }
    }
    Check '20 display-change cycles: process alive' (-not $cycleFail -and -not $proc.HasExited)
    if (-not $proc.HasExited) {
        $end = Snapshot
        Write-Host ("      final:    handles={0} gdi={1} user={2} private={3}MB working={4}MB threads={5}" -f $end.Handles, $end.Gdi, $end.User, $end.PrivateMB, $end.WorkingMB, $end.Threads)
        foreach ($s in $series) { Write-Host ("      series:   handles={0} gdi={1} user={2} private={3}MB" -f $s.Handles, $s.Gdi, $s.User, $s.PrivateMB) }
        Check "leak: handles growth ($($base.Handles) -> $($end.Handles)) <= 15" (($end.Handles - $base.Handles) -le 15)
        Check "leak: GDI objects growth ($($base.Gdi) -> $($end.Gdi)) <= 10" (($end.Gdi - $base.Gdi) -le 10)
        Check "leak: USER objects growth ($($base.User) -> $($end.User)) <= 10" (($end.User - $base.User) -le 10)
        Check "leak: private bytes growth ($($base.PrivateMB) -> $($end.PrivateMB) MB) <= 40 MB" (($end.PrivateMB - $base.PrivateMB) -le 40)
        Check "leak: thread count stable ($($base.Threads) -> $($end.Threads))" (($end.Threads - $base.Threads) -le 3)
        $ov = Get-Overlays $procId
        Check 'after cycles: overlay count == monitors, visible' ($ov.Count -eq $monitors.Count -and @($ov | Where-Object { -not $_.Visible }).Count -eq 0)
    }
}
catch {
    $script:fails++
    Write-Host "FAIL  script error: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace
}
finally {
    Get-Process -Name Tryflake -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "Passed: $script:passes  Failed: $script:fails"
if ($script:fails -gt 0) { exit 1 }
exit 0
