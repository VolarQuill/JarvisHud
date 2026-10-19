<#
    devshot.ps1 - capture the screen in true device pixels and crop the
    region the HUD window actually occupies (physical coordinates).
#>
param(
    [string] $ExePath = "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe",
    [string] $Shot    = "$env:TEMP\jarvis-devshot.png",
    [int]    $AtMs    = 1500
)

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class D {
    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")]
    public static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, int rop);
    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("kernel32.dll")]
    public static extern uint GetLastError();
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -TypeDefinition $sig

Add-Type -AssemblyName System.Drawing

# be DPI aware so GetWindowRect and the screen size are real device pixels
$ctx = [IntPtr](-4)   # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
if (-not [D]::SetProcessDpiAwarenessContext($ctx)) {
    Write-Host ('DPI awareness not set, error ' + [D]::GetLastError())
}

$p = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds $AtMs

$script:hud = [IntPtr]::Zero
$cb = [D+EnumProc] {
    param($h, $l)
    if ([D]::IsWindowVisible($h)) {
        $cn = New-Object System.Text.StringBuilder 256
        [void][D]::GetClassNameW($h, $cn, 256)
        if ($cn.ToString() -eq 'JarvisHudWindowClass') { $script:hud = $h }
    }
    return $true
}
[void][D]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:hud -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw 'HUD window not found' }

$r = New-Object D+RECT
[void][D]::GetWindowRect($script:hud, [ref]$r)
$sw = [D]::GetSystemMetrics(0)
$sh = [D]::GetSystemMetrics(1)
Write-Host ('screen (GetSystemMetrics): ' + $sw + 'x' + $sh)
Write-Host ('HUD rect (device px)     : ' + $r.Left + ',' + $r.Top + ' -> ' + $r.Right + ',' + $r.Bottom)

# raise the HUD for the capture (test only) and report its z-order slot
$ok = [D]::SetWindowPos($script:hud, [IntPtr]::Zero, 0, 0, 0, 0, 0x0013)
Write-Host ('SetWindowPos(HWND_TOP) returned ' + $ok + ' err=' + [D]::GetLastError())
Start-Sleep -Milliseconds 350
$order = New-Object System.Collections.ArrayList
$cb2 = [D+EnumProc] {
    param($h, $l)
    if ([D]::IsWindowVisible($h)) {
        $cn = New-Object System.Text.StringBuilder 256
        [void][D]::GetClassNameW($h, $cn, 256)
        [void]$order.Add($cn.ToString())
    }
    return $true
}
[void][D]::EnumWindows($cb2, [IntPtr]::Zero)
$idx = [array]::IndexOf($order.ToArray(), 'JarvisHudWindowClass')
Write-Host ('HUD z-order slot (0 = top): ' + $idx + ' of ' + $order.Count)
for ($i = 0; $i -lt [math]::Min(8, $order.Count); $i++) {
    Write-Host ('   [' + $i + '] ' + $order[$i])
}

# full screen in device pixels
$full = [System.Drawing.Bitmap]::new($sw, $sh, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($full)
$dst = $g.GetHdc()
$src = [D]::GetDC([IntPtr]::Zero)
[void][D]::BitBlt($dst, 0, 0, $sw, $sh, $src, 0, 0, 0x00CC0020)   # SRCCOPY
[void][D]::ReleaseDC([IntPtr]::Zero, $src)
$g.ReleaseHdc($dst)
$g.Dispose()

# crop the HUD rect, with a small margin
$mx = 20; $my = 20
$cx = [math]::Max(0, $r.Left - $mx)
$cy = [math]::Max(0, $r.Top - $my)
$cw = [math]::Min($sw - $cx, ($r.Right - $r.Left) + 2 * $mx)
$ch = [math]::Min($sh - $cy, ($r.Bottom - $r.Top) + 2 * $mx)
$crop = [System.Drawing.Bitmap]::new($cw, $ch, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gc = [System.Drawing.Graphics]::FromImage($crop)
$gc.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $cw, $ch), $cx, $cy, $cw, $ch, [System.Drawing.GraphicsUnit]::Pixel)
$gc.Dispose()
$crop.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
$crop.Dispose(); $full.Dispose()
Write-Host ('saved ' + $Shot + '  (' + $cw + 'x' + $ch + ')')

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
