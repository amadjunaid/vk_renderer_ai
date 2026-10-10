# Captures the renderer's window to a PNG and samples known pixel positions, so
# rendering changes can be verified without eyeballing a screenshot.
#
#   powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1
#   powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 -Title "My App" -Out out.png
#
# Uses PrintWindow(PW_RENDERFULLCONTENT) which reads the DWM backing store, so it
# works even when the window is occluded or not focused.
param(
    [string]$Title = "Vulkan Triangle",
    [string]$ProcessName = "vk_triangle",
    [string]$Out = "build/capture_window.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT pt);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@

# Without this, GetClientRect returns DPI-virtualized coordinates and the capture
# would only cover the top-left corner of the real framebuffer.
[Cap]::SetProcessDPIAware() | Out-Null

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -eq $Title } |
    Select-Object -First 1
if (-not $proc) { Write-Output "PROCESS_NOT_FOUND ($ProcessName / '$Title')"; exit 1 }

$handle = $proc.MainWindowHandle
Write-Output ("handle: 0x{0:X}" -f $handle.ToInt64())

$client = New-Object Cap+RECT
[Cap]::GetClientRect($handle, [ref]$client) | Out-Null
$clientWidth = $client.Right - $client.Left
$clientHeight = $client.Bottom - $client.Top

$window = New-Object Cap+RECT
[Cap]::GetWindowRect($handle, [ref]$window) | Out-Null
$windowWidth = $window.Right - $window.Left
$windowHeight = $window.Bottom - $window.Top

$clientOrigin = New-Object Cap+POINT
[Cap]::ClientToScreen($handle, [ref]$clientOrigin) | Out-Null

# The captured image includes the title bar, so client coordinates are shifted.
$offsetX = $clientOrigin.X - $window.Left
$offsetY = $clientOrigin.Y - $window.Top

Write-Output ("client: {0}x{1}  window: {2}x{3}  client offset: {4},{5}" -f `
        $clientWidth, $clientHeight, $windowWidth, $windowHeight, $offsetX, $offsetY)

$bmp = New-Object System.Drawing.Bitmap($windowWidth, $windowHeight)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
$ok = [Cap]::PrintWindow($handle, $hdc, 2)        # PW_RENDERFULLCONTENT
$gfx.ReleaseHdc($hdc)
$gfx.Dispose()

$outPath = [System.IO.Path]::GetFullPath($Out)
$outDir = [System.IO.Path]::GetDirectoryName($outPath)
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "PrintWindow -> $ok, saved to $outPath"

function Read-ClientPixel($bitmap, [int]$x, [int]$y) {
    $c = $bitmap.GetPixel($offsetX + $x, $offsetY + $y)
    return "$($c.R),$($c.G),$($c.B)"
}

# Sample points in client (framebuffer) coordinates. For the default triangle:
# apex at 25% height, base at 75%, spanning 25%-75% horizontally.
Write-Output ("background      (8,8):        {0}" -f (Read-ClientPixel $bmp 8 8))
Write-Output ("apex      (50%, 27%):        {0}" -f (Read-ClientPixel $bmp ([int]($clientWidth * 0.50)) ([int]($clientHeight * 0.27))))
Write-Output ("centroid  (50%, 58%):        {0}" -f (Read-ClientPixel $bmp ([int]($clientWidth * 0.50)) ([int]($clientHeight * 0.58))))
Write-Output ("base left (30%, 72%):        {0}" -f (Read-ClientPixel $bmp ([int]($clientWidth * 0.30)) ([int]($clientHeight * 0.72))))
Write-Output ("base right(70%, 72%):        {0}" -f (Read-ClientPixel $bmp ([int]($clientWidth * 0.70)) ([int]($clientHeight * 0.72))))
$bmp.Dispose()
