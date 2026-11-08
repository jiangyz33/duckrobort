# 把你的音乐批量转成设备能播的 OGG/Opus
#
# 用法：
#   1. 把 mp3/flac/m4a 放进一个文件夹
#   2. powershell -ExecutionPolicy Bypass -File convert-music.ps1 -Src "D:\我的音乐"
#   3. 生成到 .\out\ 下，再整目录传服务器 data/music/
#
# 设备端 NotifyPlayer 只认 OGG/Opus，所以必须先转码。
# 码率 48k + 48kHz 单声道已经够用（喇叭本身也是单声道）。

param(
    [Parameter(Mandatory = $true)][string]$Src,
    [string]$Out = ".\out",
    [int]$Bitrate = 48
)

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "找不到 ffmpeg，请先安装并加入 PATH"
    exit 1
}
if (-not (Test-Path $Src)) {
    Write-Error "源目录不存在: $Src"
    exit 1
}

New-Item -ItemType Directory -Path $Out -Force | Out-Null

$exts = @('.mp3', '.flac', '.m4a', '.wav', '.aac', '.wma')
$files = Get-ChildItem -Path $Src -Recurse -File |
    Where-Object { $exts -contains $_.Extension.ToLower() }

if ($files.Count -eq 0) {
    Write-Host "没找到音频文件" -ForegroundColor Yellow
    exit 0
}

Write-Host "共 $($files.Count) 个文件，输出到 $Out" -ForegroundColor Cyan
$ok = 0
$fail = 0

foreach ($f in $files) {
    $dest = Join-Path $Out ($f.BaseName + '.ogg')
    # 去掉文件名里的特殊字符，免得 URL 编码出问题
    $dest = $dest -replace '[\\/:*?"<>|]', '_'

    & ffmpeg -y -v error -i $f.FullName `
        -ac 1 -ar 48000 -c:a libopus -b:a "$($Bitrate)k" $dest 2>$null

    if ($LASTEXITCODE -eq 0 -and (Test-Path $dest)) {
        $kb = [math]::Round((Get-Item $dest).Length / 1KB, 0)
        Write-Host ("  ok   {0}  ({1} KB)" -f $f.Name, $kb) -ForegroundColor Green
        $ok++
    }
    else {
        Write-Host ("  FAIL {0}" -f $f.Name) -ForegroundColor Red
        $fail++
    }
}

Write-Host "`n完成：成功 $ok，失败 $fail" -ForegroundColor Cyan
Write-Host "下一步：把 $Out 里的 .ogg 传到服务器" -ForegroundColor Cyan
Write-Host '  scp ' + $Out + '/*.ogg ubuntu@你的服务器:/home/ubuntu/xiaozhi-server/data/music/'
