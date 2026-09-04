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

rem find Ragel
set "PATH=%PATH%;%localappdata%\Microsoft\WinGet\Links\"

set BOOST_ROOT="%src_dir%/third_party/boost-windows-build-release/"
set OPENSSL_ROOT_DIR="%src_dir%/third_party/OpenSSL-windows-build/"

cmake -DAGENTXX_BUILD_CLIENT=ON ^
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
rem 2) Scan known VS install locations, last match wins (versions sort ascending)
rem NOTE: "C:\Program Files (x86)" literal contains parens which would break
rem cmd.exe block parsing (the ) in (x86) closes the if/for block early),
rem so it is kept in AGENTXX_PF86 set BEFORE the block and referenced as
rem "%AGENTXX_PF86%\..." (source line has no literal parens).
set "AGENTXX_PF86=%ProgramFiles(x86)%"
if not defined AGENTXX_CRT_SRC (
  for %%V in (Community Professional Enterprise BuildTools) do (
    for %%E in (18 17 14) do (
      for /D %%R in ("C:\Program Files\Microsoft Visual Studio\%%E\%%V\VC\Redist\MSVC\*") do (
        for /D %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
      )
      for /D %%R in ("%AGENTXX_PF86%\Microsoft Visual Studio\%%E\%%V\VC\Redist\MSVC\*") do (
        for /D %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
      )
    )
  )
)
rem 3) vswhere fallback (covers custom install paths)
rem NOTE: AGENTXX_VSWHERE is set BEFORE the block: %ProgramFiles(x86)%
rem contains parens which would break parsing inside (...) blocks, and a
rem variable set+used inside the same block would expand to empty.
set "AGENTXX_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined AGENTXX_CRT_SRC (
  if exist "%AGENTXX_VSWHERE%" (
    for /F "usebackq delims=" %%I in (`"%AGENTXX_VSWHERE%" -latest -property installationPath 2^>NUL`) do (
      for /D %%R in ("%%I\VC\Redist\MSVC\*") do (
        for /D %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "AGENTXX_CRT_SRC=%%C"
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
set "AGENTXX_PF86="
set "AGENTXX_VSWHERE="
:bundle_runtime_done