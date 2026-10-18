$dir = $PSScriptRoot
$putty = 'C:\Users\Wrok\AppData\Local\Programs\PuTTY\putty.exe'
$fp = (Get-Content (Join-Path $dir 'fingerprint.txt') -Raw).Trim()
Write-Output "FINGERPRINT=$fp"

$cmdFile = Join-Path $dir 'cmd.txt'
Set-Content -Path $cmdFile -Value 'echo hello' -Encoding ASCII

function Test-LogCjk($path) {
  if (-not (Test-Path $path)) { return $false }
  $bytes = [System.IO.File]::ReadAllBytes($path)
  foreach ($b in $bytes) { if ($b -gt 127) { return $true } }
  return $false
}

# T1: password auth + command execution + CJK output (use -m, since putty treats a trailing arg as a port)
$log1 = Join-Path $dir 'log-t1.txt'
Remove-Item $log1 -ErrorAction SilentlyContinue
$args1 = "-ssh -P 2222 -l testuser -pw testpass -hostkey $fp -m `"$cmdFile`" -sessionlog `"$log1`" 127.0.0.1"
$p1 = Start-Process -FilePath $putty -ArgumentList $args1 -PassThru
Start-Sleep -Seconds 6
$t1 = if (Test-Path $log1) { Get-Content $log1 -Raw } else { '' }
$hello1 = $t1.Contains('HELLO_FROM_PUTTY')
$cjk1 = Test-LogCjk $log1
$cmd1 = $t1.Contains('COMMAND=echo hello')
Stop-Process -Id $p1.Id -Force -ErrorAction SilentlyContinue
Write-Output "T1 hello=$hello1 cjk=$cjk1 cmd=$cmd1"
if (-not ($hello1 -and $cjk1)) {
  Write-Output '--- T1 log head ---'
  if (Test-Path $log1) { Get-Content $log1 -TotalCount 40 }
}

# T2: public key auth + command execution
$log2 = Join-Path $dir 'log-t2.txt'
Remove-Item $log2 -ErrorAction SilentlyContinue
$key = Join-Path $dir 'id_ed25519.ppk'
$args2 = "-ssh -P 2222 -l testuser -i `"$key`" -hostkey $fp -m `"$cmdFile`" -sessionlog `"$log2`" 127.0.0.1"
$p2 = Start-Process -FilePath $putty -ArgumentList $args2 -PassThru
Start-Sleep -Seconds 6
$t2 = if (Test-Path $log2) { Get-Content $log2 -Raw } else { '' }
$hello2 = $t2.Contains('HELLO_FROM_PUTTY')
Stop-Process -Id $p2.Id -Force -ErrorAction SilentlyContinue
Write-Output "T2 hello=$hello2"
if (-not $hello2) {
  Write-Output '--- T2 log head ---'
  if (Test-Path $log2) { Get-Content $log2 -TotalCount 40 }
}

# T3: local port forwarding -L (18080 -> test http on 18081)
$log3 = Join-Path $dir 'log-t3.txt'
Remove-Item $log3 -ErrorAction SilentlyContinue
$args3 = "-ssh -P 2222 -l testuser -pw testpass -hostkey $fp -L 18080:127.0.0.1:18081 -sessionlog `"$log3`" 127.0.0.1"
$p3 = Start-Process -FilePath $putty -ArgumentList $args3 -PassThru
Start-Sleep -Seconds 6
$body = ''
try {
  $r = Invoke-WebRequest -Uri 'http://127.0.0.1:18080/' -UseBasicParsing -TimeoutSec 5
  $body = $r.Content
} catch {
  $body = 'ERR:' + $_.Exception.Message
}
Stop-Process -Id $p3.Id -Force -ErrorAction SilentlyContinue
Write-Output "T3 body=$body"

# T4: interactive shell window title
$p4 = Start-Process -FilePath $putty -ArgumentList "-ssh -P 2222 -l testuser -pw testpass -hostkey $fp 127.0.0.1" -PassThru
Start-Sleep -Seconds 5
$p4.Refresh()
$title4 = $p4.MainWindowTitle
Stop-Process -Id $p4.Id -Force -ErrorAction SilentlyContinue
Write-Output "T4 title=[$title4]"

Write-Output '=== SUMMARY ==='
Write-Output ("T1 password+exec+cjk: " + $(if ($hello1 -and $cjk1 -and $cmd1) { 'PASS' } else { 'FAIL' }))
Write-Output ("T2 pubkey+exec: " + $(if ($hello2) { 'PASS' } else { 'FAIL' }))
Write-Output ("T3 port forward: " + $(if ($body -eq 'HTTP_ECHO_OK') { 'PASS' } else { 'FAIL' }))
Write-Output ("T4 shell title: " + $(if ($title4 -match 'PuTTY') { 'PASS' } else { 'FAIL' }))
