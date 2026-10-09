# 卸载 AutoScreenRecorder：结束进程、取消自启动、删除安装目录。
# 不删除已录制的视频（默认在 %USERPROFILE%\Videos\AutoScreenRecorder）。
# 用法：双击 uninstall.bat，或 powershell -ExecutionPolicy Bypass -File .\uninstall.ps1

$ErrorActionPreference = "Stop"

Get-Process -Name "AutoScreenRecorder" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if (Test-Path -LiteralPath $runKey) {
    Remove-ItemProperty -Path $runKey -Name "AutoScreenRecorder" -ErrorAction SilentlyContinue
}

$lnk = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\AutoScreenRecorder.lnk"
if (Test-Path -LiteralPath $lnk) {
    Remove-Item -LiteralPath $lnk -Force
}

$protocol = "HKCU:\Software\Classes\autorecorder"
if (Test-Path -LiteralPath $protocol) {
    Remove-Item -LiteralPath $protocol -Recurse -Force
}

$destDir = Join-Path $env:LOCALAPPDATA "AutoScreenRecorder"
if (Test-Path -LiteralPath $destDir) {
    Remove-Item -LiteralPath $destDir -Recurse -Force
}

Write-Host "已卸载，登录后不再自动启动。"
Write-Host "录制文件仍在：$env:USERPROFILE\Videos\AutoScreenRecorder"
