# Draws the project's own icon: a red waveform on dark blue, with two slice markers.
# It is drawn here from scratch and contains no iZotope artwork, name or logo, so it can go on
# anything this project produces.   .\tools\draw-app-icon.ps1 -Output images\app-icon.png
param([Parameter(Mandatory)] [string]$Output, [int]$Size = 512)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$s = $Size / 512.0
$bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.Clear([System.Drawing.Color]::Transparent)

function RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Background: a rounded square, dark blue, lighter towards the top left.
$bg = RoundedRect (16 * $s) (16 * $s) (480 * $s) (480 * $s) (88 * $s)
$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $Size, $Size),
    ([System.Drawing.Color]::FromArgb(255, 30, 70, 140)), ([System.Drawing.Color]::FromArgb(255, 8, 20, 52))
$g.FillPath($grad, $bg)
$edge = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(90, 150, 190, 240)), (4 * $s)
$g.DrawPath($edge, $bg)

# Slice markers: thin light-blue lines with a small flag at the top.
$markPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 130, 200, 235)), (5 * $s)
$markBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 130, 200, 235))
foreach ($mx in 139, 323) {
    $g.DrawLine($markPen, [single]($mx * $s), [single](84 * $s), [single]($mx * $s), [single](428 * $s))
    $flag = RoundedRect (($mx - 15) * $s) (66 * $s) (30 * $s) (26 * $s) (6 * $s)
    $g.FillPath($markBrush, $flag)
}

# Waveform: red bars mirrored about the centre line, shaped like a drum hit and its tail.
$red = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 235, 38, 46))
$heights = 0.16, 0.30, 1.00, 0.62, 0.80, 0.42, 0.54, 0.26, 0.14
$barW = 30; $gap = 16; $x = 56; $half = 168
foreach ($h in $heights) {
    $bh = [Math]::Max(10, $h * $half)
    $bar = RoundedRect ($x * $s) ((256 - $bh) * $s) ($barW * $s) (2 * $bh * $s) ([Math]::Min(9, $bh) * $s)
    $g.FillPath($red, $bar)
    $x += $barW + $gap
}
$g.Dispose()
New-Item -ItemType Directory -Force (Split-Path (Join-Path (Get-Location) $Output) -Parent) -ErrorAction SilentlyContinue | Out-Null
$bmp.Save((Join-Path (Get-Location) $Output), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "$Output written ($Size x $Size)"
