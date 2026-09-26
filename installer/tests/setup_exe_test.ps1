# End-to-end test of LSAddonManagerSetup.exe on fake Lossless Scaling folders in %TEMP% (the real install is never touched):
#   * the silent mode: status, install, install again, uninstall, and the refusals (not a Lossless Scaling folder, no folder, Lossless Scaling running)
#   * that settings and other addons survive, and that the files come out byte for byte as they went in
#   * the wizard window: it opens (on a folder, and with none given), and closes itself, without changing anything
# Needs the exes built (run_addon_tests.ps1 does that) and the manager's Lossless.dll.
#   powershell -File setup_exe_test.ps1 -Root <repository folder>
# -NoWindow skips the checks that open the wizard window (a machine with no desktop, such as a build server, cannot show one).
param([Parameter(Mandatory = $true)][string]$Root, [switch]$NoWindow)
$ErrorActionPreference = 'Stop'
$setup = "$Root\installer\build\Release\LSAddonManagerSetup.exe"
$fakes = "$Root\installer\build\Release"
$ours = "$Root\manager\build\Release\Lossless.dll"
$fail = 0
$rememberedBefore = (Get-ItemProperty 'HKCU:\Software\LSAddonManager' -ErrorAction SilentlyContinue).LastFolder
function Check($what, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what  ($detail)"; $script:fail++ }
}
foreach ($f in @($setup, "$fakes\fake_original.dll", "$fakes\fake_original_new.dll", $ours)) { if (-not (Test-Path $f)) { Write-Host "  FAIL  not built: $f"; exit 1 } }
# SHA-256 through .NET: Get-FileHash is missing when Windows PowerShell is started from PowerShell 7 (it inherits 7's module path)
function Hash($p) {
    if (-not (Test-Path $p)) { return '' }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes((Resolve-Path $p).Path))) -replace '-', '' } finally { $sha.Dispose() }
}

$tmp = Join-Path $env:TEMP "setup_exe_test_$PID"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory $tmp | Out-Null

function Run($exeArgs, $name) {   # runs the (windowless) exe, returns exit code and its log text
    $log = "$tmp\$name.log"
    $p = Start-Process -FilePath $setup -ArgumentList ($exeArgs + @('--no-remember', '--log', "`"$log`"")) -PassThru -Wait -WindowStyle Hidden
    [pscustomobject]@{ Code = $p.ExitCode; Log = $(if (Test-Path $log) { Get-Content $log -Raw } else { '' }) }
}
function MakeLs($name, $dll = 'fake_original.dll') {
    $d = "$tmp\$name"
    New-Item -ItemType Directory "$d\addons\OtherAddon" -Force | Out-Null
    Copy-Item "$env:SystemRoot\System32\cmd.exe" "$d\LosslessScaling.exe"
    Copy-Item "$fakes\$dll" "$d\Lossless.dll"
    Set-Content "$d\addons\config.json" '{ "addons": { "mine": { "x": "1" } } }'
    Set-Content "$d\addons\OtherAddon\other.dll" 'someone else''s addon'
    $d
}

# The files to install, laid out like the release zip
$payload = "$tmp\payload"
New-Item -ItemType Directory "$payload\addons\DLSS5NR01" -Force | Out-Null
Copy-Item $ours "$payload\Lossless.dll"
Set-Content "$payload\manager-icon.ico" 'icon'; Set-Content "$payload\manager-icon.png" 'png'
Set-Content "$payload\addons\DLSS5NR01\DLSS5NR01.dll" 'addon dll'
Set-Content "$payload\addons\DLSS5NR01\addon.json" '{ "id": "DLSS5NR01" }'
$ls = MakeLs 'ls1'
$configBefore = Get-Content "$ls\addons\config.json" -Raw

Write-Host '== silent mode'
$r = Run @('--silent', 'status', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'status1'
Check 'status of a plain Lossless Scaling folder: not installed, and it knows what it carries' ($r.Code -eq 0 -and $r.Log -match 'situation: not installed' -and $r.Log -match 'payload: \d+\.\d+\.\d+') $r.Log
$r = Run @('--version', '--payload', "`"$payload`"") 'version'
Check '--version says what the setup carries' ($r.Code -eq 0 -and $r.Log -match '^payload \d+\.\d+\.\d+') $r.Log

$r = Run @('--silent', 'install', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'install1'
Check 'install succeeds' ($r.Code -eq 0) "$($r.Code) $($r.Log)"
Check 'the original Lossless.dll is kept as Lossless_original.dll, byte for byte' ((Hash "$ls\Lossless_original.dll") -eq (Hash "$fakes\fake_original.dll"))
Check 'our Lossless.dll is in place, byte for byte' ((Hash "$ls\Lossless.dll") -eq (Hash $ours))
Check 'the addon files came across, byte for byte' ((Hash "$ls\addons\DLSS5NR01\DLSS5NR01.dll") -eq (Hash "$payload\addons\DLSS5NR01\DLSS5NR01.dll") -and (Hash "$ls\addons\DLSS5NR01\addon.json") -eq (Hash "$payload\addons\DLSS5NR01\addon.json"))
Check 'the icons came across' ((Test-Path "$ls\manager-icon.ico") -and (Test-Path "$ls\manager-icon.png"))
Check 'the person''s settings and other addons are untouched' ((Get-Content "$ls\addons\config.json" -Raw) -eq $configBefore -and (Test-Path "$ls\addons\OtherAddon\other.dll"))
Check 'the replaced original went to a backups folder' ((Get-ChildItem "$ls\backups" -Recurse -Filter 'Lossless.dll' -ErrorAction SilentlyContinue | Measure-Object).Count -ge 1)
$r = Run @('--silent', 'status', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'status2'
Check 'status now says installed, with the version' ($r.Log -match 'situation: installed' -and $r.Log -match 'installed: \d+\.\d+\.\d+') $r.Log

$r = Run @('--silent', 'install', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'install2'
Check 'installing again (reinstall) succeeds and changes nothing that matters' ($r.Code -eq 0 -and (Hash "$ls\Lossless.dll") -eq (Hash $ours) -and (Hash "$ls\Lossless_original.dll") -eq (Hash "$fakes\fake_original.dll")) "$($r.Code) $($r.Log)"

# Lossless Scaling updates itself over ours: repair keeps the new original
Copy-Item "$fakes\fake_original_new.dll" "$ls\Lossless.dll" -Force
$r = Run @('--silent', 'status', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'status3'
Check 'after a Lossless Scaling update the folder is recognised' ($r.Log -match 'situation: after a Lossless Scaling update') $r.Log
$r = Run @('--silent', 'install', '--folder', "`"$ls`"", '--payload', "`"$payload`"") 'repair'
Check 'repair keeps the new original and puts ours back' ($r.Code -eq 0 -and (Hash "$ls\Lossless_original.dll") -eq (Hash "$fakes\fake_original_new.dll") -and (Hash "$ls\Lossless.dll") -eq (Hash $ours)) "$($r.Code) $($r.Log)"

$r = Run @('--silent', 'uninstall', '--folder', "`"$ls`"") 'uninstall'
Check 'uninstall puts Lossless Scaling''s own Lossless.dll back' ($r.Code -eq 0 -and (Hash "$ls\Lossless.dll") -eq (Hash "$fakes\fake_original_new.dll")) "$($r.Code) $($r.Log)"
Check 'uninstall leaves the addons and the settings' ((Test-Path "$ls\addons\DLSS5NR01\DLSS5NR01.dll") -and (Get-Content "$ls\addons\config.json" -Raw) -eq $configBefore)

Write-Host '== refusals'
$other = "$tmp\not_ls"; New-Item -ItemType Directory $other | Out-Null; Set-Content "$other\file.txt" 'x'
$r = Run @('--silent', 'install', '--folder', "`"$other`"", '--payload', "`"$payload`"") 'notls'
Check 'a folder that is not Lossless Scaling is refused and left alone' ($r.Code -ne 0 -and (Get-ChildItem $other | Measure-Object).Count -eq 1) "$($r.Code) $($r.Log)"
$r = Run @('--silent', 'install', '--payload', "`"$payload`"") 'nofolder'
Check 'silent mode without a folder is refused (exit 2)' ($r.Code -eq 2) "$($r.Code) $($r.Log)"
$r = Run @('--silent', 'install', '--folder', "`"$ls`"") 'nopayload'
Check 'an exe without files says so and does nothing (exit 2)' ($r.Code -eq 2 -and $r.Log -match 'carries none|no files') "$($r.Code) $($r.Log)"
$bad = "$tmp\badpayload"; New-Item -ItemType Directory $bad | Out-Null; Set-Content "$bad\Lossless.dll" 'not a real dll'
$before = Hash "$ls\Lossless.dll"
$r = Run @('--silent', 'install', '--folder', "`"$ls`"", '--payload', "`"$bad`"") 'badpayload'
Check 'files that are not ours are refused before anything changes' ($r.Code -ne 0 -and (Hash "$ls\Lossless.dll") -eq $before) "$($r.Code) $($r.Log)"

$ls2 = MakeLs 'ls2'
$run = Start-Process -FilePath "$ls2\LosslessScaling.exe" -ArgumentList '/k' -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 500
    $r = Run @('--silent', 'install', '--folder', "`"$ls2`"", '--payload', "`"$payload`"") 'running'
    Check 'while Lossless Scaling runs from that folder, install is refused and nothing changes' ($r.Code -ne 0 -and (Hash "$ls2\Lossless.dll") -eq (Hash "$fakes\fake_original.dll") -and -not (Test-Path "$ls2\Lossless_original.dll")) "$($r.Code) $($r.Log)"
} finally { Stop-Process -Id $run.Id -Force -ErrorAction SilentlyContinue }

if (-not $NoWindow) {
Write-Host '== the wizard window (opens by itself, closes by itself)'
$ls3 = MakeLs 'ls3'
$before = Hash "$ls3\Lossless.dll"
foreach ($case in @(
        @{ Name = 'on a given folder'; Args = @('--folder', "`"$ls3`"", '--payload', "`"$payload`"", '--test-close-ms', '1500') },
        @{ Name = 'with no folder given (the folder chooser or the only folder found)'; Args = @('--payload', "`"$payload`"", '--test-close-ms', '1500') },
        @{ Name = 'without files to install (the damaged-setup page)'; Args = @('--folder', "`"$ls3`"", '--test-close-ms', '1500') })) {
    $p = Start-Process -FilePath $setup -ArgumentList $case.Args -PassThru
    $closed = $p.WaitForExit(20000)
    if (-not $closed) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Check "the window $($case.Name) opens and closes itself with exit code 0" ($closed -and $p.ExitCode -eq 0) "closed=$closed code=$($p.ExitCode)"
}
Write-Host '== only one Setup window at a time'
$name = "test$PID"
$common = @('--folder', "`"$ls3`"", '--payload', "`"$payload`"", '--instance-name', $name)
$first = Start-Process -FilePath $setup -ArgumentList ($common + @('--test-close-ms', '6000')) -PassThru
Start-Sleep -Milliseconds 2000
$second = Start-Process -FilePath $setup -ArgumentList ($common + @('--test-close-ms', '6000')) -PassThru
$secondEnded = $second.WaitForExit(8000)
if (-not $secondEnded) { Stop-Process -Id $second.Id -Force -ErrorAction SilentlyContinue }
Check 'a second Setup started while one is open ends at once with exit code 4, opening nothing' ($secondEnded -and $second.ExitCode -eq 4) "ended=$secondEnded code=$($second.ExitCode)"
$firstEnded = $first.WaitForExit(20000)
if (-not $firstEnded) { Stop-Process -Id $first.Id -Force -ErrorAction SilentlyContinue }
Check 'the first one is unaffected and closes normally (exit code 0)' ($firstEnded -and $first.ExitCode -eq 0) "ended=$firstEnded code=$($first.ExitCode)"
$third = Start-Process -FilePath $setup -ArgumentList ($common + @('--test-close-ms', '1500')) -PassThru
$thirdEnded = $third.WaitForExit(20000)
if (-not $thirdEnded) { Stop-Process -Id $third.Id -Force -ErrorAction SilentlyContinue }
Check 'once the first has closed, Setup can be opened again' ($thirdEnded -and $third.ExitCode -eq 0) "ended=$thirdEnded code=$($third.ExitCode)"
$same = (Hash "$ls3\Lossless.dll") -eq $before; $orig = Test-Path "$ls3\Lossless_original.dll"; $bk = Test-Path "$ls3\backups"
$left = (Get-ChildItem $ls3 -Force -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($ls3.Length) }) -join ' '
Check 'opening the window changed nothing in the folder' ($same -and -not $orig -and -not $bk) "Lossless.dll unchanged=$same, Lossless_original.dll=$orig, backups=$bk; in the folder: $left"
Check 'the window left no write-test file behind' (-not (Get-ChildItem $ls3 -Force -Filter '.echo_setup_write_test_*' -ErrorAction SilentlyContinue))

Write-Host "== the manager's update (--update-when-closed: waits for Lossless Scaling to close, then updates)"
$ls4 = MakeLs 'ls4'
$before4 = Hash "$ls4\Lossless.dll"
$fakeLs = Start-Process -FilePath "$ls4\LosslessScaling.exe" -ArgumentList '/k' -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 500
    $upd = Start-Process -FilePath $setup -ArgumentList @('--folder', "`"$ls4`"", '--payload', "`"$payload`"", '--update-when-closed', '--test-close-ms', '1500', '--instance-name', "upd$PID") -PassThru
    Start-Sleep -Milliseconds 3000
    Check 'while Lossless Scaling runs, it waits and changes nothing' (-not $upd.HasExited -and (Hash "$ls4\Lossless.dll") -eq $before4 -and -not (Test-Path "$ls4\Lossless_original.dll")) "exited=$($upd.HasExited)"
} finally { Stop-Process -Id $fakeLs.Id -Force -ErrorAction SilentlyContinue }
$updEnded = $upd.WaitForExit(30000)
if (-not $updEnded) { Stop-Process -Id $upd.Id -Force -ErrorAction SilentlyContinue }
Check 'once Lossless Scaling has closed, it updates by itself and closes (exit code 0)' ($updEnded -and $upd.ExitCode -eq 0) "ended=$updEnded code=$(if ($updEnded) { $upd.ExitCode })"
Check '...with the original kept and ours in place' ((Test-Path "$ls4\Lossless_original.dll") -and (Hash "$ls4\Lossless_original.dll") -eq $before4 -and (Hash "$ls4\Lossless.dll") -eq (Hash "$payload\Lossless.dll"))

}   # end of the window checks

$mine = (Get-ItemProperty 'HKCU:\Software\LSAddonManager' -ErrorAction SilentlyContinue).LastFolder
Check 'the person''s remembered folder was not touched by any of it' (-not $mine -or $mine -eq $rememberedBefore) "now: $mine"
Remove-Item 'HKCU:\Software\LSAddonManager\SetupTest' -Recurse -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($fail) { Write-Host "SETUP EXE TEST FAILED ($fail failed)"; exit 1 } else { Write-Host 'SETUP EXE TEST PASSED (0 failed)' }
