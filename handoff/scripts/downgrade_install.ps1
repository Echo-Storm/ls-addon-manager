# Puts an older release on a Lossless Scaling folder, to try the in-app update (used 2026-09-26 for 0.9.7 -> 0.9.8, which worked).
#   powershell -File handoff\scripts\downgrade_install.ps1 -OlderSetup <old LSAddonManagerSetup.exe> -CurrentSetup <the installed version's Setup> [-LsDir <folder>]
# An older Setup refuses to go over a newer install, and may refuse to uninstall it, so: uninstall with the Setup of the installed
# version (the addons and settings stay; what is replaced goes to backups), install the older one, then clear the update check's last
# time so the check runs about a minute after Lossless Scaling starts. Refuses while Lossless Scaling runs.
param(
    [Parameter(Mandatory = $true)][string]$OlderSetup,
    [Parameter(Mandatory = $true)][string]$CurrentSetup,
    [string]$LsDir = $(if ($env:LS_DIR) { $env:LS_DIR } else { 'C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling' })
)
$ErrorActionPreference = 'Stop'
if (Get-Process LosslessScaling -ErrorAction SilentlyContinue) { Write-Host 'Lossless Scaling is running: close it (also from its tray icon) first.'; exit 1 }
$log = Join-Path $env:TEMP 'lsam-downgrade.log'
Remove-Item $log -ErrorAction SilentlyContinue
foreach ($step in @(@($CurrentSetup, 'uninstall'), @($OlderSetup, 'install'))) {
    $p = Start-Process -FilePath $step[0] -ArgumentList @('--silent', $step[1], '--folder', "`"$LsDir`"", '--log', "`"$log`"") -Wait -PassThru
    Write-Host ("{0}: exit {1}" -f $step[1], $p.ExitCode)
    if ($p.ExitCode -ne 0) { Get-Content $log -Tail 10; exit 1 }
}
Get-Content $log -Tail 4
Write-Host ("Lossless.dll is now {0}" -f (Get-Item (Join-Path $LsDir 'Lossless.dll')).VersionInfo.ProductVersion)
& python (Join-Path $PSScriptRoot 'reset_update_check.py') (Join-Path $LsDir 'addons\config.json')
