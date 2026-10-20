# JARVIS HUD

JARVIS HUD is a small Win32/GDI+ program that paints a JARVIS-style greeting when you sign in to
Windows 11: a cyan arc-reactor on the left, a typewriter-revealed greeting on the right, a status
line and a clock, all drawn in code with GDI+ paths and text. It docks in a corner of the primary
monitor's work area, never takes focus, and is click-through. It lives in the *desktop band* — on
the wallpaper, above the icons, and below every application window — so whatever you open covers
it completely and it never overlays anything. It starts with Windows, stays in its corner until
you end it in Task Manager or shut the PC down, and replaces the greeting it is showing every
minute.

```
        ╭────────────────────────────────────────────────────────────────────────────╮
        │ ╭────────────────────────────────────────────────────────────────────────╮ │
        │ │                                                                        │ │
        │ │ ╭─────────────────╮                                                    │ │
        │ │ │ ╭─────────────╮ │                                                    │ │
        │ │ │ │ ╭─────────╮ │ │     G O O D   E V E N I N G ,   S I R .            │ │
        │ │ │ │ │ ╭─────╮ │ │ │                                                    │ │
        │ │ │ │ │ │ (o) │ │ │ │     "All systems are nominal, sir."                │ │
        │ │ │ │ │ ╰─────╯ │ │ │                                                    │ │
        │ │ │ │ ╰─────────╯ │ │                                                    │ │
        │ │ │ ╰─────────────╯ │                                                    │ │
        │ │ ╰─────────────────╯                                                    │ │
        │ │ ───────────────────────────────────────────────────────────────────────│ │
        │ │ S Y S T E M   O N L I N E                                         21:47│ │
        │ ╰────────────────────────────────────────────────────────────────────────╯ │
        ╰────────────────────────────────────────────────────────────────────────────╯
           (docked to the top-left of the work area, 32 px margin)
```

The mockup is not to scale, and the reactor is drawn schematically as concentric boxes. From the
top: a rounded near-black panel with a 1 px cyan border and four bright corner brackets; a faint
background grid; the arc reactor (a rotating segmented outer ring, 8 radial ticks, a mid ring, a
glowing inner ring and a white-hot core); the greeting typed in character by character; a hairline
separator; and a status line with the clock.

Every pixel is drawn by the program. No image, sprite sheet, bitmap or other artwork is embedded
or shipped.

The HUD is started by the per-user `Run` entry, so it appears a moment after the desktop shell
starts rather than on the pre-logon screen. It waits for the shell's desktop window to appear and
then comes up in the desktop band, above the wallpaper and the icons. If you need a message on
the pre-logon screen itself, see *The pre-login legal notice* below.

---

## Requirements

| | |
|---|---|
| OS | Windows 11 25H2 (build 26200) is the reference target. Also runs on Windows 10 1809+ and Windows 11. Needs per-monitor DPI awareness (v2), present since Windows 10 1703. |
| Runtime | None. GDI+ (`gdiplus.dll`) ships with Windows. No .NET, no redistributable, no installer payload beyond the three files below. |
| Fonts | Monospace only. Prefers **Cascadia Mono** (ships with Windows 11 22H2+), then **Consolas**, then **Lucida Console**, then **Courier New**. The first family that actually loads is used; there is no proportional fallback. |
| Toolchain | Only needed if you build from source: MSYS2 with the UCRT64 environment (built and tested with `g++` 16.2.0; C++17 and `-static` are the only requirements). A prebuilt `JarvisHud.exe` needs no compiler. |
| Privileges | Not needed for the HUD itself. Administrator rights are needed only for the optional pre-login legal notice (see below). |

---

## Install

### One-liner

From an ordinary (non-elevated) PowerShell, in the project directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

Useful variants:

```powershell
# custom location, overwrite an existing install
.\install.ps1 -InstallPath "$env:LOCALAPPDATA\JarvisHud" -Force

# do not register the "Run" entry (run the HUD by hand only)
.\install.ps1 -NoAutoStart

# additionally install the pre-login legal notice (needs an elevated shell)
.\install.ps1 -LegalNotice
```

The installer is idempotent: running it twice is safe. It creates the install folder, builds
`JarvisHud.exe` if a compiler is found (otherwise it copies a prebuilt `JarvisHud.exe` from the
project root, and errors out clearly if there is neither), copies `greetings.txt` and
`config.txt` without clobbering user edits (an existing file is backed up to `<file>.bak` unless
`-Force` is given), and registers the autostart entry. It never copies the `reference/` or `dev/`
folders.

Default install location: `%LOCALAPPDATA%\JarvisHud` (i.e. `C:\Users\<you>\AppData\Local\JarvisHud`).

### Manual install

1. **Build the executable** (skip if you already have a prebuilt `JarvisHud.exe`):

   ```bash
   /c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -municode -mwindows \
       -o JarvisHud.exe src/JarvisHud.cpp \
       -lgdiplus -luser32 -lgdi32 -lole32 -luuid
   ```

   Run that from the project root (`C:/Users/<you>/projects/jarvis-hud`) inside an MSYS2 UCRT64 shell.
   The library flags must come *after* the source file. `-municode` selects the wide-argument
   entry point and `-mwindows` makes it a GUI-subsystem binary, so no console window appears.
   To check for warnings, add `-Wall -Wextra`.

   **Use the static build for an installed copy.** The command above leaves the exe depending on
   `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` from
   `C:\msys64\ucrt64\bin`, which is not on the PATH of a normal logon session. A missing runtime
   DLL makes the program exit silently, which looks exactly like "the HUD never appeared". Build
   it self-contained instead:

   ```bash
   /c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -municode -mwindows \
       -o JarvisHud.exe src/JarvisHud.cpp \
       -lgdiplus -luser32 -lgdi32 -lole32 -luuid -static
   ```

   Check the result with `objdump -p JarvisHud.exe | grep "DLL Name"`: with `-static` the only
   dependencies are Windows system DLLs (`KERNEL32`, `USER32`, `GDI32`, `ADVAPI32`, `gdiplus`
   and the UCRT forwarders). The installer tries the canonical line first and then a few
   variations, and reports the exact command that succeeded.

2. **Copy the files** to the install folder:

   ```powershell
   $dst = "$env:LOCALAPPDATA\JarvisHud"
   New-Item -ItemType Directory -Force -Path $dst | Out-Null
   Copy-Item .\JarvisHud.exe, .\greetings.txt, .\config.txt $dst -Force
   ```

3. **Register the autostart entry** (per-user, so no administrator rights are needed):

   ```powershell
   $exe = "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe"
   New-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Force | Out-Null
   Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
                    -Name "JarvisHud" -Value ('"' + $exe + '"')
   ```

   Equivalent from `cmd.exe`:

   ```bat
   reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v JarvisHud /t REG_SZ /d "\"C:\Users\<you>\AppData\Local\JarvisHud\JarvisHud.exe\"" /f
   ```

   The value is the quoted path to the exe, quoted as a `REG_SZ` because the path can contain
   spaces. The program reads `config.txt` and `greetings.txt` from its own directory, so the three
   files must stay together.

4. **Test it.** Log off and back on, or launch it directly:

   ```powershell
   & "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe" --duration 15000
   ```

   The HUD appears in the top-left of the work area, greets you, and (with `--duration`) runs
   its whole animation stretched or squeezed into exactly 15 seconds so you can inspect it.

---

## Uninstall

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

Add `-RemoveFiles` to also delete the install folder and the executable, and `-KeepLegalNotice`
to leave the Winlogon `LegalNoticeText` / `LegalNoticeCaption` values in place. The uninstaller
removes the `HKCU\...\Run` value, the `JarvisHudNotice` scheduled task and the Winlogon notice
values, and it is safe to run twice (running it against an already-clean machine just reports
that there was nothing to do).

Manually, the same three things:

```powershell
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "JarvisHud" -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName "JarvisHudNotice" -Confirm:$false -ErrorAction SilentlyContinue
# requires an elevated shell:
Remove-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" -Name LegalNoticeText -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" -Name LegalNoticeCaption -ErrorAction SilentlyContinue
```

Leaving the `Run` value in place is enough to stop the HUD from appearing at logon; everything
else is cleanup.

---

## Customisation

### `greetings.txt`

One greeting per line, UTF-8 without BOM:

```
# comments start with a hash
Welcome back, sir.
All systems are nominal, sir.
{weekday} diagnostics complete, {name}. {time} is {status}.
```

* Blank lines and lines starting with `#` are ignored; every other line is a candidate greeting.
* Keep lines to roughly 64 characters or fewer. The text column is 358 logical px wide and the
  greeting wraps to at most two lines; longer lines are auto-shrunk (1 px steps, down to a floor
  of 12 px) and then ellipsised.
* Placeholders are substituted before the line is displayed:

  | placeholder | expands to |
  |---|---|
  | `{name}` | the `name` value from `config.txt` (default `sir`) |
  | `{time}` | the current local time, `HH:MM` |
  | `{date}` | the current full long date |
  | `{weekday}` | the current weekday name |
  | `{status}` | the `status_text` value from `config.txt` |

  Any other `{word}` is left exactly as written.
* A line may carry an optional **time-of-day tag**, written as `tag|` in front of the text:

  ```
  morning|A fine morning for it, sir.
  afternoon|The workshop is at its brightest, sir.
  evening|The blinds are open, sir.
  any|Something that suits any hour.
  ```

  | tag | eligible between |
  |---|---|
  | `morning` | 05:00 – 11:59 |
  | `afternoon` | 12:00 – 17:59 |
  | `evening` | 18:00 – 04:59 |
  | `any` | always |

  Untagged lines are always eligible. A tagged line only ever appears during its own part of the
  day, so you never get "a fine morning for it" at midnight. If no line matches the current hour
  the whole corpus is used as a fallback. Only the four tags above are recognised — a greeting may
  still contain a `|` character of its own.
* To add a greeting, append a line and save. The file is re-read on every launch, so no restart
  is required. Comments and blank lines may be used liberally to organise the corpus.

### `config.txt`

UTF-8 (a BOM, if present, is stripped), `key=value` per line, `#` starts a comment, whitespace is
trimmed, unknown keys are ignored so old settings never break a new build. All sizes are in
*logical* pixels and are multiplied by the DPI of the monitor the HUD lands on (so 17 means 17 px
at 96 DPI, 21 px at 125% scaling).

| key | default | what it does |
|---|---|---|
| `name` | `sir` | Value substituted into `{name}`. |
| `font` | `Cascadia Mono` | Monospace family for all text. If the family is not installed the program falls back to Consolas, then Lucida Console, then Courier New. A proportional family named here is rejected in favour of the fallback chain. |
| `font_size` | `17` | Logical pixel size of the greeting text, scaled by DPI. |
| `status_font_size` | `11` | Logical pixel size of the status line, scaled by DPI. |
| `position` | `top-left` | Which corner of the primary monitor's work area to dock to: `bottom-right`, `bottom-left`, `top-right`, `top-left` or `center`. |
| `margin` | `32` | Logical pixels between the panel and the edge of the work area. |
| `panel_width` | `520` | Logical width of the panel, scaled by DPI. |
| `panel_height` | `186` | Logical height of the panel, scaled by DPI. |
| `fade_in_ms` | `450` | Duration of the fade-in, in milliseconds. |
| `boot_ms` | `1200` | Reactor spin-up and sweep-line duration before typing starts. |
| `type_ms` | `28` | Milliseconds per character in the typewriter reveal. |
| `hold_ms` | `4200` | How long the completed greeting stays fully visible. |
| `fade_out_ms` | `700` | Duration of the fade-out. |
| `dismiss_ms` | `0` | `0` = the HUD closes itself after fade-in + boot + typing + hold + fade-out. Any value greater than `0` forces a hard dismiss that many milliseconds after launch, regardless of the other timings. |
| `persist` | `1` | `1` = keep the HUD on screen for ever after the greeting. It still fades in, types and holds, but it does not fade out and the process does not exit; the reactor keeps turning and the greeting is replaced every `rotate_seconds`. It only goes away when you end it in Task Manager or shut the PC down. `0` = show the greeting once and exit. |
| `show_clock` | `1` | `1` = draw the current time on the status line. |
| `status_text` | `SYSTEM ONLINE` | Text on the status line, and the value of the `{status}` placeholder. |
| `desktop_wait_ms` | `8000` | How long to wait at logon for the shell's desktop window to appear. The panel stays hidden until it is in the desktop band, and goes up as soon as the shell is there. |
| `greeting_line` | `1` | Which line of `greetings.txt` the panel greets you with when it comes up, counted from one. Line 1 is *"Welcome back, sir."*. `0` = pick one at random instead, using the rotation model below. |
| `rotate_seconds` | `60` | How often the persistent panel replaces the greeting it is showing with another one from `greetings.txt`. The new line is typed out from the beginning; the panel never moves or flickers. `0` = never rotate. |
| `notice_text` | `0` | `1` = also write the chosen greeting to the Winlogon `LegalNoticeText` value so it appears on the pre-login screen. Needs administrator rights and only takes effect after a restart. |

A complete, commented `config.txt` is installed alongside the executable; the table above is the
authoritative list of what the program reads.

---

## How the greeting is chosen (rotation model)

1. The program loads `greetings.txt`, drops comments and blank lines, and keeps what is left as
   the candidate list.
2. Lines are then filtered by the current hour: untagged lines and lines tagged with the current
   part of the day are eligible. If nothing matches, the whole corpus is used instead.
3. If there is exactly one candidate, it is used as-is — no rotation, no randomness.
4. Otherwise the index is drawn pseudo-randomly from a seed mixed from `GetTickCount64()`, the
   process id and the address of a stack local, so two launches in the same millisecond on
   different machines do not produce the same sequence. `greeting_line` in `config.txt` overrides
   this: if it names a line that is eligible, that line is used instead and no draw is made.
5. The index of the greeting that was used last is persisted as a `DWORD` under
   `HKCU\Software\JarvisHud\LastIndex`. If the freshly drawn index matches the stored one, it is
   drawn again (up to 8 attempts); if every attempt collides, the next eligible line is used, so
   the same greeting is never shown twice in a row. The stored value is an index into the *whole*
   corpus, so it stays meaningful as the eligible set changes with the time of day.
6. The chosen index is written back to that key. The key lives in `HKCU`, so it is per-user and
   needs no elevation. Deleting the key just resets the history.

---

## Z-order and input behaviour

The HUD is a deliberately polite overlay. The contract is:

* **Not always-on-top.** The window is created with
  `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT` and *no* `WS_EX_TOPMOST` and *no*
  `WS_EX_LAYERED`. There is no setting to turn either of those on: the panel is always an
  ordinary window, and the behaviour it produces is not configurable because it is the whole
  point of the program.
* **Never steals focus.** `WS_EX_NOACTIVATE` is set, `WM_MOUSEACTIVATE` is overridden to return
  `MA_NOACTIVATE`, the window is shown with `SW_SHOWNOACTIVATE`, and `SetForegroundWindow` is
  never called. Clicking on it, or on the desktop, or on another window, never moves the caret
  out of what you were typing in.
* **Click-through.** `WS_EX_TRANSPARENT` makes the window transparent to mouse input: every click
  and every hover reaches the window underneath. `WM_NCHITTEST` also answers `HTTRANSPARENT`, so
  even the panel's own frame is click-through. The HUD can never swallow a click meant for
  another application.
* **Out of the way.** `WS_EX_TOOLWINDOW` keeps it out of the taskbar and out of the Alt+Tab list.
  It is docked to a corner of the *work area* (the monitor rectangle minus the taskbar), so it
  never covers the middle of the screen and never lands on top of the taskbar.
* **Lives in the desktop band.** The panel is placed in the *desktop band*: immediately above
  the shell's desktop window (`Progman`) in the z-order and therefore below every application
  window. That is what "only on the desktop" means in practice — the panel is above the
  wallpaper and the desktop icons, and anything you open covers it completely, including
  maximised and full-screen applications. It is never given `WS_EX_TOPMOST` to achieve this,
  because being topmost is precisely what the requirement forbids.

### Why the panel is not a layered window

This is the single most important thing to know about the program, and it is easy to get wrong.

The first implementation drew the panel with `UpdateLayeredWindow(ULW_ALPHA)`, which is the
obvious way to get per-pixel transparency. It looked perfect and it was completely wrong. The
desktop window manager composites layered windows in a pass of its own, **after** the ordinary
windows, and a layered window is therefore painted on top of everything on the screen no matter
where it sits in the z-order. `SetWindowPos(hud, HWND_BOTTOM)` does not put it at the bottom; it
puts it at the bottom of the layered pass, which is still on top of every application. Adding
`WS_EX_TOPMOST` would not have helped either — it would have made the same mistake louder.

The fix was to make the panel an **ordinary** window with an opaque background, drawn with GDI+
and blitted into the window's own surface with `SetDIBitsToDevice`, and clipped to the card's
rounded shape with `SetWindowRgn`. Ordinary windows have a redirection surface, obey the
z-order, and are composited by DWM in the ordinary pass — so putting the panel directly above
`Progman` really does put it below every application.

The price of that decision is that an ordinary window cannot be transparent, so the fade is a
*dissolve towards the card colour* rather than a true fade to nothing. See *Rendering* below.

### Why the desktop band, and not a child window

The other obvious way to put a widget "on the desktop" is to make it a child of the shell's
desktop window, so that it belongs to the desktop layer by construction. That approach was
implemented and then **measured on this machine**, and it does not work on Windows 11 25H2: a
child of `Progman` never composites. Six variants were tried — a plain child, a layered child
using `UpdateLayeredWindow`, a colour-keyed child, a global-alpha child, a child of
`SHELLDLL_DefView` and a child of the icon list view — and not one of them ever put a single
pixel on the screen. `UpdateLayeredWindow` reports success, the window is positioned correctly
and the mouse passes straight through it to the icon list view, but the wallpaper is still there
underneath. It is not a repaint race: sampling the screen over several seconds shows the
wallpaper the whole time.

The desktop band is the approach that does work, and it was verified the same way — by exact
colour sampling of the screen, not by trusting a return code.

### Keeping the panel in the band

The shell raises its own desktop window from time to time: a desktop refresh, a shell
notification, an `explorer.exe` restart. Each time it does, a panel that was in the band falls
underneath it and the wallpaper hides it. The program therefore checks its place in the z-order
once a second and puts itself back if it has fallen out.

The check is `GetWindow(hud, GW_HWNDNEXT) == Progman`, i.e. "is the panel directly above the
shell's desktop window?". When it is not, the panel is moved back. This only ever moves the
panel *within* the band: it is never raised above an application that was in front of it, and it
never touches the foreground window.

At logon the shell may not be running yet, so the program waits up to `desktop_wait_ms` for the
desktop window to appear. Until the shell is up the panel stays hidden, and it goes up the
moment it is in the band. The same once-a-second check covers a shell that appears later.

---

## The persistent panel

With `persist=1` the panel does not fade out and the process does not exit. It stays in its
corner from logon until you end it in Task Manager or shut the PC down.

* The panel is started by the `HKCU\...\CurrentVersion\Run` entry that `install.ps1` writes, so
  it comes up with Windows like any other startup program. It does not need a task or a service,
  and it does not need administrator rights.
* The greeting is replaced every `rotate_seconds`. The new line is chosen with the same rules
  as the first one (eligible for the time of day, never the same line twice in a row), expanded
  afresh — so the clock in it is current — laid out again and typed out from the beginning. The
  panel itself does not move, resize or flicker.
* The reactor ring keeps turning slowly (12 degrees a second) while the panel idles, and the
  frame rate drops to about 11 fps once the typing has finished.
* Nothing is re-read from disk after launch: `config.txt` and `greetings.txt` are read once, at
  startup, so editing them takes effect on the next launch.

---

## The pre-login legal notice (optional)

Windows can display a text notice *before* the logon screen, using the registry values
`LegalNoticeText` and `LegalNoticeCaption` under
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`. JARVIS HUD can write a greeting
into that notice:

* `JarvisHud.exe --set-notice` picks a random greeting from `greetings.txt`, substitutes the
  placeholders, and writes the result to `LegalNoticeText` (`REG_SZ`) with
  `LegalNoticeCaption` set to `JARVIS`. It also updates the stored last index.
* `config.txt` with `notice_text=1` makes the normal run do the same thing.
* `install.ps1 -LegalNotice` creates a scheduled task named `JarvisHudNotice`, triggered
  "At log on", running the exe with `--set-notice` as `SYSTEM` at `RunLevel Highest` so the
  registry write succeeds without a UAC prompt at every boot. It also sets the notice values once
  during installation.

The caveat, stated plainly: **this is not the HUD.** The legal notice is a plain winlogon
dialog — white text on a black background, no reactor, no animation, no styling — and it is
*modal*: the logon screen does not appear until you dismiss it with a key press or a click.
That adds a small, deliberate delay to every boot, for every user of the machine, and it cannot
be skipped. The notice value is also written in plain text to the registry and is visible to any
local administrator. If you want the JARVIS greeting as the first thing you see, use the normal
HUD; use the legal notice only if a pre-login message is a hard requirement. Remove it with
`uninstall.ps1` or by deleting the two registry values.

Writing those values requires administrator rights. Without them the program exits silently with
code 0 and changes nothing.

---

## How it is drawn

* One top-down 32-bit ARGB surface the size of the *scaled panel* (not the screen), plus a work
  surface and an opaque output surface that is blitted into the window.
* Two layers: a **static** base rendered once (rounded-rect panel fill, 1 px border, the four
  corner brackets, the faint grid, the separator hairline) and a **dynamic** layer drawn every
  frame (arc reactor, sweep line, greeting text, status line, clock) over a copy of the base.
* Because the panel is an ordinary window it cannot have per-pixel alpha, so the frame is
  flattened onto the opaque card colour before it is shown: the alpha of every pixel is composited
  over the card with `dst = src + dst*(1-srcA)` (premultiplied, table driven, no per-pixel
  division), and the fade-in/fade-out is a **dissolve towards the card colour** rather than a
  true fade to nothing.
* The flattened frame is written into the window with `SetDIBitsToDevice` from a 32-bit BI_RGB
  top-down DIB, and the window is clipped to the card's rounded shape with `SetWindowRgn`, so
  the corners are cut out of the window itself rather than painted black.
* Antialiasing is enabled explicitly (`SmoothingModeAntiAlias` plus `PixelOffsetModeHalf`), which
  matters: GDI+ does not antialias by default, and an unantialiased rounded rectangle looks
  like a hard box. Text uses `TextRenderingHintAntiAliasGridFit` — never ClearType, whose
  subpixel colour fringes would be baked into a translucent surface.
* The reactor is a set of concentric paths: an outer rotating segmented ring (drawn as short
  arcs at a rotating start angle), a mid ring, an inner glow ring, 8 radial ticks, and a
  white-hot core.
* The frame is pushed with `RedrawWindow(RDW_INVALIDATE | RDW_UPDATENOW)`; the window is never
  raised above the desktop band (see the z-order section).

---

## Performance

Measured on the reference machine (Windows 11 25H2 build 26200, Intel Core i5-8350U 4c/8t,
16 GB RAM, display at 125% scaling, so the panel is 650x233 device pixels) with the shipped
build.

| metric | value |
|---|---|
| Working set while the HUD is visible | 19.4 MB peak |
| Private bytes (the part unique to the process) | 5.1 MB peak |
| CPU for one complete greeting (7.5-8.2 s of wall clock: fade-in, boot, typing, hold, fade-out) | 0.66-0.72 s |
| CPU while the animation is running | ~88 ms per second of wall clock (about 9% of one core) |
| CPU per frame, drawing only | 0.88-1.00 ms, at a fixed 30 fps |
| Frames rendered per greeting | ~225 |
| CPU with `persist=1` (reactor turning, no greeting) | 67 ms per second of wall clock (about 7% of one core) |

How it was measured:

* **Whole-process figures** — a PowerShell harness starts the exe and samples `WorkingSet64`,
  `PrivateMemorySize64` and `TotalProcessorTime` every 100 ms until the process exits; the
  reported CPU is the last sample taken before exit. Repeat runs were within 10% of each other.
* **Per-frame figures** — the program instruments its own render path and reports it in the
  self-test log (`probe_frames=120 ... present_ms=1.9363 ... flatten=0.7122`). The independent
  cross-check is `probe/verify/v20_deltas.exe`, which times a full 1280x800 GDI+ scene (rounded
  panel fill, glow blit, four rings, a dashed arc, two text lines) at 4.98 ms on this machine.
  Timing a frame without destroying the `Graphics` under-reports the cost by roughly an order of
  magnitude, so the `Graphics` lifetime is inside the timed region.

Where the memory goes: two 32-bit ARGB surfaces of the scaled panel, 650x233x4 = 606 kB each, so
about 1.2 MB of pixel data. The rest of the working set is shared system pages (`gdiplus.dll`,
`gdi32.dll`, `user32.dll`) and the GDI+ heap; the 5.1 MB private figure is the part attributable
to the program itself.

Where the CPU goes: per frame, the cost is dominated by GDI+ software rasterisation of the
reactor and the text plus the flatten pass; the remainder is the 33 ms timer, the window update
and DWM composition. The design decisions that keep it small are that the panel fill, border,
brackets, grid and separator are rendered *once* into the static base layer and blitted per
frame instead of being re-rasterised, and that only the 650x233 surface is ever touched — never
the screen.

Honest note against the original design target: the target was a single-digit MB working set
and under 50 ms of CPU for the whole animation. The private footprint (5.1 MB) is inside that
target; the working set (19.4 MB, mostly shared DLL pages) and the total CPU (~0.7 s spread over
7.5 s, ~9% of one core while visible) are above it. The driver is the 30 Hz redraw of the panel
through a software-rasterised GDI+ surface, not the drawing itself (0.9 ms per frame).
If you need less: keep `persist=0` so the process exits and the cost stops entirely, and
remember that the animation is the whole workload, so a shorter timeline is proportionally
cheaper.

With `persist=1` — the default, and the mode the panel is designed for — the process is
resident for the whole session, so the cost does not stop: the panel idles at about 11 fps and
the ring keeps turning, which is the 67 ms per second of wall clock in the table above, roughly
7% of one core. That is the price of a panel that is always there. Set `persist=0` in
`config.txt` and it goes back to being a short-lived process that spends its CPU in one burst
at sign-in and leaves nothing running.

---

## Troubleshooting

**It does not appear at all.**
Check that the `Run` value exists and points at the exe that is actually on disk:

```powershell
Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name JarvisHud
& "$env:LOCALAPPDATA\JarvisHud\JarvisHud.exe" --log "$env:TEMP\jarvis.log"
Get-Content "$env:TEMP\jarvis.log"
```

A missing `config.txt` or `greetings.txt` is *not* fatal — the program falls back to built-in
defaults — but a missing exe obviously is. Also check that the account is allowed to log on
interactively, and that no third-party "overlay" or taskbar-replacement tool is fighting for the
same corner.

If the exe exists but nothing happens at all, suspect a missing runtime DLL. The program is
designed to fail silently, so a `libstdc++-6.dll` that is not on the PATH of the logon session
looks exactly like a program that does nothing:

```bash
objdump -p JarvisHud.exe | grep "DLL Name"
```

If you see `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll` in that list,
rebuild with `-static` (see *Building from source*).

**It appears and disappears too quickly.**
Lengthen the visible phases in `config.txt`: `hold_ms` (default 4200) is the time the finished
greeting stays up, `boot_ms` (1200) is the reactor spin-up, `type_ms` (28) is the typing rate,
and `fade_in_ms` / `fade_out_ms` are the transitions. Note that `dismiss_ms`, if set to
anything greater than zero, overrides all of that with a hard lifetime limit.

**The text is too small (or too large).**
`font_size` (default 17) and `status_font_size` (default 11) are logical pixels and are scaled by
the display DPI, so 17 means 21 px at 125% scaling. If the greeting does not fit in two lines the
program shrinks it one pixel at a time down to a floor of 12 px and then ellipsises it; that
auto-shrink is a fallback, not a substitute for picking a size that fits.

**It overlaps the taskbar.**
It should not: the panel is positioned against `rcWork` from `GetMonitorInfo`, not the raw
screen rectangle, so the taskbar area is excluded. If it does overlap, check that `position` and
`margin` are sensible, and that the display you are looking at is the primary monitor (the HUD
always docks to the primary monitor's work area, by design, so it is in the same place on every
boot). Auto-hidden taskbars overlap the work area by definition, so the HUD can sit where the
taskbar would appear.

**It seems to sit on top of another application.**
That should not happen, and there is no longer any setting that could make it happen: the panel
is never topmost and never layered, so it is always in the desktop band, below every
application. If the HUD does stay on top of something, check that the installed exe is the one
from this project — an older build that used a layered window would behave exactly like that.
If a specific application (typically a game or a video player in exclusive full screen) still
draws over the HUD, that application is topmost itself; the HUD is not.

**It eats my clicks.**
It should not — `WS_EX_TRANSPARENT` makes it click-through. If clicks are being swallowed,
check that the installed exe is the one from this project (an older build without
`WS_EX_TRANSPARENT` would behave differently) and that `position=center` has not put the panel
over something you actually need to click.

**Running it by hand for testing.**

```powershell
# show the HUD, with the whole animation (fade-in, boot, typing, hold,
# fade-out) rescaled to fit exactly 15 seconds
.\JarvisHud.exe --duration 15000

# parse the config, pick a greeting, start GDI+, create and destroy the window
# without showing it, write a summary to a log file and exit
.\JarvisHud.exe --selftest --log "$env:TEMP\jarvis-selftest.txt"
Get-Content "$env:TEMP\jarvis-selftest.txt"

# write a one-line diagnostic (chosen greeting, resolved font, DPI, panel
# size, frames drawn, CPU time)
.\JarvisHud.exe --log "$env:TEMP\jarvis.log"
Get-Content "$env:TEMP\jarvis.log"
```

The self test also renders one frame to `JarvisHud_frame.png` next to the executable, which is a
quick way to check the layout without showing the HUD on screen.

The program is a GUI-subsystem binary: it never opens a console window, so run it from a
terminal and use `--log` if you want output. Every failure path exits silently with code 0 —
no error dialogs, no message boxes, no lingering process.

---

## Building from source

Requirements: [MSYS2](https://www.msys2.org/) with the `mingw-w64-ucrt-x86_64-toolchain`
package (or the full `msys2-devel` group), which provides `g++` and the UCRT64 runtime.

```bash
# in an MSYS2 UCRT64 shell (or run "MSYS2 UCRT64" from the Start menu)
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain

cd /c/Users/<you>/projects/jarvis-hud
/c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -municode -mwindows \
    -o JarvisHud.exe src/JarvisHud.cpp \
    -lgdiplus -luser32 -lgdi32 -lole32 -luuid
```

Notes:

* The library flags must follow the source file on the command line.
* `-municode` selects `wWinMain` with wide arguments; `-mwindows` links the GUI subsystem so
  no console flashes up.
* `-lgdiplus` resolves to the real Microsoft `gdiplus.dll` in `C:\Windows\System32`, not a
  redistributable, and it is present on every supported Windows version. There is nothing else to
  deploy — provided you link the C++ runtime statically. Without `-static` (or
  `-static-libgcc -static-libstdc++` plus a static `libwinpthread`), the exe imports
  `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` from
  `C:\msys64\ucrt64\bin`. Those are not on the PATH of a normal logon session, and a missing
  DLL makes the program exit silently with code 0 — indistinguishable from "the HUD does not
  work". For an installed copy, use:

  ```bash
  /c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -municode -mwindows \
      -o JarvisHud.exe src/JarvisHud.cpp \
      -lgdiplus -luser32 -lgdi32 -lole32 -luuid -static
  ```

  Verify with `objdump -p JarvisHud.exe | grep "DLL Name"`: only Windows system DLLs should be
  listed.
* The program is warning-clean under `-Wall -Wextra`:

  ```bash
  /c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows \
      -o JarvisHud.exe src/JarvisHud.cpp \
      -lgdiplus -luser32 -lgdi32 -lole32 -luuid
  ```

* To build a debug build, add `-g -O0` and drop `-mwindows` if you want a console for
  diagnostics.

---

## Testing

Three tools live in `tools/`. They are for development only; nothing in them is installed and
the program does not depend on any of them.

| file | what it does |
|---|---|
| `tools/acceptance.ps1` | The acceptance test. It starts the HUD, checks the window's extended styles, checks that the panel is in the desktop band, screenshots the screen to prove the panel is painted there, then launches an ordinary application window over the panel and screenshots again to prove the application covers it. It reports `PASS`/`FAIL` per check and exits with the number of failures. |
| `tools/cover.cpp` | A plain Win32 window that covers a rectangle of the screen in one solid colour. It is deliberately *not* topmost — a topmost window would cover the HUD even if the HUD were topmost too, which would prove nothing. |
| `tools/shotlib.cs` | A small C# helper (compiled on the fly by the scripts) for screen captures, pixel counting and image comparison. |

```powershell
# build the cover window first
/c/msys64/ucrt64/bin/g++ -std=c++17 -O2 -municode -mwindows -o tools/cover.exe tools/cover.cpp -luser32

# run the acceptance test
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\acceptance.ps1
```

The acceptance test needs the reference frame `JarvisHud_frame.png`, which the self test
writes next to the executable:

```powershell
.\JarvisHud.exe --selftest --log "$env:TEMP\jarvis-selftest.txt"
Get-Content "$env:TEMP\jarvis-selftest.txt"
```

`tools/verify.ps1` is a lighter diagnostic for the *installed* copy: it launches
`%LOCALAPPDATA%\JarvisHud\JarvisHud.exe`, reports the window's extended styles, its on-screen
rect and the process's memory and CPU use, and saves a screenshot of the top-left corner.
`tools/zorder.ps1` prints the whole visible z-order, which is how the desktop band was
verified. `tools/devshot.ps1` renders a still of the panel without showing it.

---

## Porting to Linux (CachyOS) — planned

Not implemented. This is a design note for a future port, not a working feature. The Windows
version relies on an ordinary window in the shell's desktop band and on `WS_EX_TRANSPARENT` for
click-through; the Linux equivalents are different mechanisms, and a port is a rewrite of the
rendering and window-management layer, not a recompile.

The intended shape:

* **Rendering** — Cairo or a small Skia surface, same two-layer structure (static base plus a
  dynamic reactor/text layer), same premultiplied-alpha output.
* **Window** — a GTK4 or wlroots layer-shell surface. `gtk_layer_shell_set_layer(..., GTK_LAYER_SHELL_LAYER_TOP)` gives the overlay; `wl_surface` with `xdg_toplevel` and the `gtk-layer-shell` protocol provides the anchor and margin. Click-through is a layer-shell property (`GdkSurface` input region set to empty, or `wl_surface` with `wl_region` set to empty so pointer events pass through). The "not always-on-top" promise maps to using a non-exclusive overlay layer rather than a compositor rule.
* **Service** — a systemd *user* unit, started at graphical session start:

  ```ini
  # ~/.config/systemd/user/jarvis-hud.service
  [Unit]
  Description=JARVIS HUD login greeting
  After=graphical-session.target
  PartOf=graphical-session.target

  [Service]
  Type=simple
  ExecStart=%h/.local/bin/jarvis-hud
  Restart=no

  [Install]
  WantedBy=graphical-session.target
  ```

  enabled with `systemctl --user enable --now jarvis-hud.service`.
* **Greeting selection** — the same rotation model, with the last index stored in
  `~/.local/state/jarvis-hud/last-index` instead of the registry.
* **Fonts** — `JetBrains Mono` or `Cascadia Mono` from the system font set, with the same
  fallback chain.

Known open questions for the port: how to make the overlay appear *before* the session starts
(the analogue of the Windows logon screen is the display manager, and a user unit cannot paint
over the greeter), and how to guarantee that the overlay is not raised above application windows
once the session is up.
