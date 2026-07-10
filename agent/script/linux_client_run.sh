#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-debug"

$script_dir/linux_debug_build.sh

if [[ $? -ne 0 ]]; then
    exit $?
fi

LD_LIBRARY_PATH=$build_dir/exec $build_dir/exec/agentxx_cli