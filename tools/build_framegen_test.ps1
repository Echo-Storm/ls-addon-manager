# Builds the FSR Upscaler WITH our frame generation (a prototype, CMake NR_FRAMEGEN=ON) into its own build folder, addons\DLSS5NR01\build_fg, so that the
# normal build (addons\DLSS5NR01\build, NR_FRAMEGEN=OFF, what releases are made from) is left alone. Then deploy it for a test:
#
#   powershell -File tools\build_framegen_test.ps1
#   powershell -File tools\deploy.ps1 -What echofg -BuildDir build_fg [-LsDir '<Lossless Scaling folder>']      (close Lossless Scaling and the game first)
#
# In the addon's panel (the FSR Upscaler): "Frame generation of our own (prototype)", then "Make the frames with our own synthesis instead of FSR (test)" and
# "Frames for each real frame" (x2 to x9). Turn Lossless Scaling's own frame generation OFF first. A game running at its own pace works best with a fixed
# frame rate cap (the generated frames are spaced evenly between the real ones, which are shown half a frame or more later than without it).
#
# The tests that do not need Lossless Scaling: nr_fgpresent (the presenter on a real swap chain), nr_fgeval own=1 (scores on a recording); see tools\README.md.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = "$root\addons\DLSS5NR01"; $build = "$src\build_fg"
& cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 -DNR_FRAMEGEN=ON
if ($LASTEXITCODE -ne 0) { throw 'cmake could not configure the frame generation test build' }
& cmake --build $build --config Release --target ECHOFG nr_fgpresent -- /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'the frame generation test build failed' }
Write-Host ''
Write-Host "Built: $build\Release\ECHOFG.dll (the EchoFG addon). Deploy it with:"
Write-Host "  powershell -File tools\deploy.ps1 -What echofg -BuildDir build_fg"
Write-Host 'Check the presenter first (needs a GPU, no Lossless Scaling):'
Write-Host "  $build\Release\nr_fgpresent.exe mult=4"
