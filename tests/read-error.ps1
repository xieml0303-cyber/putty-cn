$putty = 'C:\Users\Wrok\AppData\Local\Programs\PuTTY\putty.exe'
$log = 'C:\Users\Wrok\.zcode\workspace\default\.workbuddy\putty-test\log-t1.txt'
$args = '-ssh -4 -P 2222 -l testuser -pw testpass -hostkey SHA256:R9vbIvocTm5PY2Td87NV0rPsQPKKt9XTklgHal9b/e0 -sessionlog "' + $log + '" 127.0.0.1 "echo hello"'
Write-Output ("ARGS=" + $args)
$p = Start-Process -FilePath $putty -ArgumentList $args -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
Write-Output ("TITLE=" + $p.MainWindowTitle)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$win = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
$all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
foreach ($c in $all) {
  $n = $c.Current.Name
  if ($n) { Write-Output ("MSG: " + $n) }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
