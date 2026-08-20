#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-debug"

$script_dir/linux_debug_build.sh

if [[ $? -ne 0 ]]; then
    exit $?
fi

# 测试依赖 cwd 下的 plugins/ 等资源 (与可执行文件同目录):
# 无论从哪个目录运行脚本, 都切到可执行文件目录再运行
cd "$build_dir/exec" || exit 1
LD_LIBRARY_PATH=. ./agentxx_test
