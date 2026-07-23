#!/bin/bash
# 格式化指定目录下的所有 C/C++ 文件
# 用法: ./format.sh [目录] (默认当前目录)

DIR="${1:-.}"

if [ ! -d "$DIR" ]; then
    echo "错误: 目录不存在: $DIR"
    exit 1
fi

find "$DIR" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" \
    -o -name "*.h" -o -name "*.hpp" -o -name "*.hxx" \) | while read -r file; do
    echo "格式化: $file"
    clang-format -i "$file"
done
