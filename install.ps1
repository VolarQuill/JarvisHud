<#
.SYNOPSIS
    Installs the JARVIS HUD login greeting for Windows 11.

.DESCRIPTION
    Creates the install folder, builds JarvisHud.exe (or copies a prebuilt
    one), installs the greeting corpus and the configuration, registers the
    per-user autostart entry and - on request - the pre-login legal notice.

    The script is idempotent: running it twice never errors and never
    duplicates the autostart entry or the scheduled task.

.PARAMETER InstallPath
    Directory the HUD is installed into. Defaults to
    "$env:LOCALAPPDATA\JarvisHud".

.PARAMETER NoAutoStart
    Do not register the "JarvisHud" entry under
    HKCU:\Software\Microsoft\Windows\CurrentVersion\Run. If the entry
    already exists it is removed, so the switch always means "no
    autostart".

.PARAMETER LegalNotice
    Install the pre-login legal notice. Writes LegalNoticeText and
    LegalNoticeCaption under
    HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon and
    creates the "JarvisHudNotice" scheduled task (trigger: at log on,
    run level: highest) that refreshes the notice with a random greeting.
    Requires an elevated (Administrator) session.

.PARAMETER Force
    Overwrite files that are already present, including a config.txt or
    greetings.txt the user has edited. The previous contents are always
    saved next to the file as "<name>.bak" before anything is replaced.

.NOTES
    Data file policy
    ----------------
    A ".bak" file is ALWAYS a snapshot of the file as it was immediately
    before the installer last modified it - in other words, the user's own
    edits, never the shipped default. The live file is only replaced with
    the shipped copy when
      * the file does not exist yet, or
      * the file is already byte-identical to the shipped copy, or
      * -Force was supplied.
    Otherwise an edited config.txt or greetings.txt is left exactly as it
    is, so the installer can never silently throw away a user's work.

    Exit codes
    ----------
    0   success
    1   an error occurred (a readable message is printed first)
    2   the -LegalNotice switch was used without an elevated session

.EXAMPLE
    .\install.ps1
    Installs to %LOCALAPPDATA%\JarvisHud and registers autostart.

.EXAMPLE
    .\install.ps1 -Force
    Reinstalls and replaces an edited config.txt / greetings.txt.

.EXAMPLE
    .\install.ps1 -LegalNotice
    Also installs the pre-login legal notice (run elevated).
#>

[CmdletBinding()]
param(
    [string] $InstallPath = (Join-Path $env:LOCALAPPDATA 'JarvisHud'),
    [switch] $NoAutoStart,
    [switch] $LegalNotice,
    [switch] $Force
)

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$script:AppName       = 'JarvisHud'
$script:ExeName       = 'JarvisHud.exe'
$script:ProjectRoot   = $PSScriptRoot
$script:SourceFile    = Join-Path $PSScriptRoot 'src\JarvisHud.cpp'
$script:RunKeyPath    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$script:RunValueName  = 'JarvisHud'
$script:TaskName      = 'JarvisHudNotice'
$script:WinlogonKey   = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
$script:NoticeCaption = 'JARVIS'
$script:Summary       = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------
# Small output helpers
# ---------------------------------------------------------------------------

function Write-Step {
    param([string] $Text)
    Write-Host ("==> " + $Text) -ForegroundColor Cyan
}

function Write-Ok {
    param([string] $Text)
    Write-Host ("    [ok] " + $Text) -ForegroundColor Green
}

function Write-Info {
    param([string] $Text)
    Write-Host ("    " + $Text) -ForegroundColor Gray
}

function Write-Warn {
    param([string] $Text)
    Write-Host ("    [warn] " + $Text) -ForegroundColor Yellow
}

function Add-Summary {
    param([string] $Text)
    $script:Summary.Add($Text) | Out-Null
}

function Write-Failure {
    <#
        .SYNOPSIS
            Prints a readable error message and where it happened.
    #>
    param([object] $ErrorRecord)

    $message = 'unknown error'
    if ($ErrorRecord -and $ErrorRecord.Exception -and $ErrorRecord.Exception.Message) {
        $message = $ErrorRecord.Exception.Message
    }

    Write-Host ''
    Write-Host ('ERROR: ' + $message) -ForegroundColor Red

    $position = ''
    if ($ErrorRecord -and $ErrorRecord.InvocationInfo -and $ErrorRecord.InvocationInfo.PositionMessage) {
        $position = (($ErrorRecord.InvocationInfo.PositionMessage -split "`r?`n")[0]).Trim()
    }
    if ($position) {
        Write-Host ('       ' + $position) -ForegroundColor DarkRed
    }
}

# ---------------------------------------------------------------------------
# Privilege check
# ---------------------------------------------------------------------------

function Test-Administrator {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------

function Test-SameContent {
    <#
        .SYNOPSIS
            Returns $true when two files exist and have identical bytes.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $PathA,
        [Parameter(Mandatory = $true)][string] $PathB
    )
    if (-not (Test-Path -LiteralPath $PathA)) { return $false }
    if (-not (Test-Path -LiteralPath $PathB)) { return $false }

    $hashA = (Get-FileHash -LiteralPath $PathA -Algorithm SHA256).Hash
    $hashB = (Get-FileHash -LiteralPath $PathB -Algorithm SHA256).Hash
    return ($hashA -eq $hashB)
}

function Install-DataFile {
    <#
        .SYNOPSIS
            Installs one shipped data file (greetings.txt / config.txt).

        .DESCRIPTION
            The user's copy is never silently destroyed:
              * not present                -> copied in
              * identical to the shipped copy -> left alone
              * different (i.e. possibly edited) -> left alone, and a
                snapshot of it is written to "<name>.bak"
            With -Force a differing file is replaced, but the snapshot is
            still written first, so the user's edits stay recoverable.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Source,
        [Parameter(Mandatory = $true)][string] $Destination,
        [Parameter(Mandatory = $true)][string] $Label
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        Write-Warn ('no shipped ' + $Label + ' found, skipping')
        Add-Summary ('skipped ' + $Label + ' (not present in the project folder)')
        return
    }

    $backup = $Destination + '.bak'

    if (-not (Test-Path -LiteralPath $Destination)) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        Write-Ok ('installed ' + $Label)
        Add-Summary ('installed ' + $Label)
        return
    }

    if (Test-SameContent -PathA $Source -PathB $Destination) {
        Write-Ok ($Label + ' is already up to date')
        Add-Summary ($Label + ' unchanged (already up to date)')
        return
    }

    # The installed copy differs from the shipped copy, so it may have been
    # edited by the user. Snapshot it before doing anything else.
    Copy-Item -LiteralPath $Destination -Destination $backup -Force

    if ($Force) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        Write-Ok ($Label + ' replaced with the shipped copy (previous contents saved as ' + (Split-Path -Leaf $backup) + ')')
        Add-Summary ($Label + ' replaced with the shipped copy')
    }
    else {
        Write-Warn ($Label + ' differs from the shipped copy and was left untouched (snapshot: ' + (Split-Path -Leaf $backup) + ')')
        Write-Info  're-run with -Force to replace it with the shipped copy'
        Add-Summary ($Label + ' kept (your copy differs from the shipped default; use -Force to replace)')
    }
}

# ---------------------------------------------------------------------------
# Compiler discovery and build
# ---------------------------------------------------------------------------

function Get-CppCompiler {
    <#
        .SYNOPSIS
            Locates a C++ compiler that can build the HUD.

        .DESCRIPTION
            The MSYS2 UCRT64 g++ is preferred, then any g++ on PATH, then
            the usual MSYS2 / MinGW install locations.
    #>
    $candidates = New-Object System.Collections.Generic.List[string]

    if ($env:JARVISHUD_GPP) {
        $candidates.Add($env:JARVISHUD_GPP) | Out-Null
    }

    # The toolchain this project is written against.
    $candidates.Add('C:\msys64\ucrt64\bin\g++.exe') | Out-Null

    # Anything already on PATH.
    $onPath = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
    if ($onPath) { $candidates.Add($onPath.Source) | Out-Null }

    # Common manual installations.
    foreach ($root in @(
        'C:\msys64\ucrt64\bin',
        'C:\msys64\mingw64\bin',
        'C:\msys64\clang64\bin',
        'C:\mingw64\bin',
        'C:\MinGW\bin',
        'C:\TDM-GCC-64\bin',
        'C:\ProgramData\chocolatey\bin')) {
        $candidates.Add((Join-Path $root 'g++.exe')) | Out-Null
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Invoke-ExeBuild {
    <#
        .SYNOPSIS
            Tries to build JarvisHud.exe with one link-line variant.

        .DESCRIPTION
            The canonical link line from the project specification is tried
            first. A few minimal variations are attempted after it, in this
            order:
              1. + -static  (fully self-contained exe: no MSYS2 runtime
                             DLLs at all, so the installed program is a
                             single file that runs from any folder, which
                             matters because it is launched from the
                             Run key at logon with no shell environment)
              2. the canonical line
              3. + -lmsimg32       (MSYS2's libgdi32 does not export
                                    AlphaBlend, so a direct call needs it)
              4. + -lmsimg32 -static-libgcc -static-libstdc++
                                   (drops the MSYS2 runtime DLL dependency,
                                    so the installed exe runs from any
                                    folder, including at logon)
              5. + -static-libgcc -static-libstdc++ (without msimg32)
            The first variant that links wins and its exact command is
            reported.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Compiler,
        [Parameter(Mandatory = $true)][string] $SourceFile,
        [Parameter(Mandatory = $true)][string] $OutputExe
    )

    $variants = @(
        @{ Name = '+ fully static';       Extra = @('-static') },
        @{ Name = 'canonical';            Extra = @() },
        @{ Name = '+ msimg32';            Extra = @('-lmsimg32') },
        @{ Name = '+ msimg32 + static';   Extra = @('-lmsimg32', '-static-libgcc', '-static-libstdc++') },
        @{ Name = '+ static';             Extra = @('-static-libgcc', '-static-libstdc++') }
    )

    $sourceForCompiler = $SourceFile -replace '\\', '/'
    $outputForCompiler = $OutputExe -replace '\\', '/'
    $lastLog = ''

    foreach ($variant in $variants) {
        $buildArgs = @(
            '-std=c++17', '-O2', '-municode', '-mwindows',
            '-o', $outputForCompiler,
            $sourceForCompiler
        ) + $variant.Extra + @(
            '-lgdiplus', '-luser32', '-lgdi32', '-lole32', '-luuid'
        )

        $log  = ''
        $code = 1
        try {
            $log  = & $Compiler @buildArgs 2>&1 | Out-String
            $code = $LASTEXITCODE
        }
        catch {
            $log  = $_.Exception.Message
            $code = 1
        }

        if ($code -eq 0 -and (Test-Path -LiteralPath $OutputExe)) {
            $command = ($Compiler + ' ' + ($buildArgs -join ' '))
            Write-Ok ('build succeeded (' + $variant.Name + ' variant)')
            Write-Info ('command: ' + $command)
            return $command
        }

        $lastLog = $log
        Write-Info ('build attempt "' + $variant.Name + '" failed')
    }

    if ($lastLog) {
        $lines = @($lastLog -split "`r?`n") | Where-Object { ([string]$_).Trim() -ne '' }
        $lines = @($lines | Select-Object -Last 12)
        Write-Host '    compiler output:' -ForegroundColor DarkGray
        foreach ($line in $lines) {
            Write-Host ('      ' + ([string]$line).Trim()) -ForegroundColor DarkGray
        }
    }
    return $null
}

function Install-Executable {
    <#
        .SYNOPSIS
            Puts JarvisHud.exe into the install folder.

        .DESCRIPTION
            Prefers building from source. Falls back to the prebuilt exe in
            the project root when no compiler is available or the build
            fails. Errors out (non-zero exit) when neither is possible.
    #>
    param([Parameter(Mandatory = $true)][string] $DestinationDir)

    $destination = Join-Path $DestinationDir $script:ExeName
    $prebuilt    = Join-Path $script:ProjectRoot $script:ExeName
    $command     = $null

    $compiler = Get-CppCompiler
    $canBuild = ($null -ne $compiler) -and (Test-Path -LiteralPath $script:SourceFile)

    if ($canBuild) {
        Write-Step ('building ' + $script:ExeName + ' with ' + $compiler)

        # Build into a scratch folder so a failed build never leaves a
        # half-written exe behind.
        $stage     = Join-Path ([IO.Path]::GetTempPath()) ('jarvishud-build-' + [Guid]::NewGuid().ToString('N'))
        $stagedExe = Join-Path $stage $script:ExeName

        try {
            New-Item -ItemType Directory -Path $stage -Force | Out-Null
            $command = Invoke-ExeBuild -Compiler $compiler -SourceFile $script:SourceFile -OutputExe $stagedExe

            if ($null -ne $command) {
                Copy-Item -LiteralPath $stagedExe -Destination $destination -Force
                Write-Ok ('built ' + $script:ExeName)
                Copy-RuntimeDependencies -ExePath $destination -Compiler $compiler
                Add-Summary ('built ' + $script:ExeName + ' from source')
            }
        }
        finally {
            Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
        }

        if ($null -ne $command) { return }
    }
    elseif ($null -eq $compiler) {
        Write-Warn 'no C++ compiler found'
    }
    else {
        Write-Warn ('source file not found: ' + $script:SourceFile)
    }

    # Fall back to the prebuilt executable in the project root.
    if (Test-Path -LiteralPath $prebuilt) {
        Write-Step ('no usable build - copying the prebuilt ' + $script:ExeName)
        Copy-Item -LiteralPath $prebuilt -Destination $destination -Force
        Write-Ok ('copied the prebuilt ' + $script:ExeName)
        if ($null -ne $compiler) {
            Copy-RuntimeDependencies -ExePath $destination -Compiler $compiler
        }
        Add-Summary ('copied the prebuilt ' + $script:ExeName)
        return
    }

    throw ('neither a working build nor a prebuilt ' + $script:ExeName +
           ' could be found. Install MSYS2 (ucrt64) or place a prebuilt ' +
           $script:ExeName + ' in ' + $script:ProjectRoot + ' and run again.')
}

# ---------------------------------------------------------------------------
# Runtime dependency check
# ---------------------------------------------------------------------------

function Copy-RuntimeDependencies {
    <#
        .SYNOPSIS
            Copies any non-system DLL the executable needs next to it.

        .DESCRIPTION
            A GUI program started at logon must not depend on the caller's
            PATH. The MSYS2 runtime (libstdc++-6.dll, libgcc_s_seh-1.dll)
            lives in the toolchain's bin folder, so those DLLs are copied
            into the install folder. The check uses objdump when it is
            available and is a best effort: a failure here never aborts
            the installation.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $ExePath,
        [Parameter(Mandatory = $true)][string] $Compiler
    )

    try {
        $objdump = Join-Path (Split-Path -Parent $Compiler) 'objdump.exe'
        if (-not (Test-Path -LiteralPath $objdump)) {
            $command = Get-Command 'objdump.exe' -ErrorAction SilentlyContinue
            if ($command) { $objdump = $command.Source }
        }
        if (-not (Test-Path -LiteralPath $objdump)) {
            Write-Info 'objdump is not available, skipping the runtime DLL check'
            return
        }

        $dump = & $objdump -p $ExePath 2>&1 | Out-String
        $names = New-Object System.Collections.Generic.List[string]
        foreach ($line in @($dump -split "`r?`n")) {
            if ($line -match 'DLL Name:\s*(\S+)') { $names.Add($Matches[1]) | Out-Null }
        }
        if ($names.Count -eq 0) { return }

        $toolchainBin = Split-Path -Parent $Compiler
        $systemDir   = Join-Path $env:SystemRoot 'System32'
        $copied      = 0

        foreach ($name in $names) {
            # The API-set forwarders are part of Windows itself.
            if ($name -like 'api-ms-*' -or $name -like 'ext-ms-*') { continue }
            if (Test-Path -LiteralPath (Join-Path $systemDir $name)) { continue }

            $source = Join-Path $toolchainBin $name
            if (-not (Test-Path -LiteralPath $source)) { continue }

            $target = Join-Path (Split-Path -Parent $ExePath) $name
            Copy-Item -LiteralPath $source -Destination $target -Force
            Write-Ok ('copied the runtime library ' + $name + ' next to the executable')
            $copied++
        }

        if ($copied -eq 0) {
            Write-Info 'the executable only needs system libraries'
        }
    }
    catch {
        Write-Warn ('the runtime library check failed: ' + $_.Exception.Message)
    }
}

# ---------------------------------------------------------------------------
# Autostart registration
# ---------------------------------------------------------------------------

function Set-Autostart {
    <#
        .SYNOPSIS
            Registers (or removes) the per-user autostart entry.

        .DESCRIPTION
            The value written is the quoted path of the executable, e.g.
            "C:\Users\you\AppData\Local\JarvisHud\JarvisHud.exe"
            Registering again with the same value changes nothing, so the
            entry is never duplicated.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $ExePath,
        [switch] $Remove
    )

    if (-not (Test-Path -LiteralPath $RunKeyPath)) {
        New-Item -Path $RunKeyPath -Force | Out-Null
    }

    $existing = Get-ItemProperty -Path $RunKeyPath -Name $script:RunValueName -ErrorAction SilentlyContinue
    $previous = ''
    if ($null -ne $existing) {
        $previous = [string](Get-ItemPropertyValue -Path $RunKeyPath -Name $script:RunValueName -ErrorAction SilentlyContinue)
    }

    if ($Remove) {
        if ($null -eq $existing) {
            Write-Ok 'no autostart entry was registered'
            Add-Summary 'no autostart entry was registered'
            return
        }
        Remove-ItemProperty -Path $RunKeyPath -Name $script:RunValueName -Force -ErrorAction SilentlyContinue
        Write-Ok ('removed the autostart entry "' + $script:RunValueName + '"')
        Write-Info ('was: ' + $previous)
        Add-Summary 'removed the autostart entry (because -NoAutoStart was given)'
        return
    }

    $quoted = '"' + $ExePath + '"'
    New-ItemProperty -Path $RunKeyPath -Name $script:RunValueName -Value $quoted -PropertyType String -Force | Out-Null

    if ($previous -eq $quoted) {
        Write-Ok ('the autostart entry "' + $script:RunValueName + '" is already correct')
    }
    else {
        Write-Ok ('registered the autostart entry "' + $script:RunValueName + '"')
    }
    Write-Info ('value: ' + $quoted)
    Add-Summary 'registered the autostart entry'
}

# ---------------------------------------------------------------------------
# Pre-login legal notice
# ---------------------------------------------------------------------------

function Get-ShippedGreeting {
    <#
        .SYNOPSIS
            Returns the first usable line of greetings.txt, with {name}
            substituted, for use as the initial legal notice text.
    #>
    $greetings = Join-Path $script:ProjectRoot 'greetings.txt'
    $config    = Join-Path $script:ProjectRoot 'config.txt'
    $name      = 'sir'

    if (Test-Path -LiteralPath $config) {
        foreach ($line in @(Get-Content -LiteralPath $config -ErrorAction SilentlyContinue)) {
            $trimmed = ([string]$line).Trim()
            if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
            if ($trimmed -match '(?i)^name\s*=\s*(.+)$') { $name = $Matches[1].Trim() }
        }
    }

    $fallback = 'Welcome back, ' + $name + '.'
    if (-not (Test-Path -LiteralPath $greetings)) { return $fallback }

    foreach ($line in @(Get-Content -LiteralPath $greetings -ErrorAction SilentlyContinue)) {
        $trimmed = ([string]$line).Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        return ($trimmed -replace '\{name\}', $name)
    }
    return $fallback
}

function Install-LegalNotice {
    param([Parameter(Mandatory = $true)][string] $ExePath)

    if (-not (Test-Path -LiteralPath $script:WinlogonKey)) {
        throw ('the Winlogon registry key was not found: ' + $script:WinlogonKey)
    }

    $text    = Get-ShippedGreeting
    $caption = $script:NoticeCaption

    New-ItemProperty -Path $script:WinlogonKey -Name 'LegalNoticeText'    -Value $text    -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $script:WinlogonKey -Name 'LegalNoticeCaption' -Value $caption -PropertyType String -Force | Out-Null
    Write-Ok 'wrote LegalNoticeText and LegalNoticeCaption'
    Write-Info ('caption: ' + $caption)
    Write-Info ('text   : ' + $text)
    Add-Summary 'set the pre-login legal notice (text and caption)'

    Install-NoticeTask -ExePath $ExePath
}

function Install-NoticeTask {
    <#
        .SYNOPSIS
            Creates (or refreshes) the "JarvisHudNotice" scheduled task.

        .DESCRIPTION
            Prefers the ScheduledTasks cmdlets. Falls back to schtasks.exe
            when they are unavailable. The quoting of the /TR switch for
            schtasks.exe is the backslash-escaped form, which is the only
            form that survives a path containing spaces.

            Task definition:
              name      : JarvisHudNotice
              action    : "<install path>\JarvisHud.exe" --set-notice
              trigger   : at log on
              principal : SYSTEM, run level highest
    #>
    param([Parameter(Mandatory = $true)][string] $ExePath)

    $hasCmdlets = $null -ne (Get-Command 'Register-ScheduledTask' -ErrorAction SilentlyContinue)

    if ($hasCmdlets) {
        try {
            $taskAction    = New-ScheduledTaskAction -Execute $ExePath -Argument '--set-notice'
            $taskTrigger   = New-ScheduledTaskTrigger -AtLogOn
            $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
            $taskSettings  = New-ScheduledTaskSettingsSet `
                                -AllowStartIfOnBatteries `
                                -DontStopIfGoingOnBatteries `
                                -StartWhenAvailable `
                                -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
                                -MultipleInstances IgnoreNew

            Register-ScheduledTask `
                -TaskName $script:TaskName `
                -Action $taskAction `
                -Trigger $taskTrigger `
                -Principal $taskPrincipal `
                -Settings $taskSettings `
                -Description 'Writes the JARVIS greeting to the Winlogon LegalNoticeText value at logon.' `
                -Force | Out-Null

            Write-Ok ('created the scheduled task "' + $script:TaskName + '"')
            Write-Info  'trigger: at log on | run level: highest | account: SYSTEM'
            Add-Summary 'created the scheduled task "' + $script:TaskName + '"'
            return
        }
        catch {
            Write-Warn ('Register-ScheduledTask failed: ' + $_.Exception.Message)
            Write-Info  'falling back to schtasks.exe'
        }
    }
    else {
        Write-Info 'the ScheduledTasks cmdlets are not available, using schtasks.exe'
    }

    # schtasks.exe fallback. The /TR value keeps the quotes around the
    # executable path; the backslashes are what schtasks expects.
    $taskRun = '\"' + $ExePath + '\" --set-notice'
    $log = & schtasks.exe /Create /TN $script:TaskName /TR $taskRun /SC ONLOGON /RU SYSTEM /RL HIGHEST /F 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw ('schtasks.exe could not create "' + $script:TaskName + '": ' + $log.Trim())
    }

    Write-Ok ('created the scheduled task "' + $script:TaskName + '" with schtasks.exe')
    Write-Info  'trigger: at log on | run level: highest | account: SYSTEM'
    Add-Summary 'created the scheduled task "' + $script:TaskName + '"'
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

trap {
    Write-Failure -ErrorRecord $_
    exit 1
}

try {
    Write-Host ''
    Write-Host '  J A R V I S   H U D   -   installer' -ForegroundColor Cyan
    Write-Host '  ------------------------------------' -ForegroundColor DarkCyan
    Write-Host ''

    if ([string]::IsNullOrWhiteSpace($InstallPath)) {
        throw 'the install path must not be empty'
    }
    $InstallPath = [IO.Path]::GetFullPath($InstallPath)

    # The legal notice needs administrator rights. Check this up front so
    # the script cannot half-install before failing.
    if ($LegalNotice -and -not (Test-Administrator)) {
        Write-Host ''
        Write-Host 'ERROR: -LegalNotice needs an elevated session.' -ForegroundColor Red
        Write-Host '       Start Windows PowerShell with "Run as administrator" and' -ForegroundColor Red
        Write-Host '       run this script again.' -ForegroundColor Red
        exit 2
    }

    # -- install folder ----------------------------------------------------
    Write-Step ('install folder: ' + $InstallPath)
    if (-not (Test-Path -LiteralPath $InstallPath)) {
        New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
        Write-Ok 'created the install folder'
    }
    else {
        Write-Ok 'the install folder already exists'
    }
    $exePath = Join-Path $InstallPath $script:ExeName

    # -- executable --------------------------------------------------------
    Install-Executable -DestinationDir $InstallPath

    # -- data files --------------------------------------------------------
    Write-Step 'data files'
    Install-DataFile -Source (Join-Path $script:ProjectRoot 'greetings.txt') `
                     -Destination (Join-Path $InstallPath 'greetings.txt') `
                     -Label 'greetings.txt'
    Install-DataFile -Source (Join-Path $script:ProjectRoot 'config.txt') `
                     -Destination (Join-Path $InstallPath 'config.txt') `
                     -Label 'config.txt'

    # -- autostart ---------------------------------------------------------
    Write-Step 'autostart'
    if ($NoAutoStart) {
        Set-Autostart -ExePath $exePath -Remove
        Write-Info 'autostart is disabled (-NoAutoStart)'
    }
    else {
        Set-Autostart -ExePath $exePath
    }

    # -- legal notice ------------------------------------------------------
    if ($LegalNotice) {
        Write-Step 'pre-login legal notice'
        Install-LegalNotice -ExePath $exePath
    }
    else {
        Write-Info 'the pre-login legal notice was not installed (use -LegalNotice)'
    }

    # -- summary -----------------------------------------------------------
    Write-Host ''
    Write-Host '  Summary' -ForegroundColor Cyan
    Write-Host '  -------' -ForegroundColor DarkCyan
    foreach ($line in $script:Summary) {
        Write-Host ('  * ' + $line) -ForegroundColor Gray
    }

    Write-Host ''
    Write-Host '  How to test' -ForegroundColor Cyan
    Write-Host '  ------------' -ForegroundColor DarkCyan
    Write-Host ('  1. Run it now     :  & "' + $exePath + '"') -ForegroundColor Gray
    Write-Host ('  2. Self test      :  & "' + $exePath + '" --selftest --log "$env:TEMP\jarvis-selftest.log"') -ForegroundColor Gray
    Write-Host ('  3. Fixed lifetime :  & "' + $exePath + '" --duration 4000') -ForegroundColor Gray
    Write-Host ('  4. Log on again to see the greeting at login.') -ForegroundColor Gray
    Write-Host ('  5. Remove it all  :  powershell -ExecutionPolicy Bypass -File "' + (Join-Path $script:ProjectRoot 'uninstall.ps1') + '" -RemoveFiles') -ForegroundColor Gray
    Write-Host ''

    exit 0
}
catch {
    Write-Failure -ErrorRecord $_
    exit 1
}
