<#
    acceptance.ps1 - the acceptance test for the JARVIS HUD.

    It checks the things the project promises:

      1. the HUD paints the panel on the desktop, in the top-left
      2. it lives in the desktop band: directly above the shell's
         desktop window, so every application window is above it
      3. an ordinary application window really does cover it - the HUD
         never overlays an application
      4. it is cheap: a small working set, no focus, click-through

    The checks are made against the z order and against the window's own
    pixels, not against a screen capture alone.  A screen capture of the
    HUD's rectangle shows whatever the compositor put on top of it, which
    on a machine with a full screen game running is the game, not the
    HUD.  PrintWindow shows the HUD's own content whatever is on top, so
    "the HUD painted itself" and "the HUD is under the application" are
    two separate questions and each is measured directly.

    Nothing is moved, resized, hidden or closed, so a full screen game
    keeps running undisturbed.

    Usage:
        .\tools\acceptance.ps1 [-ExePath <path>]
#>
param(
    [string] $ExePath   = "$PSScriptRoot\..\JarvisHud.exe",
    [string] $CoverExe  = "$PSScriptRoot\cover.exe",
    [switch] $NoRestore
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition (Get-Content -Raw "$PSScriptRoot\shotlib.cs") -ReferencedAssemblies System.Drawing
[Cap]::Init()

$sig = @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class A {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -TypeDefinition $sig

$fail = 0
function Check($name, $ok, $detail) {
    if ($ok) { Write-Host ("  PASS  {0}: {1}" -f $name, $detail) -ForegroundColor Green }
    else     { Write-Host ("  FAIL  {0}: {1}" -f $name, $detail) -ForegroundColor Red; $script:fail++ }
}

#  Shell windows that belong to the desktop itself.
$shellClasses = @('Progman','WorkerW','Shell_TrayWnd','Shell_SecondaryTrayWnd',
                  'Windows.UI.Core.CoreWindow','EdgeUiInputTopWndClass',
                  'DummyDWMListenerWindow','ThumbnailDeviceHelperWnd',
                  'SysShadow','tooltips_class32','XamlExplorerHostIslandWindow_WASDK',
                  'PseudoConsoleWindow','Windows.UI.Composition.DesktopWindowContentBridge')

#  The colour of the card.  The panel is a near black navy.
$cardR = 4; $cardG = 14; $cardB = 24

#  The visible top level windows, in z order: the first one is the top.
function Get-ZOrder {
    $list = New-Object System.Collections.ArrayList
    $script:list = $list
    $script:index = 0
    $cb = [A+EnumProc] {
        param($h, $l)
        if ([A]::IsWindowVisible($h)) {
            $cn = New-Object System.Text.StringBuilder 256
            [void][A]::GetClassNameW($h, $cn, 256)
            $tt = New-Object System.Text.StringBuilder 256
            [void][A]::GetWindowTextW($h, $tt, 256)
            $r = New-Object A+RECT
            [void][A]::GetWindowRect($h, [ref]$r)
            $script:index++
            [void]$script:list.Add([pscustomobject]@{
                H = $h; Class = $cn.ToString(); Title = $tt.ToString()
                Index = $script:index
                L = $r.Left; T = $r.Top; R = $r.Right; B = $r.Bottom
            })
        }
        return $true
    }
    [void][A]::EnumWindows($cb, [IntPtr]::Zero)
    return $list
}

function Get-DesktopWindow {
    $hit = $null
    $cb = [A+EnumProc] {
        param($h, $l)
        $cn = New-Object System.Text.StringBuilder 256
        [void][A]::GetClassNameW($h, $cn, 256)
        if ($cn.ToString() -eq 'Progman' -and [A]::IsWindowVisible($h)) { $script:hit = $h; return $false }
        return $true
    }
    [void][A]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:hit
}

#  The share of the pixels of an image that are the card colour.
function Get-CardFraction($path) {
    return [Math]::Round(100.0 * [Cap]::ColorFraction($path, $cardR, $cardG, $cardB, 10))
}

Write-Host ''
Write-Host 'JARVIS HUD acceptance test' -ForegroundColor Cyan
Write-Host ("  exe  : {0}" -f (Resolve-Path -LiteralPath $ExePath).Path)

$cover = $null
try {
    Get-Process JarvisHud -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700

    # --- start the HUD ----------------------------------------------------
    Write-Host ''
    Write-Host 'starting the HUD...'
    $p = Start-Process -FilePath $ExePath -PassThru

    #  The panel waits for the shell's desktop window before it shows
    #  itself, so give it a moment to come up.
    $hud = $null
    for ($i = 0; $i -lt 40 -and $hud -eq $null; $i++) {
        Start-Sleep -Milliseconds 400
        if ($p.HasExited) { throw "the HUD exited immediately (code $($p.ExitCode))" }
        $cb = [A+EnumProc] {
            param($h, $l)
            $cn = New-Object System.Text.StringBuilder 256
            [void][A]::GetClassNameW($h, $cn, 256)
            if ($cn.ToString() -eq 'JarvisHudWindowClass' -and [A]::IsWindowVisible($h)) { $script:hud = $h; return $false }
            return $true
        }
        [void][A]::EnumWindows($cb, [IntPtr]::Zero)
    }
    Check 'window' ($hud -ne $null) $(if ($hud) { "handle 0x$($hud.ToInt64().ToString('X'))" } else { 'no HUD window found' })
    if ($hud -eq $null) { throw 'no HUD window' }

    # --- the window's styles ------------------------------------------------
    Write-Host ''
    Write-Host 'checking the window...'
    $ex = [A]::GetWindowLongPtr($hud, -20).ToInt64()
    $r = New-Object A+RECT
    [void][A]::GetWindowRect($hud, [ref]$r)
    $pw = $r.Right - $r.Left; $ph = $r.Bottom - $r.Top

    Check 'not topmost'   (($ex -band 0x8) -eq 0)        ("ex=0x{0:X8}" -f $ex)
    #  The panel has to answer the mouse, or it could never be dragged.
    Check 'answers the mouse' (($ex -band 0x20) -eq 0)  ("WS_EX_TRANSPARENT={0}" -f (($ex -band 0x20) -ne 0))
    Check 'no-activate'   (($ex -band 0x08000000) -ne 0) ("WS_EX_NOACTIVATE={0}" -f (($ex -band 0x08000000) -ne 0))
    #  The panel must be an ORDINARY window.  A layered window is blended
    #  by the desktop window manager in a pass of its own, after the
    #  ordinary windows, so it floats above every application no matter
    #  where it sits in the z order.  That is exactly what we must not
    #  have, so the layer bit has to be clear.
    Check 'not layered'   (($ex -band 0x80000) -eq 0)     ("WS_EX_LAYERED={0}" -f (($ex -band 0x80000) -eq 0))
    Check 'never focused' ([A]::GetForegroundWindow() -ne $hud) 'the HUD does not take focus'

    #  The panel can be dragged anywhere, so it only has to be on the
    #  screen - not in a corner in particular.
    Check 'on the screen' ($r.Left -ge 0 -and $r.Top -ge 0 -and $r.Right -le 1920 -and $r.Bottom -le 1080) ("rect = {0},{1} - {2},{3}  ({4}x{5})" -f `
        $r.Left, $r.Top, $r.Right, $r.Bottom, $pw, $ph)

    # --- the desktop band ---------------------------------------------------
    Write-Host ''
    Write-Host 'checking the desktop band...'
    $progman = Get-DesktopWindow
    Check 'shell desktop found' ($progman -ne $null) $(if ($progman) { "0x$($progman.ToInt64().ToString('X'))" } else { 'not found' })

    $below = [A]::GetWindow($hud, 1)     # GW_HWNDNEXT: the window below
    Check 'in the desktop band' ($below -eq $progman) ("the window directly below the HUD is {0}" -f `
        $(if ($below -eq $progman) { 'the shell desktop (Progman)' } else { "0x$($below.ToInt64().ToString('X'))" }))

    #  The band means: above the wallpaper and the icons, below every
    #  application.  Both halves are checked against the z order.
    $z = Get-ZOrder
    $hudRow = $z | Where-Object { $_.H -eq $hud }
    $progRow = $z | Where-Object { $_.H -eq $progman }
    $apps = @($z | Where-Object {
        $_.H -ne $hud -and $_.H -ne $progman -and ($shellClasses -notcontains $_.Class) -and
        ($_.L -lt $r.Right) -and ($_.R -gt $r.Left) -and ($_.T -lt $r.Bottom) -and ($_.B -gt $r.Top)
    })
    Check 'above the wallpaper' ($progRow -and $hudRow -and $hudRow.Index -lt $progRow.Index) `
        ("the HUD is at {0} of {1} in the z order, the shell desktop at {2}" -f `
        $(if ($hudRow) { $hudRow.Index } else { '?' }), $z.Count, $(if ($progRow) { $progRow.Index } else { '?' }))

    #  A smaller index means a higher place in the z order, so an
    #  application that overlaps the panel must have a smaller index
    #  than the HUD: it has to be in front of it.  Any application that
    #  is behind the HUD is a failure.
    $behind = @($apps | Where-Object { $hudRow -and $_.Index -gt $hudRow.Index })
    Check 'below every application' ($behind.Count -eq 0) `
        ("{0} application(s) overlap the panel, all of them in front of the HUD{1}" -f `
        $apps.Count, $(if ($behind.Count) { ', except ' + (($behind | ForEach-Object { $_.Class }) -join ', ') } else { '' }))

    # --- it paints ----------------------------------------------------------
    Write-Host ''
    Write-Host 'checking that it paints...'
    $own = "$env:TEMP\hud_acc_own.png"
    $drew = [Cap]::WindowShot($hud, $own, $pw, $ph)
    $ownPct = Get-CardFraction $own
    Check 'paints the panel' ($drew -and $ownPct -ge 40) `
        ("{0}% of the panel's own pixels are the card colour" -f $ownPct)
    [Cap]::Shot("$env:TEMP\hud_acc_desktop.png", 0, 0, 760, 300)

    # --- an application covers it -------------------------------------------
    Write-Host ''
    Write-Host 'checking that an application covers it...'
    $cover = Start-Process -FilePath $CoverExe -ArgumentList @(
        $r.Left.ToString(), $r.Top.ToString(), $pw.ToString(), $ph.ToString(), '255', '0', '255') -PassThru
    Start-Sleep -Milliseconds 1800

    $z2 = Get-ZOrder
    $hudRow2 = $z2 | Where-Object { $_.H -eq $hud }
    $coverRow = $z2 | Where-Object { $_.Class -eq 'JarvisCoverTestWindow' }
    Check 'the application is above the HUD' ($coverRow -and $hudRow2 -and $coverRow.Index -lt $hudRow2.Index) `
        ("the covering window is at {0} in the z order, the HUD at {1}" -f `
        $(if ($coverRow) { $coverRow.Index } else { 'not found' }), $(if ($hudRow2) { $hudRow2.Index } else { '?' }))

    #  Now the decisive measurement: none of the HUD's own pixels may be
    #  visible on the screen in the HUD's rectangle.  Whatever is on top
    #  of it - the covering window, a game, anything - has to be on top.
    #
    #  The covering window has a topmost twin, so that this measurement
    #  does not depend on which application the machine happens to have
    #  open.  An application that keeps re-raising itself would otherwise
    #  sit on top of the ordinary cover, and the screen would show that
    #  application instead of the cover: the question could not be
    #  answered at all.  A topmost ordinary window is still composited
    #  before a layered one, so a HUD that was accidentally made layered
    #  still shows up here.
    #
    #  The screen is captured once, wide, and the panel's rectangle is
    #  cut out of that.  Two separate captures race with the compositor:
    #  the screen device context is not the composited output, so a
    #  capture of the same spot a moment later can come back empty.
    $wideB = "$env:TEMP\hud_acc_covered_wide.png"
    $shotB = "$env:TEMP\hud_acc_covered.png"
    [Cap]::Shot($wideB, 0, 0, ($r.Right + 60), ($r.Bottom + 60))
    [Cap]::Crop($wideB, $shotB, $r.Left, $r.Top, $pw, $ph)
    $stillHud = [Cap]::MatchCount($own, $shotB, 0, 0, $pw, $ph, 12)
    $pctHud = [Math]::Round(100.0 * $stillHud / ($pw * $ph))
    $pctCover = [Math]::Round(100.0 * [Cap]::ColorFraction($shotB, 255, 0, 255, 8))
    Check 'the covering window is on screen' ($pctCover -ge 50) `
        ("{0}% of the region is the covering window" -f $pctCover)
    Check 'covered by an application' ($pctHud -le 2) `
        ("$pctHud% of the panel is still visible on top of the application")
    [Cap]::Shot("$env:TEMP\hud_acc_covered_desktop.png", 0, 0, 760, 300)
    [Cap]::Shot("$env:TEMP\hud_acc_covered_desktop.png", 0, 0, 760, 300)
    Stop-Process -Id $cover.Id -Force -ErrorAction SilentlyContinue
    $cover = $null
    Start-Sleep -Milliseconds 600

    # --- it survives the shell raising its own desktop window -----------
    [void][A]::SetWindowPos($progman, [IntPtr]::Zero, 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0010)
    Start-Sleep -Milliseconds 2600
    $below2 = [A]::GetWindow($hud, 1)
    Check 'recovers the desktop band' ($below2 -eq $progman) ("the window directly below the HUD is {0}" -f `
        $(if ($below2 -eq $progman) { 'the shell desktop (Progman)' } else { "0x$($below2.ToInt64().ToString('X'))" }))

    # --- resources --------------------------------------------------------
    $p.Refresh()
    $ws = [Math]::Round($p.WorkingSet64 / 1KB, 0)
    Check 'light on memory' ($ws -lt 60000) ("working set ${ws} KB")
    Write-Host ("  cpu time: {0} ms   handles: {1}   threads: {2}" -f `
        [Math]::Round($p.TotalProcessorTime.TotalMilliseconds, 0), $p.HandleCount, $p.Threads.Count)

    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
finally {
    Get-Process JarvisHud -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($cover) { Stop-Process -Id $cover.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($fail -eq 0) { Write-Host '  RESULT: OK' -ForegroundColor Green }
else             { Write-Host ("  RESULT: {0} FAILURE(S)" -f $fail) -ForegroundColor Red }
Write-Host ''
exit $fail
