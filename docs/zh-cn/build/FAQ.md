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

## 新增依赖库时 cmake 使用 ExternalProject 还是 FetchContent
- 建议统一`ExternalProject`, 尽管`FetchContent`也可以用，但他们的执行时机不一样，会导致被`FetchContent`(configure时执行)导入的库内用`find_package`找不到`ExternalProject`(build时执行)导入的库
- 统一使用`FetchContent`我们尝试过，最开始由于`FetchContent`可以自动继承主项目的变量，觉得对跨平台和交叉编译方便，所以统一用它，但是它不支持非cmake项目，控制 install、build 细节也不足，因此最终统一使用 `ExternalProject`