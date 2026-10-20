#  A screenshot of the whole screen, in physical pixels.
param([string]$Out = "$env:TEMP\jarvis-desktop.png")
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace W -Name U -MemberDefinition @'
    [DllImport("user32.dll", EntryPoint = "SetProcessDpiAwarenessContext")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
'@
[void][W.U]::SetProcessDpiAwarenessContext([IntPtr](-4))
$bmp = New-Object System.Drawing.Bitmap 1920, 1080
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out"
