# Collects what is needed to see how a test session went into one zip on the Desktop: the logs, the settings, the addons' versions, the graphics cards.
#   powershell -ExecutionPolicy Bypass -File tools\collect_test_logs.ps1 [-LsDir '<Lossless Scaling folder>'] [-Out '<folder for the zip>']
# Nothing is changed; the zip is for sending. It holds paths with your user name in the logs; look inside first if that matters.
param(
    [string]$LsDir = $(if ($env:LS_DIR) { $env:LS_DIR } elseif (Test-Path 'D:\Utilities\Lossless Scaling') { 'D:\Utilities\Lossless Scaling' } else { 'C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling' }),
    [string]$Out = [Environment]::GetFolderPath('Desktop')
)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path $LsDir)) { Write-Host "Lossless Scaling folder not found: $LsDir (give it with -LsDir)"; exit 1 }
$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm'
$work = Join-Path ([IO.Path]::GetTempPath()) "ls-test-bundle-$stamp"
New-Item -ItemType Directory -Force $work | Out-Null

# logs (all of them: the addons' and the manager's, and the .old ones)
$logs = Join-Path $LsDir 'logs'
if (Test-Path $logs) { Copy-Item $logs (Join-Path $work 'logs') -Recurse -Force }

# settings: the manager's and Lossless Scaling's own
foreach ($f in 'addons\config.json', 'config.ini') { $p = Join-Path $LsDir $f; if (Test-Path $p) { Copy-Item $p (Join-Path $work (Split-Path $f -Leaf)) -Force } }

# what is installed: each addon's version and DLL date, the manager's DLL
$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine("Collected $(Get-Date -Format 's') from $LsDir")
$mgr = Join-Path $LsDir 'Lossless.dll'
if (Test-Path $mgr) { $i = Get-Item $mgr; [void]$sb.AppendLine("Lossless.dll (manager)  $($i.VersionInfo.FileVersion)  $($i.Length) bytes  $($i.LastWriteTime)") }
Get-ChildItem (Join-Path $LsDir 'addons') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $dir = $_.FullName; $ver = ''
    $j = Join-Path $dir 'addon.json'
    if (Test-Path $j) { try { $ver = (Get-Content $j -Raw | ConvertFrom-Json).version } catch { } }
    $dll = Get-ChildItem $dir -Filter '*.dll' -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '^(nvngx|amd_|libxess)' } | Select-Object -First 1
    [void]$sb.AppendLine(("{0,-12} version {1,-8} {2}  {3}" -f $_.Name, $ver, $(if ($dll) { $dll.Name } else { '(no dll)' }), $(if ($dll) { $dll.LastWriteTime } else { '' })))
}
# the graphics cards, Windows
[void]$sb.AppendLine('')
Get-CimInstance Win32_VideoController | ForEach-Object { [void]$sb.AppendLine("GPU: $($_.Name)  driver $($_.DriverVersion)  $([math]::Round($_.AdapterRAM / 1MB)) MB (as Windows reports it)") }
[void]$sb.AppendLine("Windows: $([Environment]::OSVersion.VersionString)")
# is Lossless Scaling running right now
[void]$sb.AppendLine("Lossless Scaling running: $([bool](Get-Process -Name 'LosslessScaling' -ErrorAction SilentlyContinue))")
$sb.ToString() | Set-Content (Join-Path $work 'installed.txt') -Encoding UTF8

# the lines that matter most, pulled out so they are quick to read
$key = Join-Path $work 'key-lines.txt'
Get-ChildItem (Join-Path $work 'logs') -Filter '*.log' -ErrorAction SilentlyContinue | ForEach-Object {
    $n = $_.Name
    Select-String -Path $_.FullName -Pattern 'Scaling status|FpsCounter|ApplySettings|lighter|upscaler alone|video memory|NIS passes seen|could not run|error|failed|Resource .* asked for' -ErrorAction SilentlyContinue |
        Select-Object -Last 60 | ForEach-Object { "$n : $($_.Line)" }
} | Set-Content $key -Encoding UTF8

$zip = Join-Path $Out "LS-test-bundle-$stamp.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item $work -Recurse -Force
Write-Host "Done: $zip"
Write-Host 'Send that file.'
