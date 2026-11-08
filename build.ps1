# Builds CrusherFX.exe and runs the offline tests. Needs g++ from WinLibs:
#   winget install BrechtSanders.WinLibs.POSIX.UCRT
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $found = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } | Where-Object { Test-Path "$_\g++.exe" } | Select-Object -First 1
    if (-not $found) { throw 'g++ not found. Run: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
    $env:Path = "$found;$env:Path"
}

New-Item -ItemType Directory -Force build | Out-Null
$warn = '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wno-cast-function-type'

& g++ -std=c++17 @warn -O1 tests\selftest.cpp -o build\selftest.exe -static
if ($LASTEXITCODE) { throw 'selftest failed to compile' }
& .\build\selftest.exe
if ($LASTEXITCODE) { throw 'selftest failed' }

Push-Location res
& windres app.rc -O coff -o ..\build\app.res
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { throw 'windres failed' }

$opt = '-std=c++17', '-Os', '-s', '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections',
       '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00', '-DNOMINMAX'
$link = '-static', '-mwindows', '-municode', '-Wl,--gc-sections'
$libs = '-lws2_32', '-lbthprops', '-ldwmapi', '-lgdi32', '-luser32', '-lshell32', '-ladvapi32'
& g++ @opt @warn src\main.cpp src\headset.cpp build\app.res -o CrusherFX.exe @link @libs
if ($LASTEXITCODE) { throw 'build failed' }

& g++ @opt @warn tests\hwtest.cpp src\headset.cpp -o build\hwtest.exe -static -municode @libs
if ($LASTEXITCODE) { throw 'hwtest failed to compile' }

$size = [math]::Round((Get-Item CrusherFX.exe).Length / 1KB)
"Built CrusherFX.exe ($size KB)"
