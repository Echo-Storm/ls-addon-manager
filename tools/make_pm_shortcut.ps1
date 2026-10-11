<#
 Puts an "LS Project Manager" icon on the desktop (and optionally the Start menu): a double-click starts the board page
 (tools\dev.py serve) with no console window and opens it in the browser; a second double-click just opens it again.
 The shortcut points at the main checkout, whichever worktree this script is run from.

   powershell -File tools\make_pm_shortcut.ps1 [-StartMenu] [-Port 8761]
#>
param([switch]$StartMenu, [int]$Port = 8761)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$parts = $root.Split('\')
$i = [array]::IndexOf($parts, '.claude')
if ($i -gt 0) { $root = ($parts[0..($i - 1)] -join '\') }      # a worktree: the main checkout
$dev = Join-Path $root 'tools\dev.py'
$ico = Join-Path $root 'tools\ls-project-manager.ico'
if (-not (Test-Path $dev)) { throw "no tools\dev.py in $root (merge the branch into main first)" }
$py = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $py) { throw 'pythonw.exe not found: install Python (python.org) with "Add to PATH"' }
$sh = New-Object -ComObject WScript.Shell
$places = @([Environment]::GetFolderPath('Desktop'))
if ($StartMenu) { $places += (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs') }
foreach ($p in $places) {
  $lnk = $sh.CreateShortcut((Join-Path $p 'LS Project Manager.lnk'))
  $lnk.TargetPath = $py
  $lnk.Arguments = "`"$dev`" serve --port $Port"
  $lnk.WorkingDirectory = $root
  $lnk.Description = 'The project board and checks for LS Addon Manager'
  if (Test-Path $ico) { $lnk.IconLocation = "$ico,0" }
  $lnk.Save()
  Write-Host "made $($lnk.FullName)"
}
