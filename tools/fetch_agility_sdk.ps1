# Fetches Microsoft's Direct3D 12 Agility SDK core (D3D12Core.dll), which the FSR Upscaler can create its Direct3D 12 device through when the FSR 4 INT8 runtime is
# the one chosen (issue #11: on an RX 6600 XT that runtime takes Lossless Scaling down with the Windows Direct3D 12, and the reporter has it working in OptiScaler with its
# FsrAgilitySDKUpgrade, which does the same). It is Microsoft's own redistributable (the NuGet package Microsoft.Direct3D.D3D12; its licence allows shipping D3D12Core.dll with an
# application), signed by Microsoft: the signature and the SHA-256 below are checked. It goes into external\agility (kept out of the repository, like the other SDK drops);
# packaging puts D3D12Core.dll beside the FSR addon's runtimes. The shipped FSR 3.1.4 runtime does not use it. Nothing is committed from here.
#   powershell -File tools\fetch_agility_sdk.ps1 [-Dest <folder>]
param([string]$Dest = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = "$root\addons\DLSS5NR01\external\agility" }

$version = '1.619.6'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$version/microsoft.direct3d.d3d12.$version.nupkg"
$inside = 'build/native/bin/x64/D3D12Core.dll'
$sha = '37fa14281a58cc834076971873006feb8a8d25cddc908d1a345bda1b149ffc7d'   # D3D12Core.dll x64, version 1.619.6.0.20260914.1
$target = "$Dest\D3D12Core.dll"

function Sha($path) {
    $h = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return (($h.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $stream.Dispose() }
}

if ($sha -and (Test-Path $target) -and (Sha $target) -eq $sha) { Write-Host "  in place: $target"; exit 0 }
$work = Join-Path ([IO.Path]::GetTempPath()) "fetch_agility_$PID"
New-Item -ItemType Directory -Force $work | Out-Null
try {
    Write-Host "  $url"
    Invoke-WebRequest -Uri $url -OutFile "$work\agility.nupkg" -UseBasicParsing
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead("$work\agility.nupkg")
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $inside } | Select-Object -First 1
        if (-not $entry) { Write-Host "  the package does not hold $inside"; exit 1 }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$work\D3D12Core.dll", $true)
    } finally { $zip.Dispose() }
    
    $zipL = [IO.Compression.ZipFile]::OpenRead("$work\agility.nupkg")
    try { $le = $zipL.Entries | Where-Object { $_.FullName -eq 'LICENSE.txt' } | Select-Object -First 1; if ($le) { [IO.Compression.ZipFileExtensions]::ExtractToFile($le, "$work\LICENSE.txt", $true) } } finally { $zipL.Dispose() }
    $sig = Get-AuthenticodeSignature "$work\D3D12Core.dll"
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft') { Write-Host "  D3D12Core.dll is not validly signed by Microsoft ($($sig.Status)); nothing was kept"; exit 1 }
    $got = Sha "$work\D3D12Core.dll"
    if ($sha -and $got -ne $sha) { Write-Host "  CHECKSUM MISMATCH: got $got, want $sha; nothing was kept"; exit 1 }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    Copy-Item "$work\D3D12Core.dll" $target -Force
    if (Test-Path "$work\LICENSE.txt") { Copy-Item "$work\LICENSE.txt" "$Dest\LICENSE.txt" -Force }
    Write-Host "  kept $target  (signed by Microsoft, version $((Get-Item $target).VersionInfo.FileVersion), sha256 $got)"
    if (-not $sha) { Write-Host '  (no checksum pinned yet: put the sha256 above into this script)' }
} finally { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
