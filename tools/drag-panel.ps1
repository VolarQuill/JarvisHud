#  Drags a panel by posting the mouse messages to its window.
#
#  The injected input cannot be used here: the person at the machine
#  moves the real mouse, and every injected click lands wherever their
#  cursor happens to be.  The messages are posted instead, and the
#  panels read the position of the mouse with GetCursorPos, so the
#  cursor is moved with SetCursorPos as well.
param([int]$X = 1205, [int]$Y = 148, [int]$DX = -120, [int]$DY = 30,
      [string]$Class = 'JarvisSearchBar')
$ErrorActionPreference = 'Stop'

Add-Type -Namespace W -Name U -MemberDefinition @'
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumWindows")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", EntryPoint = "GetWindowRect")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", EntryPoint = "SetCursorPos")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "PostMessageW")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint = "SetProcessDpiAwarenessContext")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public struct RECT { public int L, T, R, B; }
'@
[void][W.U]::SetProcessDpiAwarenessContext([IntPtr](-4))

$WM_MOUSEMOVE = 0x0200
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP = 0x0202
$MK_LBUTTON = 0x0001

$target = [IntPtr]::Zero
$cb = [W.U+EnumProc]{
    param($h, $p)
    $cls = New-Object System.Text.StringBuilder 256
    [void][W.U]::GetClassNameW($h, $cls, 256)
    if ($cls.ToString() -eq $Class) { $script:target = $h; return $false }
    return $true
}
$script:target = [IntPtr]::Zero
[void][W.U]::EnumWindows($cb, [IntPtr]::Zero)
if (-not $target) { throw "no window of class $Class" }

function Rect() {
    $r = New-Object W.U+RECT
    [void][W.U]::GetWindowRect($target, [ref]$r)
    $r
}
$r0 = Rect
Write-Host "before : $($r0.L),$($r0.T) - $($r0.R),$($r0.B)"

[void][W.U]::SetCursorPos($X, $Y)
Start-Sleep -Milliseconds 200
$lp = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
[void][W.U]::PostMessageW($target, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
Start-Sleep -Milliseconds 200

$steps = 8
for ($i = 1; $i -le $steps; $i++) {
    $px = $X + [int]($DX * $i / $steps)
    $py = $Y + [int]($DY * $i / $steps)
    [void][W.U]::SetCursorPos($px, $py)
    Start-Sleep -Milliseconds 60
    $lp = [IntPtr](($py -shl 16) -bor ($px -band 0xFFFF))
    [void][W.U]::PostMessageW($target, $WM_MOUSEMOVE, [IntPtr]$MK_LBUTTON, $lp)
    Start-Sleep -Milliseconds 60
}
[void][W.U]::PostMessageW($target, $WM_LBUTTONUP, [IntPtr]0, $lp)
Start-Sleep -Milliseconds 500

$r1 = Rect
Write-Host "after  : $($r1.L),$($r1.T) - $($r1.R),$($r1.B)"
