# Fetches the NVIDIA DLSS SDK files the Neural Rendering addon needs from NVIDIA's own public repository, https://github.com/NVIDIA/DLSS, into
# addons\DLSS5NR01\external\ngx: the headers and static library to build it, NVIDIA's DLSS runtime (nvngx_dlss.dll, for the DLAA model), and
# NVIDIA's licence, which goes into the release next to them. Nothing is taken from anywhere else, and nothing is committed: that folder is
# ignored by git. NVIDIA's licence allows the runtime and the library's object code to ship inside an application such as this one, under
# its own terms (not this project's MIT licence); see NOTICE.md.
#
# The files are pinned to one commit of NVIDIA's repository and checked against SHA-256 values, so what you build against is what this
# addon was built against. Files already in place that match are left alone.
#
# NVIDIA's RTX SDKs licence applies to these files, and using them means accepting it:
#   https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
# so the download only runs when you say you accept it:
#   powershell -File tools\fetch_ngx_sdk.ps1 -AcceptNvidiaLicense [-Dest <folder>]
#
# -Latest takes NVIDIA's newest commit instead of the pinned one (-Commit <sha> takes a commit you name). Those files are checked against
# NVIDIA's own git object ids rather than against the pinned SHA-256 values, and the script prints the lines to paste in here to pin them.
# The pinned commit is "DLSS 310.9.1 SDK" (the headers and the static library); the runtime, nvngx_dlss.dll, is the one of "DLSS 310.9.16 SDK" (a file can name its own
# Commit): the same header files, a newer runtime.
param(
    [switch]$AcceptNvidiaLicense,
    [string]$Dest = '',
    [switch]$Latest,
    [string]$Commit = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = "$root\addons\DLSS5NR01\external\ngx" }

$pinnedCommit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'   # NVIDIA/DLSS main when these files were taken
$files = @(
    @{ Remote = 'include/nvsdk_ngx.h';                   Local = 'include\nvsdk_ngx.h';                   Sha = 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848' },
    @{ Remote = 'include/nvsdk_ngx_defs.h';              Local = 'include\nvsdk_ngx_defs.h';              Sha = 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e' },
    @{ Remote = 'include/nvsdk_ngx_defs_dlssd.h';        Local = 'include\nvsdk_ngx_defs_dlssd.h';        Sha = '2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716' },
    @{ Remote = 'include/nvsdk_ngx_helpers.h';           Local = 'include\nvsdk_ngx_helpers.h';           Sha = '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471' },
    @{ Remote = 'include/nvsdk_ngx_helpers_dlssd.h';     Local = 'include\nvsdk_ngx_helpers_dlssd.h';     Sha = 'a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532' },
    @{ Remote = 'include/nvsdk_ngx_params.h';            Local = 'include\nvsdk_ngx_params.h';            Sha = '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc' },
    @{ Remote = 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'; Local = 'lib\nvsdk_ngx_s.lib';                  Sha = '4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3' },   # the /MT static library, x64
    @{ Remote = 'lib/Windows_x86_64/rel/nvngx_dlss.dll'; Local = 'bin\nvngx_dlss.dll';                    Sha = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'; Commit = '9981c6350dc58c0ce21f024fd65e31e851c31689' },   # DLSS Super Res, 310.9.16 (NVIDIA's git blob 23c4672e)olution / DLAA 310.9.1.0, 59 MB
    @{ Remote = 'LICENSE.txt';                           Local = 'LICENSE.txt';                            Sha = 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0' }    # NVIDIA RTX SDKs licence
)

# SHA-256 of a file as NVIDIA publishes it. Headers are compared with line endings ignored: NVIDIA's are LF, and git on Windows may have turned
# a copy into CRLF, which is still the same file.
function Sha($path) {
    if ($path -like '*.h') {
        $bytes = [IO.File]::ReadAllBytes($path)
        $text = [Text.Encoding]::UTF8.GetString($bytes) -replace "`r`n", "`n"
        $sha = [Security.Cryptography.SHA256]::Create()
        return (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

# git's own id for a file (what NVIDIA's repository lists), from its bytes
function BlobSha1($path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $head = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    return (($sha1.ComputeHash([byte[]]($head + $bytes)) | ForEach-Object { $_.ToString('x2') }) -join '')
}
$ua = @{ 'User-Agent' = 'ls-addon-manager' }

if ($Latest) {
    $head = Invoke-RestMethod -Uri 'https://api.github.com/repos/NVIDIA/DLSS/commits/main' -Headers $ua
    $Commit = $head.sha
    Write-Host "NVIDIA/DLSS main is $($Commit.Substring(0, 7)): $(($head.commit.message -split "`n")[0])"
}
if ($Commit -and $Commit -ne $pinnedCommit) {
    if (-not $AcceptNvidiaLicense) {
        Write-Host "This fetches NVIDIA's DLSS SDK files at commit $Commit from https://github.com/NVIDIA/DLSS under NVIDIA's RTX SDKs licence:"
        Write-Host '  https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'
        Write-Host 'Read it, and if you accept it run this again with -AcceptNvidiaLicense.'
        exit 3
    }
    $pins = @()
    foreach ($f in $files) {
        $target = "$Dest\$($f.Local)"
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        $meta = Invoke-RestMethod -Uri "https://api.github.com/repos/NVIDIA/DLSS/contents/$($f.Remote)?ref=$Commit" -Headers $ua
        $tmp = "$target.download"
        Write-Host "  $($f.Remote)"
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/$Commit/$($f.Remote)" -OutFile $tmp -UseBasicParsing
        if ((BlobSha1 $tmp) -ne $meta.sha) { Remove-Item -LiteralPath $tmp; throw "$($f.Remote) does not match NVIDIA's git object id; not using it." }
        Move-Item -LiteralPath $tmp -Destination $target -Force
        $pins += "    @{ Remote = '$($f.Remote)'; Local = '$($f.Local)'; Sha = '$(Sha $target)' },"
    }
    Write-Host "Done: NVIDIA/DLSS $Commit placed in $Dest (checked against NVIDIA's git object ids). Build and run the addon's tests with it. To pin it, set"
    Write-Host "  `$pinnedCommit = '$Commit'"
    Write-Host 'and these Sha values in $files:'
    $pins | ForEach-Object { Write-Host $_ }
    exit 0
}

$missing = @($files | Where-Object { -not ((Test-Path "$Dest\$($_.Local)") -and (Sha "$Dest\$($_.Local)") -eq $_.Sha) })
if ($missing.Count -eq 0) { Write-Host "The NVIDIA SDK files are already in $Dest and match. Nothing to do."; exit 0 }

if (-not $AcceptNvidiaLicense) {
    Write-Host "$($missing.Count) NVIDIA SDK file(s) are missing or different in $Dest."
    Write-Host 'They come from NVIDIA''s public repository, https://github.com/NVIDIA/DLSS, under NVIDIA''s RTX SDKs licence:'
    Write-Host '  https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'
    Write-Host 'Read it, and if you accept it run this again with -AcceptNvidiaLicense.'
    exit 3
}

foreach ($f in $missing) {
    $target = "$Dest\$($f.Local)"
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    $at = if ($f.Commit) { $f.Commit } else { $pinnedCommit }
    $url = "https://raw.githubusercontent.com/NVIDIA/DLSS/$at/$($f.Remote)"
    $tmp = "$target.download"
    Write-Host "  $($f.Remote)"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    if ((Sha $tmp) -ne $f.Sha) { Remove-Item -LiteralPath $tmp; throw "$($f.Remote) does not match the expected SHA-256; not using it." }
    Move-Item -LiteralPath $tmp -Destination $target -Force
}
Write-Host "Done: $($missing.Count) file(s) placed in $Dest. They are ignored by git: do not commit them. The release carries the runtime and the licence under NVIDIA's terms (see NOTICE.md)."
