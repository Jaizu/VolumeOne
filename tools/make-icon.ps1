# Generates res\app.ico: a Fluent-style rounded tile with the Segoe Fluent
# Icons "Volume3" glyph. Run once; the .ico is committed.
param([string]$Out = "$PSScriptRoot\..\res\app.ico")

Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(2.0, $s * 0.22)
    $inset = [Math]::Max(0.5, $s * 0.03)
    $rect = New-Object System.Drawing.RectangleF $inset, $inset, ($s - 2 * $inset), ($s - 2 * $inset)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect,
        ([System.Drawing.Color]::FromArgb(255, 0x4C, 0xC2, 0xFF)),
        ([System.Drawing.Color]::FromArgb(255, 0x00, 0x5F, 0xB8)), 60.0
    $g.FillPath($brush, $path)

    $fontPx = $s * 0.58
    $font = New-Object System.Drawing.Font "Segoe Fluent Icons", $fontPx, ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'
    $fmt.LineAlignment = 'Center'
    $glyph = [string][char]0xE995
    $g.DrawString($glyph, $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, ($s * 0.02), $s, $s), $fmt)

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $g.Dispose(); $bmp.Dispose()
}

$icoStream = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $icoStream
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$target = [System.IO.Path]::GetFullPath($Out)
[System.IO.File]::WriteAllBytes($target, $icoStream.ToArray())
Write-Host "Wrote $Out"
