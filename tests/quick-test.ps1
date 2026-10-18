$putty = 'C:\Users\Wrok\AppData\Local\Programs\PuTTY\putty.exe'
$fp = 'SHA256:R9vbIvocTm5PY2Td87NV0rPsQPKKt9XTklgHal9b/e0'
$dir = $PSScriptRoot
$cmdFile = Join-Path $dir 'cmd.txt'
Set-Content -Path $cmdFile -Value 'echo hello' -Encoding ASCII
$log = Join-Path $dir 'log-q2.txt'
Remove-Item $log -ErrorAction SilentlyContinue
$args = "-ssh -P 2222 -l testuser -pw testpass -hostkey $fp -m `"$cmdFile`" -sessionlog `"$log`" 127.0.0.1"
$p = Start-Process -FilePath $putty -ArgumentList $args -PassThru
Start-Sleep -Seconds 6
$p.Refresh()
Write-Output ("TITLE=[" + $p.MainWindowTitle + "]")
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
if (Test-Path $log) {
  Write-Output '--- log ---'
  Get-Content $log
} else {
  Write-Output 'no log file'
}
