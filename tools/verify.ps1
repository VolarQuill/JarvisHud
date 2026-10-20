<#
    verify.ps1 - live acceptance test for the installed JarvisHud.exe.

    Launches the installed program, then reports:
      * the HUD window's extended styles (topmost / click-through / tool window)
      * its on-screen rect (proves the top-left docking)
      * the owning process working set and CPU time
      * a PNG screenshot of the top-left screen region
#>
param(
    [string] $ExePath = "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe",
    [string] $Shot    = "$env:TEMP\jarvis-verify.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

if (-not (Test-Path -LiteralPath $ExePath)) { throw "not found: $ExePath" }

Write-Host "launching $ExePath"
$p = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 2600

if ($p.HasExited) { throw "the program exited immediately with code $($p.ExitCode)" }

# --- window probe ------------------------------------------------------
Add-Type -TypeDefinition $sig
$found = New-Object System.Collections.ArrayList
$cb = [W+EnumProc] {
    param($h, $l)
    if ([W]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 512
        [void][W]::GetWindowTextW($h, $sb, 512)
        $cn = New-Object System.Text.StringBuilder 256
        [void][W]::GetClassNameW($h, $cn, 256)
        $ex = [W]::GetWindowLongPtr($h, -20).ToInt64()
        #  The panel is an ordinary window, so it is found by its class
        #  name, not by the layer bit.
        if ($cn.ToString() -eq 'JarvisHudWindowClass') {
            $r = New-Object W+RECT
            [void][W]::GetWindowRect($h, [ref]$r)
            [void]$found.Add([pscustomobject]@{
                Handle  = $h
                Title   = $sb.ToString()
                Class   = $cn.ToString()
                ExStyle = ('0x{0:X8}' -f $ex)
                L = $r.Left; T = $r.Top; R = $r.Right; B = $r.Bottom
            })
        }
    }
    return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

$fg = [W]::GetForegroundWindow()
foreach ($w in $found) {
    Write-Host ''
    Write-Host '  HUD window' -ForegroundColor Cyan
    Write-Host ('    title    : ' + $w.Title)
    Write-Host ('    class    : ' + $w.Class)
    Write-Host ('    exStyle  : ' + $w.ExStyle)
    Write-Host ('    topmost  : ' + $(if (([Convert]::ToInt64($w.ExStyle.Substring(2), 16) -band 0x8) -ne 0) { 'YES' } else { 'no' }))
    Write-Host ('    layered  : ' + $(if (([Convert]::ToInt64($w.ExStyle.Substring(2), 16) -band 0x80000) -ne 0) { 'YES (wrong)' } else { 'no (correct)' }))
    Write-Host ('    clickthru: ' + $(if (([Convert]::ToInt64($w.ExStyle.Substring(2), 16) -band 0x20) -ne 0) { 'yes' } else { 'no' }))
    Write-Host ('    rect     : L=' + $w.L + ' T=' + $w.T + ' R=' + $w.R + ' B=' + $w.B)
    Write-Host ('    is foreground: ' + ($w.Handle -eq $fg))
}

# --- resource use ------------------------------------------------------
$p.Refresh()
$ws = [math]::Round($p.WorkingSet64 / 1KB, 0)
$pm = [math]::Round($p.PrivateMemorySize64 / 1KB, 0)
$cpu = [math]::Round($p.TotalProcessorTime.TotalMilliseconds, 0)
$handles = $p.HandleCount
$threads = $p.Threads.Count
Write-Host ''
Write-Host '  resources' -ForegroundColor Cyan
Write-Host ('    working set : ' + $ws + ' KB')
Write-Host ('    private mem : ' + $pm + ' KB')
Write-Host ('    cpu time    : ' + $cpu + ' ms')
Write-Host ('    handles     : ' + $handles)
Write-Host ('    threads     : ' + $threads)

# --- screenshot --------------------------------------------------------
$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$w = [int][math]::Min(700, $scr.Width)
$h = [int][math]::Min(320, $scr.Height)
$region = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$bmp = [System.Drawing.Bitmap]::new($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($region.Location, [System.Drawing.Point]::Empty, $region.Size)
$g.Dispose()
$bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host ''
Write-Host ('    screenshot  : ' + $Shot)

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host '  done' -ForegroundColor Green
