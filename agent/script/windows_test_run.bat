@echo off
rem set utf8
chcp 65001 > NUL

set "script_dir=%~dp0"
set "src_dir=%script_dir%\..\"
set "build_dir=%script_dir%\..\build\windows-debug"

call %script_dir%\windows_debug_build.bat
if %ERRORLEVEL% neq 0 (
    echo cmake build failed!
    exit /b 1
)

%build_dir%\exec\agentxx_test.exe
