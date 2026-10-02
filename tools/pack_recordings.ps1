# Compresses the recordings (.lsrec) after the fact, lossless, and checks the result before anything is removed.
#   -Method zstd   (default) each file becomes <name>.lsrec.zst: Zstandard with long-range matching, checked by its own content checksum. A 3 GB half-float HDR recording
#                  (the recorder's own codec, QOI, leaves such a frame as big as it was raw) comes out at a third in 37 s at level 12; level 19 is a little smaller and much slower.
#   -Method video  each file becomes <name>.lsrec.mkv (+ .meta): FFV1 lossless video through ffmpeg (nr_lsrec video), which plays in any player for 8-bit frames, and is
#                  decoded and compared with the recording frame for frame, bit for bit, before it counts. About the same size as zstd for HDR game frames, 7 s for the same 3 GB file,
#                  and 16 % of the size for 8-bit pictures. `nr_lsrec unvideo` makes the .lsrec again.
#   powershell -File tools\pack_recordings.ps1 [-Folder <folder>] [-Method zstd|video] [-Level 12] [-RemoveOriginals] [-Expand] [-Newest N] [-Tool <nr_lsrec.exe>]
# -Folder           where the recordings are (default: Videos\Lossless Scaling)
# -Level            zstd 1..19 (higher: smaller and slower); default 12
# -RemoveOriginals  delete a .lsrec after its packed file has passed the check (never otherwise; a check that fails keeps both)
# -Expand           the other way: every .lsrec.zst / .lsrec.mkv becomes a .lsrec again (the offline tools read .lsrec only)
# -Newest N         only the N newest files
# zstd needs zstd.exe on the PATH (https://github.com/facebook/zstd, BSD licence) or next to this script; video needs ffmpeg (nr_lsrec looks for it on the PATH and in D:\Applications\MPV).
param(
    [string]$Folder = (Join-Path ([Environment]::GetFolderPath('MyVideos')) 'Lossless Scaling'),
    [ValidateSet('zstd', 'video')][string]$Method = 'zstd',
    [ValidateRange(1, 19)][int]$Level = 12,
    [switch]$RemoveOriginals,
    [switch]$Expand,
    [int]$Newest = 0,
    [string]$Tool = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$zstd = (Get-Command zstd -ErrorAction SilentlyContinue).Source
if (-not $zstd -and (Test-Path "$PSScriptRoot\zstd.exe")) { $zstd = "$PSScriptRoot\zstd.exe" }
if (-not $Tool) { $Tool = @("$root\addons\DLSS5NR01\build\Release\nr_lsrec.exe", "$root\out\analyze_addons\Release\nr_lsrec.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not (Test-Path $Folder)) { throw "no folder $Folder" }
$needVideo = ($Method -eq 'video' -and -not $Expand) -or $Expand
if ($Method -eq 'zstd' -and -not $zstd -and -not $Expand) { throw 'zstd.exe was not found (put it on the PATH, or next to this script)' }
if ($Method -eq 'video' -and -not $Tool) { throw 'nr_lsrec.exe was not found (build it: cmake --build addons\DLSS5NR01\build --target nr_lsrec, or pass -Tool)' }

function Size($path) { (Get-Item $path).Length }
$before = [int64]0; $after = [int64]0; $done = 0

if ($Expand) {
    foreach ($f in @(Get-ChildItem $Folder -File | Where-Object { $_.Name -like '*.lsrec.zst' -or $_.Name -like '*.lsrec.mkv' })) {
        $target = $f.FullName.Substring(0, $f.FullName.Length - 4)
        if (Test-Path $target) { Write-Host "  kept: $($f.Name) (the .lsrec is there already)"; continue }
        if ($f.Extension -eq '.zst') {
            if (-not $zstd) { throw 'zstd.exe was not found' }
            & $zstd -d -q -T0 --long=27 $f.FullName -o $target
        } else {
            if (-not $Tool) { throw 'nr_lsrec.exe was not found (pass -Tool)' }
            & $Tool unvideo $f.FullName $target | Out-Null
        }
        if ($LASTEXITCODE -ne 0) { Remove-Item $target -ErrorAction SilentlyContinue; throw "could not expand $($f.Name)" }
        Write-Host ("  expanded: {0} ({1:N0} MB)" -f (Split-Path $target -Leaf), ((Get-Item $target).Length / 1MB))
    }
    exit 0
}

$files = @(Get-ChildItem $Folder -Filter '*.lsrec' -File | Sort-Object LastWriteTime -Descending)
if ($Newest -gt 0) { $files = @($files | Select-Object -First $Newest) }
if (-not $files.Count) { Write-Host "no .lsrec in $Folder"; exit 0 }

foreach ($f in $files) {
    $packed = $f.FullName + $(if ($Method -eq 'zstd') { '.zst' } else { '.mkv' })
    if (Test-Path $packed) {
        if ($RemoveOriginals -and $Method -eq 'zstd') {
            & $zstd -t -q $packed
            if ($LASTEXITCODE -eq 0) { Remove-Item $f.FullName; Write-Host "  removed: $($f.Name) (its .zst was there and checks)"; continue }
        }
        Write-Host "  skipped: $($f.Name) (already packed)"
        continue
    }
    $started = Get-Date
    if ($Method -eq 'zstd') {
        & $zstd "-$Level" -q -T0 --long=27 --check $f.FullName -o "$packed.part"
        if ($LASTEXITCODE -ne 0) { Remove-Item "$packed.part" -ErrorAction SilentlyContinue; throw "could not pack $($f.Name)" }
        & $zstd -t -q "$packed.part"
        if ($LASTEXITCODE -ne 0) { Remove-Item "$packed.part" -ErrorAction SilentlyContinue; throw "the packed $($f.Name) did not check; both kept" }
        Move-Item "$packed.part" $packed
        $size = Size $packed
    } else {
        # nr_lsrec writes the video and its .meta, then decodes it and compares every frame with the recording's (verify=1): a failure exits non-zero and nothing is kept
        $part = $packed -replace '\.mkv$', '.part.mkv'
        & $Tool video $f.FullName $part verify=1
        if ($LASTEXITCODE -ne 0) { Remove-Item $part, "$part.meta", "$part.log", "$part.check.log" -ErrorAction SilentlyContinue; throw "the video of $($f.Name) did not check; the recording is kept" }
        Move-Item $part $packed; Move-Item "$part.meta" "$packed.meta"
        Remove-Item "$part.log", "$part.check.log" -ErrorAction SilentlyContinue
        $size = Size $packed
    }
    $before += $f.Length; $after += $size; ++$done
    Write-Host ("  packed: {0}  {1:N0} MB -> {2:N0} MB ({3:P0}) in {4:N0} s" -f $f.Name, ($f.Length / 1MB), ($size / 1MB), ($size / $f.Length), ((Get-Date) - $started).TotalSeconds)
    if ($RemoveOriginals) { Remove-Item $f.FullName }
}
if ($done) { Write-Host ("{0} files: {1:N1} GB -> {2:N1} GB" -f $done, ($before / 1GB), ($after / 1GB)) }
