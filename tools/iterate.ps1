# Regenerate, rebuild and run Skate 1 for a while, then print the end of the newest log.
param([int]$RunSeconds = 60, [switch]$SkipCodegen)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'out\build\win-amd64-relwithdebinfo'
# The SDK location comes from the CMake cache (REXSDK_DIR), so any SDK checkout works.
$sdk = ((Select-String -Path (Join-Path $bin 'CMakeCache.txt') -Pattern '^REXSDK_DIR:PATH=(.*)$').Matches[0].Groups[1].Value) -replace '/', '\'
$rexglue = Join-Path $sdk 'out\win-amd64\rexglue.exe'
$devenv = Join-Path $root 'devenv.bat'

Set-Location $root
if (-not $SkipCodegen) {
    & $rexglue codegen skate1_manifest.toml *> codegen.log
    Select-String -Path codegen.log -Pattern 'ERRORS' -Context 0,6 | ForEach-Object { $_.ToString() }
}

cmd /c "`"$devenv`" cmake --build out/build/win-amd64-relwithdebinfo" *> build.log
if ($LASTEXITCODE -ne 0) {
    Select-String -Path build.log -Pattern 'error:' | Select-Object -First 10 | ForEach-Object { $_.Line }
    exit 1
}

Set-Location $bin
# The runtime DLL builds into the SDK tree and is only copied when the exe relinks.
Copy-Item (Join-Path $sdk 'out\win-amd64\rexruntimerd.dll') . -Force
$p = Start-Process .\skate1.exe -PassThru
$deadline = (Get-Date).AddSeconds($RunSeconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
"alive_after_${RunSeconds}s=$alive"

$log = Get-ChildItem logs -Filter *.log | Sort-Object LastWriteTime | Select-Object -Last 1
Select-String -Path $log.FullName -Pattern 'critical|FATAL|error' | Select-Object -Last 8 | ForEach-Object { $_.Line }
Get-Content $log.FullName -Tail 6
