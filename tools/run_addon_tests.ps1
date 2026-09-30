# Builds and runs the offline tests (no game needed; the window test needs a GPU and shows a window for a moment). By default only the suites
# that the files changed since the last commit can affect; -All runs every suite, -Only names them.
#
#   powershell -File tools\run_addon_tests.ps1                  the suites for what changed (git diff against HEAD, plus new files)
#   powershell -File tools\run_addon_tests.ps1 -Only core,gui   these suites
#   powershell -File tools\run_addon_tests.ps1 -All             every suite, as before a release (and the full Neural Rendering matrix)
#   powershell -File tools\run_addon_tests.ps1 -List            the suites and what starts each
#
# Suites:
#   core       eam_coretest (addon handling, settings, host interface, the frozen 1.0 interface, hooks) and eam_installtest (installing addons,
#              backups, diagnostics); also the exit with the GPU sampler running
#   features   eam_featurestest: ReShade passthrough and Windowed mode (switched on and off at start-up), and the exit with its watcher running
#   sample     eam_sampletest: examples\SampleAddon loaded, started and drawn by the real manager
#   update     eam_updatetest: the update check against a small local server
#   gui        eam_guitest: the manager window, three times (defaults, a saved placement, interface size 150 %)
#   installer  the Setup program's core and its file bundle, on fake Lossless Scaling folders
#   setupexe   the Setup exe end to end (silent install, repair, uninstall, the wizard window)
#   nr         Neural Rendering's requirements check and frame tap, and the model scenarios for the files changed (needs the NVIDIA SDK configured and
#              LS_DIR pointing at a Lossless Scaling folder with nvngx_dlssnr.dll); -All runs every scenario
param([string[]]$Only = @(), [switch]$All, [switch]$List)
$root = Split-Path $PSScriptRoot -Parent   # the repository folder
$mgr = "$root\manager\build\Release"
$nrBuild = "$root\addons\DLSS5NR01\build"

# name, the changed paths that start it (regular expressions on forward-slash paths), and what it builds and runs
$suites = [ordered]@{
    core      = @{ When = '^manager/(src|sdk)/|^manager/CMakeLists|^manager/tools/(core_test|test_addon|install_test|abi_)';
                   Build = @('manager', 'Lossless', 'eam_coretest', 'eam_installtest');
                   Runs = @(@('core (addon handling, host, hooks)', "$mgr\eam_coretest.exe", @()), @('exit with the GPU sampler running', "$mgr\eam_coretest.exe", @('abrupt-gpu')),
                            @('install (addons, backups, diagnostics)', "$mgr\eam_installtest.exe", @())) }
    features  = @{ When = '^manager/src/(features|core|config)/|^manager/CMakeLists|^manager/tools/features_test';
                   Build = @('manager', 'eam_featurestest');
                   Runs = @(@('features (Windowed on at start-up)', "$mgr\eam_featurestest.exe", @()), @('features (Windowed off at start-up)', "$mgr\eam_featurestest.exe", @('off')),
                            @('exit with the ReShade watcher running', "$mgr\eam_featurestest.exe", @('abrupt'))) }
    sample    = @{ When = '^manager/sdk/|^manager/src/(addon|host|config)/|^examples/|^manager/tools/sample_test';
                   Build = @('manager', 'eam_sampletest');
                   Runs = @(, @('sample addon (examples\SampleAddon)', "$mgr\eam_sampletest.exe", @())) }
    update    = @{ When = '^manager/src/update/|^manager/sdk/include/eam/version\.h|^manager/tools/update_test';
                   Build = @('manager', 'eam_updatetest');
                   Runs = @(, @('update check (local server)', "$mgr\eam_updatetest.exe", @())) }
    gui       = @{ When = '^manager/src/gui/|^manager/sdk/include/eam/(widgets|icons)\.h|^manager/tools/gui_test';
                   Build = @('manager', 'Lossless', 'eam_guitest');
                   Runs = @(@('window (defaults)', "$mgr\eam_guitest.exe", @()), @('window (saved placement)', "$mgr\eam_guitest.exe", @('place')),
                            @('window (saved placement, interface size 150 %)', "$mgr\eam_guitest.exe", @('scaled'))) }
    runtimes  = @{ When = '^manager/src/addon/runtime_files|^manager/tools/runtime_test';
                   Build = @('manager', 'eam_runtimetest');
                   Runs = @(, @('Runtimes list (a runtime file read, loaded under another path spelling)', "$mgr\eam_runtimetest.exe", @("$nrBuild\Release"))) }
    installer = @{ When = '^installer/src/core/|^installer/tests/(installer_test|payload_test)|^installer/CMakeLists|^manager/sdk/include/eam/version\.h';
                   Build = @('installer', 'setup_core', 'setup_test', 'setup_payload_test');
                   Runs = @(@('installer core (fake Lossless Scaling folders)', "$root\installer\build\Release\setup_test.exe", @()),
                            @('installer file bundle', "$root\installer\build\Release\setup_payload_test.exe", @())) }
    setupexe  = @{ When = '^installer/src/|^installer/tests/setup_exe_test|^installer/CMakeLists|^tools/package';
                   Build = @('installer', 'setup_core', 'setup_cli', 'pack_payload', 'LSAddonManagerSetup');
                   Script = "$root\installer\tests\setup_exe_test.ps1" }
    nr        = @{ When = '^addons/DLSS5NR01/(src|tools|CMakeLists)|^manager/sdk/|^tools/run_hosttest_matrix';
                   Build = @('nr', 'nr_reqtest', 'nr_taptest', 'nr_settingstest', 'nr_autotest', 'nr_tracetest', 'nr_diagtest', 'nr_rectest', 'nr_lsrec', 'nr_sreval', 'DLSS5NR01', 'DLSS4DLAA', 'FSR3UPSC', 'nr_hosttest', 'nr_selftest');
                   Runs = @(@('Neural Rendering requirements check', "$nrBuild\Release\nr_reqtest.exe", @()),
                            @('Neural Rendering settings and looks', "$nrBuild\Release\nr_settingstest.exe", @()),
                            @('Neural Rendering auto quality', "$nrBuild\Release\nr_autotest.exe", @()),
                            @('Neural Rendering "what is wrong" rules', "$nrBuild\Release\nr_diagtest.exe", @()),
                            @('Neural Rendering frame trace (ring and export)', "$nrBuild\Release\nr_tracetest.exe", @()),
                            @('The recorder (codec and file)', "$nrBuild\Release\nr_rectest.exe", @()),
                            @('Neural Rendering frame tap', "$nrBuild\Release\nr_taptest.exe", @()),
                            @('HDR: the upscaler keeps highlights (a made-up fp16 clip with bright glints; sharpening, lean, steady sharpening: no specks)', $env:ComSpec,
                              @('/c', "`"$nrBuild\Release\nr_lsrec.exe`" make `"$env:TEMP\nr_hdr_test.lsrec`" 1280 720 40 hdr=1 && `"$nrBuild\Release\nr_sreval.exe`" `"$env:TEMP\nr_hdr_test.lsrec`" `"$env:TEMP\nr_hdr_test_out`" count=30 shrink=150 backend=fsr hdr=1 hdrcheck=1 sharpen=80 restmix=50 steady=60 movecut=50 show=0")));
                   Matrix = $true }
}
# the XeSS Upscaler is built only when Intel's SDK is in external\xess (tools\fetch_xess_sdk.ps1)
if (Test-Path "$root\addons\DLSS5NR01\external\xess\inc\xess\xess_d3d12.h") { $suites.nr.Build += 'XESSUPSC' }
$buildDirs = @{ manager = "$root\manager\build"; installer = "$root\installer\build"; nr = $nrBuild }

if ($List) { foreach ($n in $suites.Keys) { Write-Host ("{0,-10} when {1}" -f $n, $suites[$n].When) }; exit 0 }

# which suites
$chosen = @()
if ($Only.Count) { $chosen = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }   # with -All: these suites at full depth
elseif ($All) { $chosen = @($suites.Keys) }
else {
    $changed = @(git -C $root diff --name-only HEAD) + @(git -C $root ls-files --others --exclude-standard) | Where-Object { $_ -and $_ -notmatch '\.md$' }
    foreach ($n in $suites.Keys) { if ($changed | Where-Object { $_ -match $suites[$n].When }) { $chosen += $n } }
    if (-not $chosen.Count) { Write-Host 'Nothing changed that a test covers (documentation only, or nothing at all). -All runs everything.'; exit 0 }
}
foreach ($n in $chosen) { if (-not $suites.Contains($n)) { Write-Host "No suite called '$n' (-List shows them)."; exit 2 } }
Write-Host ("suites: {0}" -f ($chosen -join ', '))

$total = [Diagnostics.Stopwatch]::StartNew()

# 1. Build: one build per build folder with every target the chosen suites need, MSBuild using all cores.
$targetsByDir = [ordered]@{}
foreach ($n in $chosen) {
    $b = $suites[$n].Build
    if (-not $targetsByDir.Contains($b[0])) { $targetsByDir[$b[0]] = @() }
    $targetsByDir[$b[0]] += @($b | Select-Object -Skip 1)
}
$skipped = @{}
foreach ($which in $targetsByDir.Keys) {
    $dir = $buildDirs[$which]
    if ($which -eq 'nr' -and -not (Test-Path "$dir\CMakeCache.txt")) { Write-Host '(Neural Rendering is not configured here: it needs the NVIDIA SDK; its suite is skipped)'; $skipped[$which] = $true; continue }
    if ($which -eq 'installer' -and -not (Test-Path "$dir\CMakeCache.txt")) { & cmake -S "$root\installer" -B $dir -G 'Visual Studio 17 2022' -A x64 2>&1 | Select-String -Pattern 'error' | ForEach-Object { Write-Host $_.Line } }
    [string[]]$targets = @($targetsByDir[$which] | Select-Object -Unique)
    & cmake --build $dir --config Release --parallel --target @targets 2>&1 | Select-String -Pattern ' error ' | ForEach-Object { Write-Host $_.Line }
}
Write-Host ("built in {0:0} s" -f $total.Elapsed.TotalSeconds)

# 2. What to run. The window tests (gui, features, setupexe) open windows and use the hotkey, so they run one after another; everything else
#    (including the model scenarios on the GPU) runs beside them, all at once.
$serialSuites = @('gui', 'features', 'setupexe')
$runs = @()
foreach ($n in $chosen) {
    $s = $suites[$n]
    if ($skipped[$s.Build[0]]) { continue }
    $serial = $serialSuites -contains $n
    foreach ($r in @($s.Runs)) { if ($r) { $runs += [pscustomobject]@{ Name = $r[0]; Exe = $r[1]; Args = @($r[2]); Serial = $serial; Filter = '^\s*FAIL\b|FAILED' } } }
    if ($s.Script) { $runs += [pscustomobject]@{ Name = 'Setup exe end to end'; Exe = 'powershell'; Args = @('-NoProfile', '-File', $s.Script, '-Root', $root); Serial = $true; Filter = '^\s*FAIL\b|FAILED' } }
    if ($s.Matrix) {
        if (-not $env:LS_DIR) { Write-Host '(model scenarios skipped: set LS_DIR to a Lossless Scaling folder with nvngx_dlssnr.dll)' }
        else {
            $matrixArgs = @("$root\tools\run_hosttest_matrix.py") + $(if ($All) { @() } else { @('--changed') })
            $runs += [pscustomobject]@{ Name = ('Neural Rendering model scenarios ({0})' -f $(if ($All) { 'all' } else { 'for what changed' })); Exe = 'python'; Args = $matrixArgs; Serial = $false; Filter = '^\s*FAIL\b|SCENARIOS? FAILED|rror' }
        }
    }
}

$outDir = Join-Path $env:TEMP ("eam_tests_{0}" -f $PID)
New-Item -ItemType Directory -Force $outDir | Out-Null
function Start-Run($r, $i) {
    $r | Add-Member -NotePropertyName Out -NotePropertyValue (Join-Path $outDir "$i.txt") -Force
    if ($r.Exe -notin @('python', 'powershell') -and -not (Test-Path $r.Exe)) { $r | Add-Member -NotePropertyName Missing -NotePropertyValue $true -Force; return }
    $argText = ($r.Args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $start = @{ FilePath = $r.Exe; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $r.Out; RedirectStandardError = "$($r.Out).err" }
    if ($argText) { $start.ArgumentList = $argText }
    $p = Start-Process @start
    $null = $p.Handle   # keeps the exit code readable after the process ends
    $r | Add-Member -NotePropertyName Process -NotePropertyValue $p -Force
}
function Finish-Run($r) {
    if ($r.Process) { $r.Process.WaitForExit() }
}

$i = 0
foreach ($r in ($runs | Where-Object { -not $_.Serial })) { Start-Run $r ($i++) }
foreach ($r in ($runs | Where-Object { $_.Serial })) { Start-Run $r ($i++); Finish-Run $r }
foreach ($r in $runs) { Finish-Run $r }

# 3. One line per test program, and the failing checks of any that failed.
$fail = 0
foreach ($r in $runs) {
    if ($r.Missing) { Write-Host ("  FAIL  {0}: not built ({1})" -f $r.Name, $r.Exe); $fail++; continue }
    $code = $r.Process.ExitCode
    $seconds = ($r.Process.ExitTime - $r.Process.StartTime).TotalSeconds
    Write-Host ("  {0,-4}  {1}  ({2:0} s)" -f $(if ($code -eq 0) { 'ok' } else { 'FAIL' }), $r.Name, $seconds)
    if ($code -ne 0) {
        $fail++
        $text = @(Get-Content $r.Out -ErrorAction SilentlyContinue) + @(Get-Content "$($r.Out).err" -ErrorAction SilentlyContinue)
        $text | Where-Object { $_ -cmatch $r.Filter } | Select-Object -First 20 | ForEach-Object { Write-Host "        $_" }
        Write-Host "        (exit $code; full output in $($r.Out))"
    }
}
Write-Host ("{0:0} s in all" -f $total.Elapsed.TotalSeconds)
if ($fail) { Write-Host "$fail test program(s) failed"; exit 1 }
Remove-Item -LiteralPath $outDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'all addon tests passed'
