param([ValidateRange(1, 1000000000)][long]$DispatchLimit = 1000000)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$binary = Join-Path $repo 'out/badman/bin/Release/BadmanNative.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Run profiles/badman/scripts/build.ps1 first.' }
$game = Join-Path $repo 'profiles/badman/game'
$analysis = Join-Path $repo 'profiles/badman/analysis'
New-Item -ItemType Directory -Force $analysis | Out-Null
& $binary (Join-Path $game 'PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF') $game $DispatchLimit (Join-Path $analysis 'boot-trace.log')
exit $LASTEXITCODE
