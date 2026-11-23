$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$sizes=@(16,20,24,32,40,48,64,128,256)
$report=@()
foreach($kind in @('video','image','model')) {
  $master=[System.Drawing.Bitmap]::new((Join-Path $PSScriptRoot "formaview-$kind-master.png"))
  $entries=@()
  foreach($size in ($sizes+512)) {
    $bitmap=[System.Drawing.Bitmap]::new($size,$size,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics=[System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CompositingMode=[System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode=[System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($master,[System.Drawing.Rectangle]::new(0,0,$size,$size))
    $graphics.Dispose()
    $path=Join-Path $PSScriptRoot "formaview-$kind-$size.png"
    $bitmap.Save($path,[System.Drawing.Imaging.ImageFormat]::Png)
    if($bitmap.GetPixel(0,0).A -ne 0 -or $bitmap.GetPixel($size-1,$size-1).A -ne 0){throw 'Transparent corner check failed'}
    $bitmap.Dispose()
    if($size -le 256){$entries+=@{size=$size;data=[System.IO.File]::ReadAllBytes($path)}}
  }
  $iconPath=Join-Path $PSScriptRoot "formaview-$kind.ico"
  $stream=[System.IO.File]::Create($iconPath)
  $writer=[System.IO.BinaryWriter]::new($stream)
  $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$entries.Count)
  $offset=6+16*$entries.Count
  foreach($entry in $entries){
    $dim=if($entry.size -eq 256){0}else{$entry.size}
    $writer.Write([byte]$dim);$writer.Write([byte]$dim);$writer.Write([byte]0);$writer.Write([byte]0)
    $writer.Write([uint16]1);$writer.Write([uint16]32)
    $writer.Write([uint32]$entry.data.Length);$writer.Write([uint32]$offset)
    $offset+=$entry.data.Length
  }
  foreach($entry in $entries){$writer.Write([byte[]]$entry.data)}
  $writer.Dispose();$stream.Dispose()
  $report+=@{kind=$kind;master=@($master.Width,$master.Height);sizes=$sizes;sha256=(Get-FileHash -LiteralPath $iconPath -Algorithm SHA256).Hash.ToLower();bytes=(Get-Item -LiteralPath $iconPath).Length}
  $master.Dispose()
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 5
