@echo off
rem set utf8
chcp 65001 > NUL
rem Tell the build chain that the console code page is UTF-8(65001).
rem CMake generates Directory.Build.targets which forces MSBuild CustomBuild
rem tasks to decode child-process output as UTF-8 (fix garbled CJK in nested builds).
rem NOTE: keep this batch file pure ASCII, multi-byte chars after chcp 65001 may
rem trigger cmd.exe batch file-pointer misalignment bug.
set AgentxxBuildConsoleCP=65001

rem ===== resolve absolute paths: script dir / source root / build dir =====
rem - %~dp0 already carries the drive, so derive every path from it instead of
rem   from the caller's %CD%
rem - normalize through pushd + %CD% (NOT a plain `cd`): `cd D:\dir` executed
rem   while the current drive is C: only changes D:'s current directory and
rem   leaves %CD% on C:, so the old code stored e.g. C:\Windows\System32 and
rem   then wrongly reported the Boost/OpenSSL/ragel artifacts as missing and
rem   lost the directory of prepare_windows_deps.ps1
rem - a UNC script path (\\server\share\...) is kept verbatim: pushd maps a
rem   temporary drive letter which popd releases again
rem - all dirs are stored WITHOUT a trailing backslash (every use appends
rem   `\...` explicitly), which also avoids the trailing-backslash/quote pitfall
rem   described next to the BOOST_ROOT assignment below
set "script_dir=%~dp0"
set "script_drive=%~d0"
if not "%script_drive:~1,1%"==":" goto paths_verbatim
pushd "%script_dir%"
set "script_dir=%CD%"
popd
pushd "%script_dir%\.."
set "src_dir=%CD%"
popd
goto paths_ready
:paths_verbatim
set "script_dir=%script_dir:~0,-1%"
set "src_dir=%script_dir%\.."
:paths_ready
set "script_drive="
set "build_dir=%src_dir%\build\windows-debug"
set "output_dir=%src_dir%\build\windows-debug-output"
if not exist "%build_dir%" mkdir "%build_dir%"
echo [paths] script=%script_dir%
echo [paths] src=%src_dir% build=%build_dir% output=%output_dir%

rem MSVC output english
set VSLANG=1033
rem Disable vcpkg entirely: this project builds all deps from third_party
rem sources, no vcpkg is needed.
rem 1) VCPKG_ROOT= disables vcpkg CMake toolchain integration.
rem 2) VCPkgLocalAppDataDisabled=1 (non-empty) disables the USER-LEVEL MSBuild
rem    integration installed by `vcpkg integrate install` (files
rem    %LOCALAPPDATA%\vcpkg\vcpkg.user.props / vcpkg.user.targets import
rem    vcpkg.props/targets only when '$(VCPkgLocalAppDataDisabled)' == '').
rem    Without it MSBuild auto-imports vcpkg.targets for every project, which
rem    runs the applocal.ps1 DLL-gathering step and emits the
rem    "Failed to gather app local DLL dependencies" warning.
set VCPKG_ROOT=
set VCPkgLocalAppDataDisabled=1

rem find Ragel (winget links dir kept for fallback)
set "PATH=%PATH%;%localappdata%\Microsoft\WinGet\Links\"

rem ===== Hermetic deps: self-build/fetch Boost/OpenSSL/ragel (not system installs) =====
rem - SKIP: set AGENTXX_SKIP_AUTO_DEPS=1 to disable auto-fetch (then the dirs
rem   below must already exist, else cmake fails with a clear message)
rem - Prebuilt dirs under third_party are reused automatically when present.
rem - OpenSSL is downloaded as slproweb Win64 prebuilt installer (no perl).
rem - The ps1 is pure ASCII; output goes through powershell stdout.
if "%AGENTXX_SKIP_AUTO_DEPS%"=="1" goto deps_skip_all
rem ===== compile environment pre-check (fail fast with clear message) =====
rem check: cmake, git (boost source fetch), Visual Studio (vswhere or dir scan)
where cmake >nul 2>&1
if errorlevel 1 (
    echo [env] cmake not found, install from https://cmake.org/download/
    exit /b 1
)
where git >nul 2>&1
if errorlevel 1 (
    echo [env] git not found ^(needed to fetch boost sources if missing^)
    exit /b 1
)
set "AGENTXX_VS_FOUND="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "AGENTXX_VS_FOUND=1"
if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" set "AGENTXX_VS_FOUND=1"
rem fallback: scan "<pf>\Microsoft Visual Studio\<ver>\<edition>\Common7\Tools\" for
rem VsDevCmd.bat (VS2022/2026 install under ProgramFiles, VS2019 under (x86)).
if not defined AGENTXX_VS_FOUND (
    for %%R in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
        if exist "%%~R\Microsoft Visual Studio\" (
            for /D %%V in ("%%~R\Microsoft Visual Studio\*") do (
                for /D %%E in ("%%V\*") do (
                    if exist "%%E\Common7\Tools\VsDevCmd.bat" set "AGENTXX_VS_FOUND=1"
                )
            )
        )
    )
)
if not defined AGENTXX_VS_FOUND (
    echo [env] Visual Studio with MSVC not found ^(need VsDevCmd.bat, VS2022+ recommended^)
    exit /b 1
)
set "AGENTXX_VS_FOUND="
set "AGENTXX_DEPS_NEEDED="
if not exist "%src_dir%\third_party\boost-windows-build-debug\include\boost\version.hpp" set "AGENTXX_DEPS_NEEDED=1"
if not exist "%src_dir%\third_party\OpenSSL-windows-build\include\openssl\opensslv.h" set "AGENTXX_DEPS_NEEDED=1"
rem hyperscan needs ragel; if missing let ps1 fetch/build it
if "%AGENTXX_ENABLE_HYPERSCAN%"=="OFF" goto deps_skip_ragel
where ragel >nul 2>&1
if errorlevel 1 set "AGENTXX_DEPS_NEEDED=1"
:deps_skip_ragel
if not defined AGENTXX_DEPS_NEEDED goto deps_done
echo [deps] Boost/OpenSSL/ragel windows artifacts missing, self-building/fetching ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%script_dir%\deps\prepare_windows_deps.ps1" -Mode Debug -SrcDir "%src_dir%\third_party"
if %ERRORLEVEL% neq 0 (
    echo [deps] auto deps failed ^(AGENTXX_SKIP_AUTO_DEPS=1 to skip^)
    exit /b 1
)
:deps_done
:deps_skip_all
rem NOTE: assign WITHOUT a trailing backslash: `set "VAR=...\dir\"` makes the
rem closing quote part of the value (cmd pairs the quotes), corrupting the
rem later -DBOOST_ROOT=... expansion (seen as `debug" -DOPENSSL_ROOT_DIR=...`).
if not defined BOOST_ROOT (
    if exist "%src_dir%\third_party\boost-windows-build-debug\" (
        set "BOOST_ROOT=%src_dir%\third_party\boost-windows-build-debug"
    )
)
if not defined OPENSSL_ROOT_DIR (
    if exist "%src_dir%\third_party\OpenSSL-windows-build\" (
        set "OPENSSL_ROOT_DIR=%src_dir%\third_party\OpenSSL-windows-build"
    )
)

rem mimalloc (OFF by default; set AGENTXX_ENABLE_MIMALLOC=ON to enable):
rem - AGENTXX_MIMALLOC_LINK=SHARED    link mimalloc.dll instead of the static lib
if not defined AGENTXX_ENABLE_MIMALLOC set "AGENTXX_ENABLE_MIMALLOC=OFF"
if not defined AGENTXX_MIMALLOC_LINK set "AGENTXX_MIMALLOC_LINK=SHARED"

cmake -DAGENTXX_BUILD_CLIENT=ON ^
    -DAGENTXX_BUILD_TEST=ON ^
    -DAGENTXX_BUILD_BENCHMARK=OFF ^
    -DAGENTXX_ENABLE_HYPERSCAN=ON ^
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON ^
    -DAGENTXX_ENABLE_MIMALLOC="%AGENTXX_ENABLE_MIMALLOC%" ^
    -DAGENTXX_MIMALLOC_LINK="%AGENTXX_MIMALLOC_LINK%" ^
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

rem parallel build jobs, default 4; set AGENTXX_BUILD_PARALLEL to override
rem e.g. set AGENTXX_BUILD_PARALLEL=12 && windows_debug_build.bat
if "%AGENTXX_BUILD_PARALLEL%"=="" set AGENTXX_BUILD_PARALLEL=4

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
