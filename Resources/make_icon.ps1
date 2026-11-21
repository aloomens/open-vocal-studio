# Draws the Open Vocal Studio icon (studio microphone on a dark tile) and writes icon.png and Plugin.ico.
# Usage: pwsh -File Resources\make_icon.ps1
Add-Type -AssemblyName System.Drawing
$here = $PSScriptRoot

function Draw-Icon([int]$Size) {
    $s = $Size / 512.0
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    function C([string]$hex, [int]$a = 255) { $c = [System.Drawing.ColorTranslator]::FromHtml($hex); [System.Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B) }
    function RR([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath; $d = 2 * $r
        $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
        $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
        $p.CloseFigure(); return $p
    }

    # tile
    $tile = RR (16*$s) (16*$s) (480*$s) (480*$s) (104*$s)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 16*$s)), ([System.Drawing.PointF]::new(0, 496*$s)), (C "#2d2f35"), (C "#131417")
    $g.FillPath($bg, $tile)
    $g.DrawPath((New-Object System.Drawing.Pen (C "#44474e"), (4*$s)), $tile)

    # sound arcs either side (coral)
    $arc = New-Object System.Drawing.Pen (C "#e39a6b"), (22*$s)
    $arc.StartCap = $arc.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($arc, [System.Drawing.RectangleF]::new(70*$s, 96*$s, 372*$s, 300*$s), 150, 60)
    $g.DrawArc($arc, [System.Drawing.RectangleF]::new(70*$s, 96*$s, 372*$s, 300*$s), -30, 60)
    $arc2 = New-Object System.Drawing.Pen (C "#e39a6b" 140), (18*$s)
    $arc2.StartCap = $arc2.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($arc2, [System.Drawing.RectangleF]::new(36*$s, 66*$s, 440*$s, 360*$s), 152, 56)
    $g.DrawArc($arc2, [System.Drawing.RectangleF]::new(36*$s, 66*$s, 440*$s, 360*$s), -28, 56)

    # yoke and stand
    $yoke = New-Object System.Drawing.Pen (C "#c9c4ba"), (20*$s)
    $yoke.StartCap = $yoke.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($yoke, [System.Drawing.RectangleF]::new(160*$s, 170*$s, 192*$s, 180*$s), 0, 180)
    $g.DrawLine($yoke, 256*$s, 350*$s, 256*$s, 418*$s)
    $g.DrawLine($yoke, 196*$s, 422*$s, 316*$s, 422*$s)

    # capsule (head)
    $head = RR (190*$s) (86*$s) (132*$s) (206*$s) (66*$s)
    $hb = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(190*$s, 0)), ([System.Drawing.PointF]::new(322*$s, 0)), (C "#f0b48d"), (C "#b8714a")
    $g.FillPath($hb, $head)
    # grille
    $gr = New-Object System.Drawing.Pen (C "#5a3423" 170), (6*$s)
    foreach ($yy in 118, 142, 166, 190, 214, 238) { $g.DrawLine($gr, 204*$s, $yy*$s, 308*$s, $yy*$s) }
    $g.DrawPath((New-Object System.Drawing.Pen (C "#000000" 120), (4*$s)), $head)

    $g.Dispose()
    return $bmp
}

$png = Join-Path $here 'icon.png'
$b = Draw-Icon 1024; $b.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()

# .ico with PNG-compressed images (256, 64, 48, 32, 16)
$sizes = 256, 64, 48, 32, 16
$blobs = foreach ($sz in $sizes) { $bm = Draw-Icon $sz; $ms = New-Object System.IO.MemoryStream; $bm.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bm.Dispose(); , $ms.ToArray() }
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]; $len = $blobs[$i].Length
    $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz }))); $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$len); $w.Write([UInt32]$offset); $offset += $len
}
foreach ($bl in $blobs) { $w.Write($bl) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $here 'Plugin.ico'), $out.ToArray())
"wrote icon.png and Plugin.ico"
