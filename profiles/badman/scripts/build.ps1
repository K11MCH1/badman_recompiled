param([ValidateRange(1, 8)][int]$Jobs = 2)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
$bin = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$cmake = Join-Path $bin 'cmake.exe'
$output = Join-Path $repo 'out/badman'
& $cmake -S $repo -B $output -G 'Visual Studio 17 2022' -A x64 -DPSPRECOMP_PROFILE=badman "-DPSPRECOMP_MSVC_MP_JOBS=$Jobs"
if ($LASTEXITCODE -ne 0) { throw 'Badman configuration failed.' }
$targets = @(
    'BadmanNative', 'BadmanFontProbe', 'BadmanMpegProbe', 'BadmanAtracProbe', 'BadmanLauncher',
    'psprecomp_tests', 'psprecomp_memory_tests', 'badman_host_tests', 'badman_sync_tests', 'badman_audio_tests',
    'badman_savedata_tests', 'badman_osk_tests', 'badman_thread_service_tests', 'badman_mpeg_tests',
    'badman_mpeg_callback_tests', 'badman_event_tests', 'badman_display_timing_tests', 'badman_dma_tests', 'badman_atrac_tests', 'badman_atrac_hle_tests', 'badman_audio_hle_tests',
    'badman_ge_tests', 'badman_disc_tests', 'badman_module_tests', 'badman_io_tests',
    'badman_registry_tests', 'badman_window_tests', 'badman_raster_tests',
    'badman_input_tests', 'badman_texture_tests', 'badman_clut_tests', 'badman_capture_file_tests'
)
& $cmake --build $output --config Release --parallel 1 --target $targets
if ($LASTEXITCODE -ne 0) { throw 'Badman compilation failed.' }
& (Join-Path $bin 'ctest.exe') --test-dir $output -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Badman/framework tests failed.' }
Write-Host "Built: $output/bin/Release/BadmanNative.exe"
