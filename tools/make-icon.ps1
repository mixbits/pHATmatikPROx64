# Builds a multi-size Windows icon from a PNG, so that the small sizes used in title bars and on
# the taskbar are properly resampled instead of being scaled down by Windows at run time.
#   .\tools\make-icon.ps1 -Source images\app-icon.png -Output build\obj\app.ico
param(
    [Parameter(Mandatory)] [string]$Source,
    [Parameter(Mandatory)] [string]$Output,
    [int[]]$Sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source))
$entries = @()
foreach ($size in $Sizes) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $attr = New-Object System.Drawing.Imaging.ImageAttributes
    $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)   # no dark fringe at the edges
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $size, $size), 0, 0, $src.Width, $src.Height, [System.Drawing.GraphicsUnit]::Pixel, $attr)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    if ($size -ge 256) {
        # Large images are stored as PNG, as Windows expects.
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        # Smaller ones as an uncompressed 32-bit bitmap: header, pixel rows bottom-up, 1-bit mask.
        $w = New-Object System.IO.BinaryWriter $ms
        $w.Write([int]40); $w.Write([int]$size); $w.Write([int]($size * 2)); $w.Write([int16]1); $w.Write([int16]32)
        $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0)
        $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $row = New-Object byte[] ($size * 4)
        for ($y = $size - 1; $y -ge 0; $y--) {
            [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $row, 0, $row.Length)
            $w.Write($row)
        }
        $bmp.UnlockBits($data)
        $maskRow = New-Object byte[] ([int]([Math]::Ceiling($size / 32.0)) * 4)
        for ($y = 0; $y -lt $size; $y++) { $w.Write($maskRow) }
        $w.Flush()
    }
    $entries += , @{ Size = $size; Bytes = $ms.ToArray() }
    $bmp.Dispose()
}
$src.Dispose()

New-Item -ItemType Directory -Force (Split-Path $Output -Parent) | Out-Null
$out = New-Object System.IO.BinaryWriter ([System.IO.File]::Create($Output))
$out.Write([int16]0); $out.Write([int16]1); $out.Write([int16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
    $out.Write([byte]$dim); $out.Write([byte]$dim); $out.Write([byte]0); $out.Write([byte]0)
    $out.Write([int16]1); $out.Write([int16]32); $out.Write([int]$e.Bytes.Length); $out.Write([int]$offset)
    $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $out.Write([byte[]]$e.Bytes) }
$out.Close()
Write-Host ("{0}: {1} sizes, {2:N0} bytes" -f $Output, $entries.Count, (Get-Item $Output).Length)
