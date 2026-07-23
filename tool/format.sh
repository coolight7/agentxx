#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)

$script_dir/format-file.sh $script_dir/../agent/client/
$script_dir/format-file.sh $script_dir/../agent/lib/
$script_dir/format-file.sh $script_dir/../agent/test/