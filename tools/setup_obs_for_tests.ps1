# Sets OBS Studio up for recording what the addons show, for finding flicker and trailing: a profile and a scene collection named
# "LS Addon Tests" beside the owner's own (theirs are not touched), then optionally starts OBS on them with its replay buffer running.
#   * the whole display (Display Capture, not Game Capture: that would take the game's window without Lossless Scaling's picture),
#     at the display's own size and 60 fps;
#   * NVIDIA NVENC AV1 at CQP 16 (close to lossless: compression must not hide flicker or make its own), MKV (a crash keeps the file);
#   * a replay buffer of the last 30 s, saved with F1: the addons' own save-recording key, so one press keeps both, the same moment;
#   * files in %USERPROFILE%\Videos\Lossless Scaling\OBS, beside the addons' recordings (not a OneDrive Videos folder).
# Refuses while OBS runs (it would overwrite the files on exit). Rerunning rewrites only the "LS Addon Tests" files.
#   powershell -File tools\setup_obs_for_tests.ps1 [-Launch] [-Width 3840] [-Height 2160] [-Fps 60] [-Cqp 16] [-ReplaySeconds 30] [-SaveKey F1]
param(
    [switch]$Launch,
    [int]$Width = 3840, [int]$Height = 2160, [int]$Fps = 60,
    [int]$Cqp = 16, [int]$ReplaySeconds = 30, [string]$SaveKey = 'F1'
)
$ErrorActionPreference = 'Stop'
$name = 'LS Addon Tests'
$obs = 'C:\Program Files\obs-studio\bin\64bit\obs64.exe'
$cfg = Join-Path $env:APPDATA 'obs-studio'
$utf8 = New-Object System.Text.UTF8Encoding($false)   # OBS reads its JSON without a byte order mark

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    if ($Launch) { Write-Host 'OBS is already running (close it to set it up again).'; return }
    throw 'OBS is running: close it first (it writes its settings on exit).'
}
# the display: OBS names each by its device path, which its own log lists ("output 0: id=..."); the first one found (the owner has one)
$displayId = ''
$log = Get-ChildItem (Join-Path $cfg 'logs') -Filter *.txt -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) { $m = Select-String -Path $log.FullName -Pattern '^\S+:\s+id=(\\\\\?\\DISPLAY\S+)' | Select-Object -First 1; if ($m) { $displayId = $m.Matches[0].Groups[1].Value } }
if (-not $displayId) { Write-Host 'No display found in OBS''s log (start OBS once first): pick it in the Display source''s properties.' }
if ($true) {
    $videos = Join-Path $env:USERPROFILE 'Videos\Lossless Scaling\OBS'   # as the addons' recorder: not a OneDrive Videos folder (it would sync gigabytes)
    New-Item -ItemType Directory -Force $videos | Out-Null
    $profileDir = Join-Path $cfg "basic\profiles\$name"
    New-Item -ItemType Directory -Force $profileDir | Out-Null
    $esc = $videos.Replace('\', '\\')
    $hotkey = '{"ReplayBuffer.Save":[{"key":"OBS_KEY_' + $SaveKey + '"}]}'
    $ini = @"
[General]
Name=$name

[Output]
Mode=Advanced
FilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss

[AdvOut]
RecType=Standard
RecFilePath=$esc
RecFormat2=mkv
RecEncoder=obs_nvenc_av1_tex
RecAudioEncoder=ffmpeg_aac
RecTracks=1
RecUseRescale=false
RecRB=true
RecRBTime=$ReplaySeconds
RecRBSize=16384
Encoder=obs_nvenc_h264_tex
AudioEncoder=ffmpeg_aac

[Video]
BaseCX=$Width
BaseCY=$Height
OutputCX=$Width
OutputCY=$Height
FPSType=0
FPSCommon=$Fps
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Full

[Audio]
SampleRate=48000
ChannelSetup=Stereo

[Hotkeys]
ReplayBuffer=$hotkey
"@
    [IO.File]::WriteAllText((Join-Path $profileDir 'basic.ini'), $ini, $utf8)
    $encoder = '{"rate_control":"CQP","cqp":' + $Cqp + ',"preset2":"p5","tune":"hq","multipass":"disabled","keyint_sec":2,"bf":2}'
    [IO.File]::WriteAllText((Join-Path $profileDir 'recordEncoder.json'), $encoder, $utf8)
    [IO.File]::WriteAllText((Join-Path $profileDir 'streamEncoder.json'), '{}', $utf8)

    $display = [guid]::NewGuid().ToString(); $scene = [guid]::NewGuid().ToString()
    $collection = @"
{
  "name": "$name",
  "current_scene": "Screen",
  "current_program_scene": "Screen",
  "scene_order": [ { "name": "Screen" } ],
  "sources": [
    { "id": "monitor_capture", "versioned_id": "monitor_capture", "name": "Display", "uuid": "$display", "enabled": true, "flags": 0,
      "volume": 1.0, "mixers": 0, "settings": { "method": 0, "monitor_id": "$($displayId.Replace('\', '\\'))", "capture_cursor": false } },
    { "id": "scene", "versioned_id": "scene", "name": "Screen", "uuid": "$scene", "enabled": true, "flags": 0, "volume": 1.0, "mixers": 0,
      "settings": { "id_counter": 1, "custom_size": false, "items": [
        { "name": "Display", "source_uuid": "$display", "visible": true, "locked": true, "id": 1, "align": 5,
          "pos": { "x": 0.0, "y": 0.0 }, "scale": { "x": 1.0, "y": 1.0 }, "rot": 0.0, "bounds_type": 0, "bounds": { "x": 0.0, "y": 0.0 },
          "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0, "scale_filter": "disable" } ] } }
  ],
  "groups": [], "quick_transitions": [], "transitions": [], "saved_projectors": [],
  "current_transition": "Cut", "transition_duration": 300, "preview_locked": false, "scaling_enabled": false, "modules": {}
}
"@
    [IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\$name.json"), $collection, $utf8)
    Write-Host "OBS set up: profile and scene collection '$name' (the owner's own are untouched)."
    Write-Host "  ${Width}x$Height at $Fps fps, NVENC AV1 CQP $Cqp, MKV, replay buffer $ReplaySeconds s saved with $SaveKey, into $videos"
}
if ($Launch) {
    if (Get-Process obs64 -ErrorAction SilentlyContinue) { Write-Host 'OBS is already running.'; return }
    Start-Process -FilePath $obs -WorkingDirectory (Split-Path $obs) -ArgumentList @('--profile', "`"$name`"", '--collection', "`"$name`"", '--startreplaybuffer', '--minimize-to-tray', '--disable-shutdown-check')
    Write-Host "OBS started on '$name' with the replay buffer running (in the tray). $SaveKey saves the last $ReplaySeconds s."
}
