# After Explorer or the taskbar misbehaves (for example after closing a scaled game by its X): what Windows recorded in the last 14 days, read-only.
# Explorer or the shell crashing (Application Error / Hang, with the faulting module), display-driver resets, display re-enumerations, Lossless Scaling crashes, and when explorer started.
#   powershell -File tools\shell_events.ps1
$since = (Get-Date).AddDays(-14)
Write-Output "=== explorer.exe application errors / hangs (14 days)"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error', 'Application Hang'; StartTime = $since } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'explorer\.exe|dwm\.exe|ShellExperienceHost|StartMenuExperienceHost' } |
  Select-Object -First 12 | ForEach-Object {
    $m = $_.Message -split "`r?`n"
    "{0:yyyy-MM-dd HH:mm:ss} id {1}: {2} | {3} | {4}" -f $_.TimeCreated, $_.Id, ($m | Select-String 'Faulting application name' | Select-Object -First 1), ($m | Select-String 'Faulting module name' | Select-Object -First 1), ($m | Select-String 'Exception code' | Select-Object -First 1)
  }
Write-Output "=== display driver resets (nvlddmkm / Display 4101, 14 days)"
Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display', 'nvlddmkm'; StartTime = $since } -ErrorAction SilentlyContinue |
  Select-Object -First 10 | ForEach-Object { "{0:yyyy-MM-dd HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, ($_.Message -split "`r?`n")[0] }
Write-Output "=== Lossless Scaling crashes (14 days)"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error'; StartTime = $since } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'LosslessScaling' } | Select-Object -First 8 | ForEach-Object {
    $m = $_.Message -split "`r?`n"
    "{0:yyyy-MM-dd HH:mm:ss}: {1} | {2} | {3}" -f $_.TimeCreated, ($m | Select-String 'Faulting module name' | Select-Object -First 1), ($m | Select-String 'Exception code' | Select-Object -First 1), ($m | Select-String 'Fault offset' | Select-Object -First 1)
  }
Write-Output "=== explorer.exe start times (restarts), 14 days"
Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 4688; StartTime = $since } -ErrorAction SilentlyContinue -MaxEvents 5 | Out-Null
Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { "explorer pid {0} started {1}" -f $_.Id, $_.StartTime }
