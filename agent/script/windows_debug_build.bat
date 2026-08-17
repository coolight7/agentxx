@echo off
rem set utf8
chcp 65001 > NUL
rem Tell the build chain that the console code page is UTF-8(65001).
rem CMake generates Directory.Build.targets which forces MSBuild CustomBuild
rem tasks to decode child-process output as UTF-8 (fix garbled CJK in nested builds).
rem NOTE: keep this batch file pure ASCII, multi-byte chars after chcp 65001 may
rem trigger cmd.exe batch file-pointer misalignment bug.
set AgentxxBuildConsoleCP=65001

set "crude_dir=%CD%"

set "script_dir=%~dp0"
set "src_dir=%script_dir%\..\"
set "build_dir=%script_dir%\..\build\windows-debug"
set "output_dir=%script_dir%\..\build\windows-debug-output"

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

set BOOST_ROOT="%src_dir%/third_party/boost-windows-build-debug/"
set OPENSSL_ROOT_DIR="%src_dir%/third_party/OpenSSL-windows-build/"

cmake -DAGENTXX_BUILD_CLIENT=ON ^
    -DAGENTXX_BUILD_TEST=ON ^
    -DAGENTXX_BUILD_BENCHMARK=OFF ^
    -DAGENTXX_ENABLE_HYPERSCAN=ON ^
    -DAGENTXX_ENABLE_CODEGRAPH=ON ^
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON ^
    -DBOOST_ROOT="%BOOST_ROOT%" ^
    -DOPENSSL_ROOT_DIR="%OPENSSL_ROOT_DIR%" ^
    -DXX_IS_RELEASE_D=0 ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_CONFIGURATION_TYPES=Debug ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 ^
    -A x64 -B %build_dir% -S %src_dir%

if %ERRORLEVEL% neq 0 (
    echo cmake config failed!
    exit /b 1
)

rem parallel build jobs, default 6; set AGENTXX_BUILD_PARALLEL to override
rem e.g. set AGENTXX_BUILD_PARALLEL=12 && windows_debug_build.bat
if "%AGENTXX_BUILD_PARALLEL%"=="" set AGENTXX_BUILD_PARALLEL=6

cmake --build "%build_dir%" --config Debug --parallel %AGENTXX_BUILD_PARALLEL%
if %ERRORLEVEL% neq 0 (
    echo cmake build failed!
    exit /b 1
)

cmake --install "%build_dir%" --config Debug
if %ERRORLEVEL% neq 0 (
    echo cmake install failed!
    exit /b 1
)

rem Copy the exe to the output dir. Failure (e.g. target file locked by a
rem running process / output dir missing) is only reported, and does NOT fail
rem the build, because the compiled artifact in %build_dir% is already valid.
rem NOTE: keep ASCII only - multi-byte chars break cmd.exe batch parsing.
xcopy /E /I /H /Y "%build_dir%\exec" "%output_dir%" >NUL 2>&1
if errorlevel 1 echo copy `exec` to "%output_dir%" failed (ignored)
exit /b 0
