# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
param([switch]$Diagnostics,[switch]$Test,[string]$MsysRoot,[string]$BuildDir,[int]$Jobs=4)
$ErrorActionPreference='Stop'
Set-Location -LiteralPath $PSScriptRoot
if (-not $MsysRoot) {
    $candidates=@((Join-Path (Split-Path $PSScriptRoot) 'tools-local\msys64'),'C:\msys64')
    $MsysRoot=$candidates | Where-Object {Test-Path -LiteralPath (Join-Path $_ 'ucrt64\bin\gcc.exe')} | Select-Object -First 1
}
if (-not $MsysRoot) {throw 'MSYS2 UCRT64 is required. See docs/WINDOWS.md.'}
$toolBin=Join-Path $MsysRoot 'ucrt64\bin'
$env:Path="$toolBin;$env:Path"
$build=if($BuildDir){$BuildDir}elseif($Diagnostics){'out/windows-diagnostic'}else{'out/windows'}
$build=[System.IO.Path]::GetFullPath($build)
$diagnosticFlag=if($Diagnostics){'ON'}else{'OFF'}
$dependencyArgs=@()
foreach($dependency in @(@('MAGIC_ENUM','magic_enum-0.9.7'),@('MINIZ','miniz-3.1.0'),@('XBYAK','xbyak-7.23'))){
    $source=Join-Path $PSScriptRoot "out/dependency-sources/$($dependency[1])"
    if(Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt')){
        $dependencyArgs+="-DFETCHCONTENT_SOURCE_DIR_$($dependency[0])=$($source.Replace('\','/'))"
    }
}
& cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBB_LTO=OFF "-DBB_DIAGNOSTIC_ONLY=$diagnosticFlag" @dependencyArgs
if($LASTEXITCODE){throw 'Windows CMake configuration failed.'}
& cmake --build $build --parallel $Jobs
if($LASTEXITCODE){throw 'Windows build failed; no older executable was launched.'}
if($Test){
    & ctest --test-dir $build --output-on-failure
    if($LASTEXITCODE){throw 'Windows runtime tests failed.'}
    $env:BB_TEST_PROBE=Join-Path $build 'bin/bb-probe.exe'
    $env:BB_TEST_CONTENT=Join-Path $build 'bin/windows-content-test.exe'
    $env:BB_TRAINER_FIXTURE=Join-Path $build 'bin/trainer-fixture-windows.exe'
    foreach($suite in @('test_probe.py','test_prepare.py','test_link_libc.py','test_patches.py','test_content.py','test_windows_launcher.py','test_windows_graphics.py','test_windows_trainer*.py')){
        & python -m unittest discover -s tests -p $suite
        if($LASTEXITCODE){throw "Python test suite failed: $suite"}
    }
}
Write-Host "Built: $build\bin\bb-probe.exe"
