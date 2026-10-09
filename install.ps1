# 安装 AutoScreenRecorder 并设置登录自启动。
# 安装位置：%LOCALAPPDATA%\AutoScreenRecorder
# 用法：双击 install.bat，或在此目录执行 powershell -ExecutionPolicy Bypass -File .\install.ps1

$ErrorActionPreference = "Stop"

$srcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exeName = "AutoScreenRecorder.exe"
$srcExe = Join-Path $srcDir $exeName
if (-not (Test-Path -LiteralPath $srcExe)) {
    Write-Host "找不到 $srcExe"
    Write-Host "请先在项目目录编译：go build -trimpath -ldflags ""-s -w -H windowsgui"" -o AutoScreenRecorder.exe ./cmd/autorecorder"
    exit 1
}

$destDir = Join-Path $env:LOCALAPPDATA "AutoScreenRecorder"
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$destExe = Join-Path $destDir $exeName

Get-Process -Name "AutoScreenRecorder" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Copy-Item -LiteralPath $srcExe -Destination $destExe -Force

$destConfig = Join-Path $destDir "config.yaml"
if (-not (Test-Path -LiteralPath $destConfig)) {
    $srcConfig = Join-Path $srcDir "config.yaml"
    $example = Join-Path $srcDir "config.example.balanced.yaml"
    if (Test-Path -LiteralPath $srcConfig) {
        Copy-Item -LiteralPath $srcConfig -Destination $destConfig
    } elseif (Test-Path -LiteralPath $example) {
        Copy-Item -LiteralPath $example -Destination $destConfig
    }
}

$vbsPath = Join-Path $destDir "start-hidden.vbs"
$vbs = @"
Set sh = CreateObject("Wscript.Shell")
sh.Run """$destExe""", 0, False
"@
Set-Content -LiteralPath $vbsPath -Value $vbs -Encoding Unicode

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
# 这个键里还有其他程序的自启动。只新增本程序这一项。
# 不能用 New-Item -Force：键已存在时 -Force 会删掉里面全部启动项。
if (-not (Test-Path -LiteralPath $runKey)) {
    New-Item -Path $runKey | Out-Null
}
$runValue = "wscript.exe //B `"$vbsPath`""
Set-ItemProperty -Path $runKey -Name "AutoScreenRecorder" -Value $runValue

Start-Process -FilePath "wscript.exe" -ArgumentList @("//B", $vbsPath) | Out-Null

Write-Host "已安装并启动：$destExe"
Write-Host "登录后会自动启动。配置文件：$destConfig"
Write-Host "日志写在同一目录的 AutoScreenRecorder.log"
