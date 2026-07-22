# 常见问题

## AI 修改代码后可能运行仍无效果
- 有些 AI 编译时会直接指定 target 执行 make，但不 install，所以运行时用的可能还是老的可执行文件，可以手动执行 `agent/script/` 内的编译脚本后运行尝试

## Boost 编译静态库启用 fPIC:
- 添加 `cflags=-fPIC cxxflags=-fPIC` 即可
```sh
cd boost/

# 清理构建缓存
./b2 --clean-all
rm -rf bin.v2

./b2 cflags=-fPIC cxxflags=-fPIC {原本的参数}
```