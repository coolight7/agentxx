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

rem 测试依赖 cwd 下的 plugins\ 等资源 (与可执行文件同目录):
rem 无论从哪个目录运行脚本, 都切到可执行文件目录再运行
cd /d "%build_dir%\exec"
agentxx_test.exe
