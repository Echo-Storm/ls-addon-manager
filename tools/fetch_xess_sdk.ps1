# Fetches Intel's XeSS SDK the XeSS Upscaler is built against and runs on: its headers (inc\xess), its runtime libxess.dll and its licence,
# from Intel's own public repository, https://github.com/intel/xess (release v3.0.2, XeSS_SDK_3.0.2.zip), into addons\DLSS5NR01\external\xess.
# The build copies libxess.dll into the XeSS addon's xess folder, and the release ships it there with Intel's licence.
#
# It is Intel's, under the Intel Simplified Software License (external\xess\LICENSE.txt): the runtime may be redistributed unmodified with
# that licence; the headers are only fetched, never committed (external\ is ignored by git).
#
# The zip is pinned to Intel's release and checked against a SHA-256 value, libxess.dll against its own, and its Authenticode signature must
# be valid and Intel's, so what the addon loads is exactly what Intel published. Files already in place that match are left alone.
#   powershell -File tools\fetch_xess_sdk.ps1 [-Dest <folder>]
param([string]$Dest = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = "$root\addons\DLSS5NR01\external\xess" }

$zipUrl = 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
$zipSha = '88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990'
$dllSha = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'   # libxess.dll 2.0.2.68
$fgSha = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'   # libxess_fg.dll (frame generation, for tools\nr_fgeval), 1.3.1.78
$llSha = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'   # libxell.dll (Xe Low Latency, which XeSS frame generation needs)

function Sha($path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return (($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $stream.Dispose() }
}
function SignedByIntel($path) {
    $s = Get-AuthenticodeSignature -LiteralPath $path
    return $s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -like 'CN=Intel Corporation*'
}

$dll = "$Dest\bin\libxess.dll"
if ((Test-Path $dll) -and (Test-Path "$Dest\inc\xess\xess_d3d12.h") -and (Test-Path "$Dest\LICENSE.txt") -and (Sha $dll) -eq $dllSha -and (Test-Path "$Dest\bin\libxess_fg.dll") -and (Sha "$Dest\bin\libxess_fg.dll") -eq $fgSha -and (Test-Path "$Dest\bin\libxell.dll") -and (Sha "$Dest\bin\libxell.dll") -eq $llSha) {
    Write-Host "  in place: Intel's XeSS SDK in $Dest"; exit 0
}
$tmp = Join-Path $env:TEMP "xess_sdk_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    Write-Host "  $zipUrl (about 80 MB)"
    Invoke-WebRequest -Uri $zipUrl -OutFile "$tmp\sdk.zip" -UseBasicParsing
    $got = Sha "$tmp\sdk.zip"
    if ($got -ne $zipSha) { Write-Host "  CHECKSUM MISMATCH for the SDK zip: got $got"; exit 1 }
    Expand-Archive -LiteralPath "$tmp\sdk.zip" -DestinationPath "$tmp\sdk" -Force
    $got = Sha "$tmp\sdk\bin\libxess.dll"
    if ($got -ne $dllSha) { Write-Host "  CHECKSUM MISMATCH for libxess.dll: got $got"; exit 1 }
    if (-not (SignedByIntel "$tmp\sdk\bin\libxess.dll")) { Write-Host '  NOT SIGNED BY INTEL: libxess.dll'; exit 1 }
    $got = Sha "$tmp\sdk\bin\libxess_fg.dll"
    if ($got -ne $fgSha) { Write-Host "  CHECKSUM MISMATCH for libxess_fg.dll: got $got"; exit 1 }
    if (-not (SignedByIntel "$tmp\sdk\bin\libxess_fg.dll")) { Write-Host '  NOT SIGNED BY INTEL: libxess_fg.dll'; exit 1 }
    $got = Sha "$tmp\sdk\bin\libxell.dll"
    if ($got -ne $llSha) { Write-Host "  CHECKSUM MISMATCH for libxell.dll: got $got"; exit 1 }
    if (-not (SignedByIntel "$tmp\sdk\bin\libxell.dll")) { Write-Host '  NOT SIGNED BY INTEL: libxell.dll'; exit 1 }
    New-Item -ItemType Directory -Force "$Dest\bin", "$Dest\inc" | Out-Null
    Copy-Item -Recurse -Force "$tmp\sdk\inc\xess", "$tmp\sdk\inc\xess_fg", "$tmp\sdk\inc\xell" "$Dest\inc\"
    Copy-Item -Force "$tmp\sdk\bin\libxess.dll", "$tmp\sdk\bin\libxess_fg.dll", "$tmp\sdk\bin\libxell.dll" "$Dest\bin\"
    Copy-Item -Force "$tmp\sdk\LICENSE.txt" "$Dest\"
    Write-Host "Intel's XeSS SDK is in $Dest."
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
