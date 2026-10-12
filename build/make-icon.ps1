<#
.SYNOPSIS
  Generates the application icon (app.ico) used by the control panel and the installer.
  Draws a simple headset glyph with GDI+ at 16/24/32/48/256 px and packs the PNG frames
  into one .ico so that the repository needs no binary art assets.

.USAGE
  .\build\make-icon.ps1                       # writes tools\pna2dp-panel\app.ico and tools\pna2dp-setup\app.ico
#>
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function New-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 48.0
    # rounded background
    $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = 10 * $s; $w = $size - 1
    $bg.AddArc(0, 0, $r, $r, 180, 90); $bg.AddArc($w - $r, 0, $r, $r, 270, 90); $bg.AddArc($w - $r, $w - $r, $r, $r, 0, 90); $bg.AddArc(0, $w - $r, $r, $r, 90, 90); $bg.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point $size, $size), ([System.Drawing.Color]::FromArgb(255, 0, 120, 212)), ([System.Drawing.Color]::FromArgb(255, 0, 70, 140))
    $g.FillPath($brush, $bg)
    # headband (arc) + two ear cups
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1.5, 4.5 * $s))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawArc($pen, 9 * $s, 9 * $s, 30 * $s, 30 * $s, 190, 160)
    $white = [System.Drawing.Brushes]::White
    $g.FillRectangle($white, 7 * $s, 24 * $s, 9 * $s, 14 * $s)
    $g.FillRectangle($white, 32 * $s, 24 * $s, 9 * $s, 14 * $s)
    # bluetooth-ish wave at the bottom
    $pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 120, 200, 255)), ([Math]::Max(1, 2.5 * $s))
    $g.DrawArc($pen2, 17 * $s, 30 * $s, 14 * $s, 14 * $s, 20, 140)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = @(16, 24, 32, 48, 256)
$frames = @()
foreach ($sz in $sizes) { $frames += ,(New-Frame $sz) }

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]; $data = $frames[$i]
    $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz }))); $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($data in $frames) { $bw.Write($data) }
$bw.Flush()
$bytes = $ms.ToArray()

$targets = if ($Out) { @($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)) } else { @((Join-Path $Root 'tools\pna2dp-panel\app.ico'), (Join-Path $Root 'tools\pna2dp-setup\app.ico')) }
foreach ($t in $targets) {
    $dir = Split-Path $t
    if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllBytes($t, $bytes)
    Write-Host "wrote $t ($($bytes.Length) bytes)"
}
