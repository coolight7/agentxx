@echo off
rem set utf8
chcp 65001 > NUL

set "crude_dir=%CD%"

set "script_dir=%~dp0"
set "src_dir=%script_dir%\..\"
set "build_dir=%script_dir%\..\build\windows-debug"

cd %script_dir%
set script_dir=%CD%

cd %src_dir%
set src_dir=%CD%

mkdir %build_dir%
cd %build_dir%
set build_dir=%CD%

cd %crude_dir%

rem MSVC output english
set VSLANG=1033
set VCPKG_ROOT=

rem find Ragel
set "PATH=%PATH%;%localappdata%\Microsoft\WinGet\Links\"

cmake -DAGENTXX_BUILD_CLIENT=ON ^
    -DAGENTXX_BUILD_TEST=ON ^
    -DAGENTXX_ENABLE_VECTORSCAN=OFF ^
    -DAGENTXX_ENABLE_HYPERSCAN=ON ^
    -DAGENTXX_ENABLE_CODEGRAPH=ON ^
    -DXX_BUILD_TYPE=DEBUG ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_CONFIGURATION_TYPES=Debug ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 ^
    -A x64 -B %build_dir% -S %src_dir%

if %ERRORLEVEL% neq 0 (
    echo cmake config failed!
    exit /b 1
)

cmake --build "%build_dir%" --config Debug
if %ERRORLEVEL% neq 0 (
    echo cmake build failed!
    exit /b 1
)

cmake --install "%build_dir%" --config Debug
if %ERRORLEVEL% neq 0 (
    echo cmake install failed!
    exit /b 1
)
