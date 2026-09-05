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
set "build_dir=%script_dir%\..\build\windows-release"

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
rem Disable vcpkg entirely: this project builds all deps from third_party
rem sources, no vcpkg is needed.
rem 1) VCPKG_ROOT= disables vcpkg CMake toolchain integration.
rem 2) VCPkgLocalAppDataDisabled=1 (non-empty) disables the USER-LEVEL MSBuild
rem    integration installed by `vcpkg integrate install` (files
rem    %LOCALAPPDATA%\vcpkg\vcpkg.user.props / vcpkg.user.targets import
rem    vcpkg.props/targets only when '$(VcpkgLocalAppDataDisabled)' == '').
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
if not exist "%src_dir%\third_party\boost-windows-build-release\include\boost\version.hpp" set "AGENTXX_DEPS_NEEDED=1"
if not exist "%src_dir%\third_party\OpenSSL-windows-build\include\openssl\opensslv.h" set "AGENTXX_DEPS_NEEDED=1"
rem hyperscan needs ragel; if missing let ps1 fetch/build it
if "%AGENTXX_ENABLE_HYPERSCAN%"=="OFF" goto deps_skip_ragel
where ragel >nul 2>&1
if errorlevel 1 set "AGENTXX_DEPS_NEEDED=1"
:deps_skip_ragel
if not defined AGENTXX_DEPS_NEEDED goto deps_done
echo [deps] Boost/OpenSSL/ragel windows artifacts missing, self-building/fetching ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%script_dir%deps\prepare_windows_deps.ps1" -Mode Release -SrcDir "%src_dir%third_party"
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
    if exist "%src_dir%\third_party\boost-windows-build-release\" (
        set "BOOST_ROOT=%src_dir%\third_party\boost-windows-build-release"
    )
)
if not defined OPENSSL_ROOT_DIR (
    if exist "%src_dir%\third_party\OpenSSL-windows-build\" (
        set "OPENSSL_ROOT_DIR=%src_dir%\third_party\OpenSSL-windows-build"
    )
)

rem ===== Read unified version (agent/VERSION) =====
if not defined AGENTXX_VERSION (
    if exist "%src_dir%\VERSION" (
        set /p AGENTXX_VERSION=<"%src_dir%\VERSION"
    ) else if exist "%src_dir%\..\VERSION" (
        set /p AGENTXX_VERSION=<"%src_dir%\..\VERSION"
    )
)
if not defined AGENTXX_VERSION set "AGENTXX_VERSION=0.1.0"
echo [version] Agentxx version: %AGENTXX_VERSION%

cmake -DAGENTXX_VERSION="%AGENTXX_VERSION%" ^
    -DAGENTXX_BUILD_CLIENT=ON ^
    -DAGENTXX_BUILD_TEST=ON ^
    -DAGENTXX_BUILD_BENCHMARK=ON ^
    -DAGENTXX_ENABLE_HYPERSCAN=ON ^
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON ^
    -DBOOST_ROOT="%BOOST_ROOT%" ^
    -DOPENSSL_ROOT_DIR="%OPENSSL_ROOT_DIR%" ^
    -DXX_IS_RELEASE_D=1 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CONFIGURATION_TYPES=Release ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 ^
    -A x64 -B %build_dir% -S %src_dir%

if %ERRORLEVEL% neq 0 (
    echo cmake config failed!
    exit /b 1
)

cmake --build "%build_dir%" --config Release --parallel 6
if %ERRORLEVEL% neq 0 (
    echo cmake build failed!
    exit /b 1
)

cmake --install "%build_dir%" --config Release
if %ERRORLEVEL% neq 0 (
    echo cmake install failed!
    exit /b 1
)

rem ===== Bundle MSVC runtime DLLs to exec (release redist) =====
rem - client CMakeLists install TARGETS agentxx_cli DESTINATION
rem   AGENTXX_EXEC_INSTALL_PREFIX (exec/); exe uses /MD dynamic CRT,
rem   needs VC Redist alongside for clean Win10+ machines without VS.
rem - UCRT (ucrtbase/api-ms-win-*) is inbox on Win10+, do NOT bundle.
rem - Skip when AGENTXX_SKIP_BUNDLE_RUNTIME=1
if "%AGENTXX_SKIP_BUNDLE_RUNTIME%"=="1" goto bundle_runtime_done
set "AGENTXX_CRT_SRC="
rem 1) VCToolsRedistDir (set when building inside VsDevCmd)
if defined VCToolsRedistDir (
  for /D %%D in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%D"
)
rem 2) vswhere (covers custom install paths / VS2026 layouts)
rem NOTE: path variables are set BEFORE the block: %ProgramFiles(x86)%
rem contains parens which would break parsing inside (...) blocks, and a
rem variable set+used inside the same block would expand to empty.
set "AGENTXX_VSWHERE1=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "AGENTXX_VSWHERE2=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined AGENTXX_CRT_SRC (
  if exist "%AGENTXX_VSWHERE1%" (
    for /F "usebackq delims=" %%I in (`"%AGENTXX_VSWHERE1%" -latest -property installationPath 2^>NUL`) do (
      for /D %%R in ("%%I\VC\Redist\MSVC\*") do (
        for /D %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
      )
    )
  )
)
if not defined AGENTXX_CRT_SRC (
  if exist "%AGENTXX_VSWHERE2%" (
    for /F "usebackq delims=" %%I in (`"%AGENTXX_VSWHERE2%" -latest -property installationPath 2^>NUL`) do (
      for /D %%R in ("%%I\VC\Redist\MSVC\*") do (
        for /D %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
      )
    )
  )
)
rem 3) Scan both ProgramFiles roots for any
rem     "Microsoft Visual Studio\<ver>\<edition>\VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT"
rem     (no hard-coded version/edition), last match wins.
rem NOTE: AGENTXX_PF / AGENTXX_PF86 are set BEFORE the block (see note above).
set "AGENTXX_PF=%ProgramFiles%"
set "AGENTXX_PF86=%ProgramFiles(x86)%"
if not defined AGENTXX_CRT_SRC (
  for %%R in ("%AGENTXX_PF%\Microsoft Visual Studio" "%AGENTXX_PF86%\Microsoft Visual Studio") do (
    if exist "%%~R\" (
      for /D %%V in ("%%~R\*") do (
        for /D %%E in ("%%V\*") do (
          for /D %%M in ("%%E\VC\Redist\MSVC\*") do (
            for /D %%C in ("%%M\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
          )
        )
      )
    )
  )
)
if not defined AGENTXX_CRT_SRC (
  echo WARNING: MSVC CRT dir not found, skip bundling runtime DLLs
  goto bundle_runtime_done
)
echo [runtime] bundle MSVC CRT from "%AGENTXX_CRT_SRC%" to "%build_dir%\exec\"
copy /Y "%AGENTXX_CRT_SRC%\msvcp140*.dll" "%build_dir%\exec\" >NUL 2>&1
copy /Y "%AGENTXX_CRT_SRC%\vcruntime140*.dll" "%build_dir%\exec\" >NUL 2>&1
copy /Y "%AGENTXX_CRT_SRC%\concrt140.dll" "%build_dir%\exec\" >NUL 2>&1
if not exist "%build_dir%\exec\vcruntime140.dll" echo WARNING: vcruntime140.dll not bundled (ignored)
if not exist "%build_dir%\exec\msvcp140.dll" echo WARNING: msvcp140.dll not bundled (ignored)
set "AGENTXX_CRT_SRC="
set "AGENTXX_PF="
set "AGENTXX_PF86="
set "AGENTXX_VSWHERE1="
set "AGENTXX_VSWHERE2="
:bundle_runtime_done

rem ===== Archive release package (AGENTXX_PACKAGE_RELEASE=1 auto pack) =====
if "%AGENTXX_PACKAGE_RELEASE%"=="1" (
    if not exist "%src_dir%\build\dist\" mkdir "%src_dir%\build\dist\"
    echo [package] Packing release archive -> "%src_dir%\build\dist\agentxx-v%AGENTXX_VERSION%-windows-x86_64.zip"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%build_dir%\exec\*' -DestinationPath '%src_dir%\build\dist\agentxx-v%AGENTXX_VERSION%-windows-x86_64.zip' -Force"
    echo [package] Done!
)