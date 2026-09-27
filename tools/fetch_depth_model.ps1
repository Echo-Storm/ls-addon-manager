# Fetches Depth Anything V2 Small, a model that estimates depth from a single picture, as ONNX: onnx-community/depth-anything-v2-small on
# Hugging Face (Apache 2.0), pinned to one revision and checked against a SHA-256 value, into addons\DLSS5NR01\external\depth (ignored by git).
# Used by the frame generation experiments (tools\depth_maps.py and nr_fgeval's depthdir=): depth for the frames, from the frames alone.
#   powershell -File tools\fetch_depth_model.ps1 [-Dest <folder>]
param([string]$Dest = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = "$root\addons\DLSS5NR01\external\depth" }

$revision = '4472b7362082ad9968fee890ca0f1e5aca36b93d'
$url = "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/$revision/onnx/model.onnx"
$sha = 'afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c'
$target = "$Dest\depth_anything_v2_small.onnx"

function Sha($path) {
    $h = [Security.Cryptography.SHA256]::Create(); $s = [IO.File]::OpenRead($path)
    try { return (($h.ComputeHash($s) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $s.Dispose() }
}
if ((Test-Path $target) -and (Sha $target) -eq $sha) { Write-Host "  in place: $target"; exit 0 }
New-Item -ItemType Directory -Force $Dest | Out-Null
Write-Host "  $url (about 100 MB)"
Invoke-WebRequest -Uri $url -OutFile "$target.download" -UseBasicParsing
$got = Sha "$target.download"
if ($got -ne $sha) { Remove-Item "$target.download"; Write-Host "  CHECKSUM MISMATCH: got $got"; exit 1 }
Move-Item -Force "$target.download" $target
Write-Host "Depth Anything V2 Small is in $Dest."
