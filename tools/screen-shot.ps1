# 从设备抓一张屏幕截图，存成 PNG
#
# 依赖固件里的 GET /api/screen.bmp 接口（LVGL lv_snapshot_take 实现）。
# 调眼睛动画 / 界面布局时用它"照着看"，不用靠猜。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File screen-shot.ps1
#   powershell -ExecutionPolicy Bypass -File screen-shot.ps1 -Device 192.168.1.11 -Out shot.png

param(
    [string]$Device = "192.168.1.11",
    [string]$Out = "screen.png",
    [int]$Scale = 2
)

Add-Type -AssemblyName System.Drawing

$url = "http://$Device/api/screen.bmp"
$tmp = [System.IO.Path]::GetTempFileName() + ".bmp"

Write-Host "抓屏 $url ..." -ForegroundColor Cyan
try {
    Invoke-WebRequest -Uri $url -OutFile $tmp -TimeoutSec 60 -UseBasicParsing
}
catch {
    Write-Error "抓屏失败：$($_.Exception.Message)"
    Write-Host "  检查：设备是否开机、是否和电脑在同一 WiFi、固件是否含 /api/screen.bmp" -ForegroundColor Yellow
    exit 1
}

$size = (Get-Item $tmp).Length
if ($size -lt 1000) {
    Write-Error "返回内容太小（$size 字节），可能不是 BMP"
    exit 1
}

try {
    $bmp = [System.Drawing.Image]::FromFile($tmp)
    $w = $bmp.Width * $Scale
    $h = $bmp.Height * $Scale
    $big = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($bmp, 0, 0, $w, $h)
    $g.Dispose()
    $big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $big.Dispose()
    $srcW = $bmp.Width
    $srcH = $bmp.Height
    $bmp.Dispose()
    Write-Host "已保存 $Out  (${srcW}x${srcH} 放大 ${Scale}x)" -ForegroundColor Green
}
finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
