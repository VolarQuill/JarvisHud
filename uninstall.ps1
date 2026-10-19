<#
.SYNOPSIS
    Removes the JARVIS HUD login greeting.

.DESCRIPTION
    Removes the per-user autostart entry, the "JarvisHudNotice" scheduled
    task, the pre-login legal notice values and - with -RemoveFiles - the
    install folder.

    The script is idempotent: every step tolerates the item already being
    gone, so running it twice is safe and never raises an error.

.PARAMETER InstallPath
    The folder the HUD was installed into. Defaults to
    "$env:LOCALAPPDATA\JarvisHud". Only used by -RemoveFiles.

.PARAMETER RemoveFiles
    Also delete the install folder. The folder is only removed when it
    looks like a JarvisHud install folder, so a wrong path cannot wipe
    an unrelated directory.

.PARAMETER KeepLegalNotice
    Leave the Winlogon LegalNoticeText / LegalNoticeCaption values in
    place. Useful when the legal notice was set for something else.

.NOTES
    Exit codes
    ----------
    0   success
    1   an error occurred (a readable message is printed first)

.EXAMPLE
    .\uninstall.ps1
    Removes the autostart entry, the scheduled task and the legal notice.

.EXAMPLE
    .\uninstall.ps1 -RemoveFiles
    Also deletes the install folder.
#>

[CmdletBinding()]
param(
    [string] $InstallPath = (Join-Path $env:LOCALAPPDATA 'JarvisHud'),
    [switch] $RemoveFiles,
    [switch] $KeepLegalNotice
)

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$script:AppName      = 'JarvisHud'
$script:ExeName      = 'JarvisHud.exe'
$script:RunKeyPath   = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$script:RunValueName = 'JarvisHud'
$script:TaskName     = 'JarvisHudNotice'
$script:WinlogonKey  = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
$script:Summary      = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------
# Output helpers
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

function Test-Administrator {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
# Removal steps
# ---------------------------------------------------------------------------

function Remove-Autostart {
    if (-not (Test-Path -LiteralPath $RunKeyPath)) {
        Write-Ok 'the autostart key does not exist, nothing to remove'
        Add-Summary 'autostart key was not present'
        return
    }

    $existing = Get-ItemProperty -Path $RunKeyPath -Name $script:RunValueName -ErrorAction SilentlyContinue
    if ($null -eq $existing) {
        Write-Ok 'no autostart entry was registered'
        Add-Summary 'no autostart entry was registered'
        return
    }

    $previous = ''
    $previous = [string](Get-ItemPropertyValue -Path $RunKeyPath -Name $script:RunValueName -ErrorAction SilentlyContinue)

    Remove-ItemProperty -Path $RunKeyPath -Name $script:RunValueName -Force -ErrorAction SilentlyContinue
    Write-Ok ('removed the autostart entry "' + $script:RunValueName + '"')
    Write-Info ('was: ' + $previous)
    Add-Summary 'removed the autostart entry'
}

function Remove-NoticeTask {
    $hasCmdlets = $null -ne (Get-Command 'Get-ScheduledTask' -ErrorAction SilentlyContinue)

    if ($hasCmdlets) {
        $task = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
        if ($task) {
            Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false -ErrorAction SilentlyContinue
            if ($null -eq (Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue)) {
                Write-Ok ('removed the scheduled task "' + $script:TaskName + '"')
                Add-Summary 'removed the scheduled task'
                return
            }
            Write-Warn ('the scheduled task "' + $script:TaskName + '" could not be removed')
        }
        else {
            Write-Ok 'no scheduled task was registered'
            Add-Summary 'no scheduled task was registered'
            return
        }
    }

    # Fallback for machines without the ScheduledTasks cmdlets.
    $log = & schtasks.exe /Query /TN $script:TaskName 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        $log = & schtasks.exe /Delete /TN $script:TaskName /F 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw ('schtasks.exe could not delete "' + $script:TaskName + '": ' + $log.Trim())
        }
        Write-Ok ('removed the scheduled task "' + $script:TaskName + '" with schtasks.exe')
        Add-Summary 'removed the scheduled task'
    }
    else {
        Write-Ok 'no scheduled task was registered'
        Add-Summary 'no scheduled task was registered'
    }
}

function Remove-LegalNotice {
    if ($KeepLegalNotice) {
        Write-Info 'the legal notice was left in place (-KeepLegalNotice)'
        Add-Summary 'kept the legal notice (-KeepLegalNotice)'
        return
    }

    if (-not (Test-Path -LiteralPath $script:WinlogonKey)) {
        Write-Warn ('the Winlogon key was not found: ' + $script:WinlogonKey)
        Add-Summary 'the Winlogon key was not found'
        return
    }

    $removed = $false
    foreach ($valueName in @('LegalNoticeText', 'LegalNoticeCaption')) {
        $existing = Get-ItemProperty -Path $script:WinlogonKey -Name $valueName -ErrorAction SilentlyContinue
        if ($null -ne $existing) {
            if (Test-Administrator) {
                Remove-ItemProperty -Path $script:WinlogonKey -Name $valueName -Force -ErrorAction SilentlyContinue
                if ($null -eq (Get-ItemProperty -Path $script:WinlogonKey -Name $valueName -ErrorAction SilentlyContinue)) {
                    Write-Ok ('removed ' + $valueName)
                    $removed = $true
                }
                else {
                    Write-Warn ('could not remove ' + $valueName)
                }
            }
            else {
                Write-Warn ($valueName + ' needs an elevated session; run this script as administrator to remove it')
            }
        }
    }

    if ($removed) {
        Add-Summary 'removed the legal notice text and caption'
    }
    elseif (-not (Test-Administrator)) {
        Add-Summary 'the legal notice could not be removed without administrator rights'
    }
    else {
        Write-Ok 'no legal notice values were present'
        Add-Summary 'no legal notice values were present'
    }
}

function Stop-HudProcess {
    <#
        .SYNOPSIS
            Stops a running JarvisHud.exe so the install folder can be
            deleted without file-lock errors.
    #>
    param([Parameter(Mandatory = $true)][string] $Folder)

    $running = @()
    try {
        $running = Get-Process -Name 'JarvisHud' -ErrorAction SilentlyContinue
    }
    catch {
        return
    }
    if (-not $running) { return }

    foreach ($proc in $running) {
        $path = ''
        try { $path = $proc.Path } catch { $path = '' }
        if ($path -and ($path.ToLower().StartsWith($Folder.ToLower()))) {
            try {
                Stop-Process -Id $proc.Id -Force -ErrorAction Stop
                Write-Ok ('stopped the running ' + $script:ExeName + ' (pid ' + $proc.Id + ')')
            }
            catch {
                Write-Warn ('could not stop the running ' + $script:ExeName + ' (pid ' + $proc.Id + ')')
            }
        }
    }
}

function Remove-InstallFolder {
    param([Parameter(Mandatory = $true)][string] $Folder)

    if (-not (Test-Path -LiteralPath $Folder)) {
        Write-Ok 'the install folder does not exist'
        Add-Summary 'the install folder was not present'
        return
    }

    # Safety net: only delete a folder that actually looks like a
    # JarvisHud installation.
    $marker = Join-Path $Folder $script:ExeName
    if (-not (Test-Path -LiteralPath $marker)) {
        Write-Warn ('"' + $Folder + '" does not contain ' + $script:ExeName + ' - not removing it')
        Add-Summary 'the install folder was left alone (no ' + $script:ExeName + ' inside)'
        return
    }

    Stop-HudProcess -Folder $Folder

    try {
        Remove-Item -LiteralPath $Folder -Recurse -Force
        Write-Ok ('removed the install folder: ' + $Folder)
        Add-Summary 'removed the install folder'
    }
    catch {
        Write-Warn ('could not remove "' + $Folder + '": ' + $_.Exception.Message)
        Add-Summary 'the install folder could not be removed'
    }
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
    Write-Host '  J A R V I S   H U D   -   uninstaller' -ForegroundColor Cyan
    Write-Host '  --------------------------------------' -ForegroundColor DarkCyan
    Write-Host ''

    if ([string]::IsNullOrWhiteSpace($InstallPath)) {
        throw 'the install path must not be empty'
    }
    $InstallPath = [IO.Path]::GetFullPath($InstallPath)

    Write-Step 'autostart'
    Remove-Autostart

    Write-Step 'scheduled task'
    Remove-NoticeTask

    Write-Step 'pre-login legal notice'
    Remove-LegalNotice

    Write-Step 'install folder'
    if ($RemoveFiles) {
        Remove-InstallFolder -Folder $InstallPath
    }
    else {
        Write-Info ('kept the install folder: ' + $InstallPath)
        Write-Info 'use -RemoveFiles to delete it'
        Add-Summary 'kept the install folder'
    }

    Write-Host ''
    Write-Host '  Summary' -ForegroundColor Cyan
    Write-Host '  -------' -ForegroundColor DarkCyan
    foreach ($line in $script:Summary) {
        Write-Host ('  * ' + $line) -ForegroundColor Gray
    }
    Write-Host ''
    Write-Host '  The JARVIS HUD has been removed.' -ForegroundColor Green
    Write-Host ''

    exit 0
}
catch {
    Write-Failure -ErrorRecord $_
    exit 1
}
