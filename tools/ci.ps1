# The tests that need neither a GPU nor a game nor NVIDIA's SDK, from a clean checkout: what the GitHub Actions build runs on every push, and what anyone can run to
# see that a checkout is healthy. It configures and builds what it needs (Dear ImGui and MinHook are fetched at pinned versions), runs the tests, and builds the sample
# addon the way its README describes. It does not build Neural Rendering (that needs NVIDIA's SDK, which cannot be redistributed; only the addon's offline tests are built, from addons\DLSS5NR01\tests) and does not open any window.
#   powershell -File tools\ci.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$failed = @()

function Section($text) { Write-Host ""; Write-Host "=== $text" }
function Configure($src, $build, $extra = @()) {
    if (Test-Path "$build\CMakeCache.txt") { return }
    & cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 @extra
    if ($LASTEXITCODE -ne 0) { throw "cmake could not configure $src" }
}
function Build($build, $targets) {
    & cmake --build $build --config Release --target @targets -- /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "the build failed in $build" }
}
function Run($name, $exe, $testArgs = @()) {
    Write-Host "--- $name"
    & $exe @testArgs
    if ($LASTEXITCODE -ne 0) { $script:failed += $name; Write-Host "FAILED: $name (exit $LASTEXITCODE)" }
}

Section 'one version everywhere'
$release = (Select-String -Path "$root\manager\sdk\include\eam\version.h" -Pattern 'EAM_VERSION_STRING "([^"]+)"').Matches[0].Groups[1].Value
if ((Select-String -Path "$root\manager\CMakeLists.txt" -Pattern 'project\(LSAddonManager VERSION ([0-9.]+)\)').Matches[0].Groups[1].Value -ne $release) { $failed += 'manager CMakeLists version'; Write-Host "manager\CMakeLists.txt is not $release" }
foreach ($manifest in @("$root\addons\DLSS5NR01\addon.json") + @(Get-ChildItem "$root\addons\DLSS5NR01\products\*\addon.json" | ForEach-Object FullName)) {
    $version = (Get-Content $manifest -Raw | ConvertFrom-Json).version
    if ($version -ne $release) { $failed += "version of $manifest"; Write-Host "$manifest says $version, the release is $release" }
}
Write-Host "  the release is $release"

Section 'the manager and its tests'
Configure "$root\manager" "$root\manager\build"
Build "$root\manager\build" @('Lossless', 'eam_installtest', 'eam_coretest', 'eam_updatetest', 'eam_sampletest')
Run 'addon install' "$root\manager\build\Release\eam_installtest.exe"
Run 'addon handling' "$root\manager\build\Release\eam_coretest.exe"
Run 'update check (a local server, no internet)' "$root\manager\build\Release\eam_updatetest.exe"
Run 'sample addon' "$root\manager\build\Release\eam_sampletest.exe"

Section 'the installer'
Configure "$root\installer" "$root\installer\build"
Build "$root\installer\build" @('setup_core', 'setup_cli', 'setup_test', 'setup_payload_test', 'pack_payload', 'LSAddonManagerSetup')
Run 'installer core (fake Lossless Scaling folders)' "$root\installer\build\Release\setup_test.exe"
Run 'installer file bundle' "$root\installer\build\Release\setup_payload_test.exe"
Run 'Setup exe, silent mode' 'powershell' @('-NoProfile', '-File', "$root\installer\tests\setup_exe_test.ps1", '-Root', $root, '-NoWindow')

Section 'the addon''s offline tests (settings, auto quality, diagnosis, frame trace, recorder files: no NVIDIA SDK, no card)'
Configure "$root\addons\DLSS5NR01\tests" "$root\addons\DLSS5NR01\tests\build"
Build "$root\addons\DLSS5NR01\tests\build" @('nr_settingstest', 'nr_autotest', 'nr_diagtest', 'nr_tracetest', 'nr_rectest')
Run 'addon settings (a new install loads the defaults, saved settings load back)' "$root\addons\DLSS5NR01\tests\build\Release\nr_settingstest.exe"
Run 'addon auto quality' "$root\addons\DLSS5NR01\tests\build\Release\nr_autotest.exe"
Run 'addon what-is-wrong rules' "$root\addons\DLSS5NR01\tests\build\Release\nr_diagtest.exe"
Run 'addon frame trace' "$root\addons\DLSS5NR01\tests\build\Release\nr_tracetest.exe"
Run 'addon recorder files' "$root\addons\DLSS5NR01\tests\build\Release\nr_rectest.exe"

Section 'the sample addon, built as its README says'
Configure "$root\examples\SampleAddon" "$root\examples\SampleAddon\build"
Build "$root\examples\SampleAddon\build" @('SampleAddon')
if (-not (Test-Path "$root\examples\SampleAddon\build\Release\SampleAddon.dll")) { $failed += 'sample addon standalone build' }

Write-Host ""
if ($failed.Count) { Write-Host "FAILED: $($failed -join '; ')"; exit 1 }
Write-Host 'ALL CI TESTS PASSED'
