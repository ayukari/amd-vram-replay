# Draws the VramReplay icon (dark disc, light ring, red dot) and writes res/app.ico with PNG-compressed sizes.
param([string]$Out = "$PSScriptRoot\app.ico")
Add-Type -AssemblyName System.Drawing
$sizes = 16, 24, 32, 48, 64, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $pad = $s * 0.02
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 32, 36))), $pad, $pad, $s - 2 * $pad, $s - 2 * $pad)
    $w = [Math]::Max(1.0, $s * 0.07)
    $r = $s * 0.40 - $w / 2
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 230, 232, 236)), $w
    $g.DrawEllipse($pen, $s / 2 - $r, $s / 2 - $r, 2 * $r, 2 * $r)
    $d = $s * 0.26
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 235, 64, 64))), $s / 2 - $d, $s / 2 - $d, 2 * $d, 2 * $d)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
"wrote $Out"
