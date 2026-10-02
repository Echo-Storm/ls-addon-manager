# Copies built files into the Lossless Scaling folder, with the safety rules learned the hard way:
#   * refuses while the game (WowB.exe, or whatever -Game names) is running: replacing files under a running session is how
#     sessions get lost;
#   * refuses while Lossless Scaling itself is running (it holds the DLLs open), unless -StopLS is given AND the game is not running;
#   * backs the old file up into <LS folder>\backups\ (never deletes) with a timestamp before replacing it;
#   * host: if Lossless.dll is still Lossless Scaling's own, it is renamed Lossless_original.dll first (the manager needs it there).
#   powershell -File deploy.ps1 -What host|nr|dlaa|fsr|xess|all [-StopLS] [-LsDir '<Lossless Scaling folder>']   (or set the LS_DIR environment variable) [-Game WowB]
param(
    [Parameter(Mandatory = $true)][ValidateSet('host', 'nr', 'dlaa', 'fsr', 'xess', 'all')][string]$What,
    [switch]$StopLS,
    [string]$LsDir = $(if ($env:LS_DIR) { $env:LS_DIR } else { 'C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling' }),
    [string]$Game = 'WowB'
)
$root = Split-Path $PSScriptRoot -Parent   # the repository folder
$items = @{
    host     = @{ Src = "$root\manager\build\Release\Lossless.dll"; Dst = "$LsDir\Lossless.dll"; Extra = @("$root\manager\manager-icon.ico", "$root\manager\manager-icon.png") }
    nr       = @{ Src = "$root\addons\DLSS5NR01\build\Release\DLSS5NR01.dll"; Dst = "$LsDir\addons\DLSS5NR01\DLSS5NR01.dll"; Extra = @("$root\addons\DLSS5NR01\build\Release\nvngx.dll_dlss5nr01.dll", "$root\addons\DLSS5NR01\build\Release\nr_selftest.exe", "$root\addons\DLSS5NR01\addon.json", "$root\addons\DLSS5NR01\icon.svg",
                  @{ Src = "$root\addons\DLSS5NR01\external\ngx\LICENSE.txt"; Rel = 'NVIDIA-LICENSE.txt' }) }
    dlaa     = @{ Src = "$root\addons\DLSS5NR01\build\Release\DLSS4DLAA.dll"; Dst = "$LsDir\addons\DLSS4DLAA\DLSS4DLAA.dll"; Extra = @("$root\addons\DLSS5NR01\products\DLSS4DLAA\addon.json", "$root\addons\DLSS5NR01\products\DLSS4DLAA\icon.svg",
                  @{ Src = "$root\addons\DLSS5NR01\build\Release\dlss\nvngx_dlss.dll"; Rel = 'dlss\nvngx_dlss.dll' }, @{ Src = "$root\addons\DLSS5NR01\external\ngx\LICENSE.txt"; Rel = 'NVIDIA-LICENSE.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\LICENSE"; Rel = 'LICENSE.txt' }) }
    fsr      = @{ Src = "$root\addons\DLSS5NR01\build\Release\FSR3UPSC.dll"; Dst = "$LsDir\addons\FSR3UPSC\FSR3UPSC.dll"; Extra = @("$root\addons\DLSS5NR01\products\FSR3UPSC\addon.json", "$root\addons\DLSS5NR01\products\FSR3UPSC\icon.svg",
                  @{ Src = "$root\addons\DLSS5NR01\build\Release\fsr\amd_fidelityfx_dx12.dll"; Rel = 'fsr\amd_fidelityfx_dx12.dll' },
                  @{ Src = "$root\addons\DLSS5NR01\external\fsr4\amd_fidelityfx_dx12.dll"; Rel = 'runtimes\FSR\0dd77d9c\amd_fidelityfx_dx12.dll' },
                  @{ Src = "$root\addons\DLSS5NR01\products\FSR3UPSC\FSR4-ABOUT.txt"; Rel = 'runtimes\FSR\0dd77d9c\ABOUT.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\third_party\ffx4\LICENSE.txt"; Rel = 'runtimes\FSR\0dd77d9c\LICENSE.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\external\agility\D3D12Core.dll"; Rel = 'runtimes\FSR\0dd77d9c\D3D12Core.dll' },
                  @{ Src = "$root\addons\DLSS5NR01\external\agility\LICENSE.txt"; Rel = 'runtimes\FSR\0dd77d9c\Microsoft-D3D12-LICENSE.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\third_party\ffx\LICENSE.txt"; Rel = 'AMD-FidelityFX-LICENSE.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\LICENSE"; Rel = 'LICENSE.txt' }) }
    xess     = @{ Src = "$root\addons\DLSS5NR01\build\Release\XESSUPSC.dll"; Dst = "$LsDir\addons\XESSUPSC\XESSUPSC.dll"; Extra = @("$root\addons\DLSS5NR01\products\XESSUPSC\addon.json", "$root\addons\DLSS5NR01\products\XESSUPSC\icon.svg",
                  @{ Src = "$root\addons\DLSS5NR01\build\Release\xess\libxess.dll"; Rel = 'xess\libxess.dll' },
                  @{ Src = "$root\addons\DLSS5NR01\external\xess\LICENSE.txt"; Rel = 'Intel-XeSS-LICENSE.txt' },
                  @{ Src = "$root\addons\DLSS5NR01\LICENSE"; Rel = 'LICENSE.txt' }) }
}
if (-not (Test-Path "$LsDir\Lossless.dll")) { Write-Host "No Lossless Scaling folder at $LsDir (pass -LsDir or set LS_DIR)."; exit 4 }
if (Get-Process $Game -ErrorAction SilentlyContinue) { Write-Host "$Game is running: not touching the Lossless Scaling folder. Close the game first."; exit 2 }
$ls = Get-Process LosslessScaling -ErrorAction SilentlyContinue
if ($ls) {
    if (-not $StopLS) { Write-Host 'Lossless Scaling is running (it holds the DLLs). Close it, or pass -StopLS.'; exit 3 }
    Stop-Process -Id $ls.Id; Start-Sleep 2
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$names = if ($What -eq 'all') { @('host', 'nr', 'dlaa', 'fsr', 'xess') } else { @($What) }
foreach ($n in $names) {
    $it = $items[$n]
    if (-not (Test-Path $it.Src)) { Write-Host "[$n] not built: $($it.Src)"; continue }
    if ($n -eq 'host' -and (Test-Path "$LsDir\Lossless.dll")) {
        # Lossless Scaling's own Lossless.dll must become Lossless_original.dll before ours takes its name (the manager passes everything on to
        # it, and Lossless Scaling does not start without it). A Lossless.dll that is Lossless Scaling's own (a fresh install, or after it
        # updated itself over ours) is renamed; the old Lossless_original.dll, if any, goes to the backups.
        if ((Get-Item "$LsDir\Lossless.dll").VersionInfo.ProductName -eq 'Lossless Scaling') {
            New-Item -ItemType Directory -Force "$LsDir\backups" | Out-Null
            if (Test-Path "$LsDir\Lossless_original.dll") { Move-Item "$LsDir\Lossless_original.dll" "$LsDir\backups\Lossless_original-$stamp.dll" }
            Copy-Item "$LsDir\Lossless.dll" "$LsDir\backups\Lossless-original-$stamp.dll"
            Move-Item "$LsDir\Lossless.dll" "$LsDir\Lossless_original.dll"
            Write-Host "[host] Lossless Scaling's own Lossless.dll is now Lossless_original.dll"
        }
        elseif (-not (Test-Path "$LsDir\Lossless_original.dll")) {
            Write-Host "[host] Lossless.dll here is not Lossless Scaling's own and there is no Lossless_original.dll: not deploying (Lossless Scaling would not start). Run Setup and choose Repair."
            continue
        }
    }
    $files = @(@{ Src = $it.Src; Dst = $it.Dst })
    # an extra file goes next to the main one, or (given as @{ Src; Rel }) into a subfolder of it
    foreach ($e in @($it.Extra)) {
        if ($e -is [hashtable]) { if (Test-Path $e.Src) { $files += @{ Src = $e.Src; Dst = Join-Path (Split-Path $it.Dst) $e.Rel } } }
        elseif ($e) { $files += @{ Src = $e; Dst = Join-Path (Split-Path $it.Dst) (Split-Path $e -Leaf) } }
    }
    foreach ($f in $files) {
        New-Item -ItemType Directory -Force (Split-Path $f.Dst) | Out-Null
        if (Test-Path $f.Dst) {
            $bk = "$LsDir\backups"; New-Item -ItemType Directory -Force $bk | Out-Null
            Copy-Item $f.Dst "$bk\$([IO.Path]::GetFileNameWithoutExtension($f.Dst))-$stamp$([IO.Path]::GetExtension($f.Dst))"
        }
        Copy-Item $f.Src $f.Dst -Force
        Write-Host "[$n] $($f.Dst)"
    }
}
# ReShade passthrough and Windowed mode are built into the manager now. If the old standalone addon folders are still in addons\, move them
# aside (never delete): the manager ignores them, but there is no reason to leave them.
if ($names -contains 'host') {
    foreach ($old in 'LSP-ReShade', 'LSP-Windowed') {
        $from = "$LsDir\addons\$old"
        if (Test-Path $from) {
            $aside = "$LsDir\backups\retired-addons-$stamp"
            New-Item -ItemType Directory -Force $aside | Out-Null
            Move-Item $from "$aside\$old"
            Write-Host "[host] moved the retired addon folder $old to $aside"
        }
    }
    # the icons were called LP-icon.ico / .png up to 0.7.4 (now manager-icon): move the old ones aside
    foreach ($old in 'LP-icon.ico', 'LP-icon.png') {
        if (Test-Path "$LsDir\$old") {
            New-Item -ItemType Directory -Force "$LsDir\backups" | Out-Null
            Move-Item "$LsDir\$old" "$LsDir\backups\$([IO.Path]::GetFileNameWithoutExtension($old))-$stamp$([IO.Path]::GetExtension($old))"
            Write-Host "[host] moved the old $old to the backups"
        }
    }
}
# Neural Rendering's helper DLL was called nvngx.dll_lspnr.dll before 0.2.1; the addon no longer loads it. Move a stale copy aside.
if ($names -contains 'nr') {
    $stale = "$LsDir\addons\DLSS5NR01\nvngx.dll_lspnr.dll"
    if (Test-Path $stale) {
        $aside = "$LsDir\backups\retired-addons-$stamp"
        New-Item -ItemType Directory -Force $aside | Out-Null
        Move-Item $stale "$aside\nvngx.dll_lspnr.dll"
        Write-Host "[nr] moved the old helper DLL nvngx.dll_lspnr.dll to $aside"
    }
    # NVIDIA's DLSS runtime was in Neural Rendering's folder for one build (0.8.0 before DLAA became its own addon): it belongs to DLSS4DLAA now
    $staleDlss = "$LsDir\addons\DLSS5NR01\dlss"
    if (Test-Path $staleDlss) {
        $aside = "$LsDir\backups\retired-addons-$stamp"
        New-Item -ItemType Directory -Force $aside | Out-Null
        Move-Item $staleDlss "$aside\DLSS5NR01-dlss"
        Write-Host "[nr] moved the DLSS runtime folder out of Neural Rendering's folder (the DLSS Upscaler has its own) to $aside"
    }
}
if ($ls) { Write-Host 'Lossless Scaling was stopped; start it again yourself.' }
