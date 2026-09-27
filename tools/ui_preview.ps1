# Renders the manager's window offscreen (no window, no screen capture) at a realistic size: every tab, and the Addons tab with the four
# plugins and each one's own panel (loaded from its DLL against a fake host), then converts the BMPs to PNGs. The README's screenshots
# come from here: set EAM_PREVIEW_CLEAN=1 for the tidy scene (no toast, no error states), then copy the PNGs into docs\images.
#   powershell -File ui_preview.ps1 [-Out C:\path\to\folder] [-Scale 1.25]
param([string]$Out = "$env:TEMP\ui_preview", [double]$Scale = 1.25)
New-Item -ItemType Directory -Force $Out | Out-Null
$root = Split-Path $PSScriptRoot -Parent   # the repository folder
$exe = "$root\manager\build\Release\eam_uipreview.exe"
$dlls = @('DLSS5NR01', 'DLSS4DLAA', 'FSR3UPSC', 'XESSUPSC') | ForEach-Object { Join-Path $root "addons\DLSS5NR01\build\Release\$_.dll" }   # their panels, in the Addons shots
& $exe ($Out -replace '\\', '/') $Scale @($dlls | Where-Object { Test-Path $_ }) | Select-Object -Last 8
python "$PSScriptRoot\bmp2png.py" $Out
