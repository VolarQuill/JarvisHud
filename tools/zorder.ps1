<#
    zorder.ps1 - dump the visible top-level window z-order (top first) and
    report where the HUD sits in it.
#>
param([string] $ExePath = "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe")

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ZO {
    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -TypeDefinition $sig

$p = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 1800

$list = New-Object System.Collections.ArrayList
$cb = [ZO+EnumProc] {
    param($h, $l)
    if ([ZO]::IsWindowVisible($h)) {
        $cn = New-Object System.Text.StringBuilder 256
        [void][ZO]::GetClassNameW($h, $cn, 256)
        $tt = New-Object System.Text.StringBuilder 256
        [void][ZO]::GetWindowTextW($h, $tt, 256)
        $ex = [ZO]::GetWindowLongPtr($h, -20).ToInt64()
        $r = New-Object ZO+RECT
        [void][ZO]::GetWindowRect($h, [ref]$r)
        [void]$list.Add([pscustomobject]@{
            Class = $cn.ToString(); Title = $tt.ToString()
            Ex = ('0x{0:X8}' -f $ex)
            Topmost = (($ex -band 0x8) -ne 0)
            L = $r.Left; T = $r.Top; R = $r.Right; B = $r.Bottom
            H = $h
        })
    }
    return $true
}
[void][ZO]::EnumWindows($cb, [IntPtr]::Zero)

$i = 0
foreach ($w in $list) {
    $mark = ' '
    if ($w.Class -eq 'JarvisHudWindowClass') { $mark = '>' }
    Write-Host ('{0}{1,3}  {2,-38} topmost={3,-5} ex={4}  rect={5},{6},{7},{8}  {9}' -f `
        $mark, $i, $w.Class, $w.Topmost, $w.Ex, $w.L, $w.T, $w.R, $w.B, $w.Title)
    $i++
}

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
