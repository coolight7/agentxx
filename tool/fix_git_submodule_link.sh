# 根据 .gitmodules 文件，批量执行 git submodule add --force
git config -f .gitmodules --name-only --get-regexp 'submodule\..*\.path' | \
sed 's/^submodule\.\(.*\)\.path$/\1/' | \
while read name; do
    path=$(git config -f .gitmodules --get "submodule.$name.path")
    url=$(git config -f .gitmodules --get "submodule.$name.url")
    
    # 检查该路径是否已经作为子模块被索引（是否有 gitlink）
    if ! git ls-files --stage | grep -q "160000.*$path"; then
        echo "正在修复子模块: $name -> $path"
        # --force 会强制覆盖可能存在的空文件夹或未跟踪文件
        git submodule add --force "$url" "$path"
    else
        echo "子模块 $name 已存在，跳过"
    fi
done
