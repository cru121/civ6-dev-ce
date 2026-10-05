# Watches CivilizationVI.exe; when it stays "not responding" for 20 s it takes a thread/stack dump with hang_dump.py (Frida) and kills the process,
# so a hung Dev CE test does not need a reboot. Run it in a separate window before starting the game; Ctrl+C to stop.
$out = Join-Path $PSScriptRoot '..\hang_dump_watchdog.txt'
$bad = 0
while ($true) {
  Start-Sleep -Seconds 2
  $p = Get-Process CivilizationVI -ErrorAction SilentlyContinue
  if (-not $p) { $bad = 0; continue }
  if ($p.Responding) { $bad = 0; continue }
  $bad += 2
  if ($bad -ge 20) {
    "hang detected $(Get-Date)" | Out-File $out -Encoding utf8
    & python (Join-Path $PSScriptRoot 'hang_dump.py') $p.Id 2>&1 | Out-File $out -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force
    "killed $(Get-Date)" | Out-File $out -Append -Encoding utf8
    $bad = 0
  }
}
