#pragma once
#include "agentxx/util/lru_cache.h"
#include <array>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <string>

/// 路由
/// 程序启动后基本固定，因此 map 不需要处理线程安全问题
template<typename HANLDE_TPYE, size_t HANLDE_NUM>
class XXRouter {
protected:

    /// 路由 树节点
    struct RouterTreePort {
    protected:
    public:

        /// 路径
        std::string path;
        /// 父节点 (最长前缀匹配时用于沿父链回退查找处理函数); 根节点为 nullptr
        RouterTreePort* parent = nullptr;
        /// 对应Http方法的处理函数
        std::array<std::shared_ptr<HANLDE_TPYE>, HANLDE_NUM> handles;
        /// 子节点
        std::map<std::string, RouterTreePort*> child;

        RouterTreePort(std::string_view in_path = "") noexcept :
            path(in_path) {
            for (size_t i = 0; i < handles.size(); ++i) {
                handles[i] = nullptr;
            }
        }

        /// 返回对应路径节点，不存在则返回nullptr
        RouterTreePort*
            getChild(const std::string& in_path, std::string& re_path, bool do_add = false) {
            return this->getChild(in_path.c_str(), re_path, do_add);
        }

        /// 获取对应路径[in_path]的节点，并返回路由路径到[re_path]
        ///
        /// - [in_path] 待查找路径（相对当前节点的剩余路径）
        /// - [re_path] 累积的节点真实路由路径（不含开头 '/'）；未匹配到时清空
        /// - [do_add] 当路径对应节点不存在时，是否添加节点
        /// - [loose] 不创建节点且精确子节点不存在时，是否回退到当前节点（最长前缀匹配）
        ///
        /// - return: 对应路径节点，不存在则返回nullptr
        RouterTreePort* getChild(
            const char*  in_path,
            std::string& re_path,
            bool         do_add = false,
            bool         loose  = false
        ) {
            const char *strptr = in_path, *nextptr = in_path;
            for (;;) {
                if (*nextptr == '/') {
                    if (*(nextptr + 1) == '/') {
                        ++nextptr;
                    } else {
                        break;
                    }
                } else if (*nextptr != '\0') {
                    ++strptr;
                    ++nextptr;
                } else {
                    // *nextptr == '\0'，in_path 是最后一个路径段
                    if (in_path == nextptr) {
                        // 空路径段（查询路径以 '/' 结尾），视为当前节点自身
                        if (!re_path.empty() && re_path.back() == '/') {
                            re_path.pop_back();
                        }
                        return this;
                    }
                    auto it = child.find(in_path);
                    if (it != child.end()) {
                        // 存在子节点
                        re_path += in_path;
                        return it->second;
                    } else {
                        if (do_add) {
                            auto treeptr     = new RouterTreePort(in_path);
                            treeptr->parent  = this;
                            child[in_path]   = treeptr;
                            re_path         += in_path;
                            return treeptr;
                        } else {
                            it = child.find("*");
                            if (it != child.end()) {
                                re_path += "*";
                                return it->second;
                            } else if (loose) {
                                // 最长前缀匹配: 无更深的精确子节点时回退到当前节点
                                if (!re_path.empty() && re_path.back() == '/') {
                                    re_path.pop_back();
                                }
                                return this;
                            } else {
                                re_path.clear();
                                return nullptr;
                            }
                        }
                    }
                }
            }

            // *nextptr == '/'，in_path 含有多个路径段
            std::string str{in_path, strptr};
            if (str.empty()) {
                // 连续结尾斜杠 (如 "/a/b//")，视为当前节点自身
                if (!re_path.empty() && re_path.back() == '/') {
                    re_path.pop_back();
                }
                return this;
            }
            ++nextptr;
            auto it = child.find(str);
            if (it != child.end()) {
                // 如果存在子节点
                const size_t re_size  = re_path.size();
                re_path              += str;
                re_path              += '/';
                auto re_ptr           = it->second->getChild(nextptr, re_path, do_add, loose);
                if (re_ptr != nullptr) {
                    // 有找到匹配的路径
                    return re_ptr;
                }
                // 未匹配，回退累积的路径
                re_path.resize(re_size);
                return nullptr;
            } else {
                if (do_add) {
                    // 如果需要新建子节点
                    auto treeptr          = new RouterTreePort(str);
                    treeptr->parent       = this;
                    child[str]            = treeptr;
                    const size_t re_size  = re_path.size();
                    re_path              += str;
                    re_path              += '/';
                    auto re_ptr           = treeptr->getChild(nextptr, re_path, do_add, loose);
                    if (re_ptr != nullptr) {
                        return re_ptr;
                    }
                    re_path.resize(re_size);
                    return nullptr;
                } else {
                    // 查找是否有通配符
                    it = child.find("*");
                    if (it != child.end()) {
                        re_path += "*";
                        return it->second;
                    } else if (loose) {
                        // 最长前缀匹配: 回退到当前节点，去掉刚累积的分隔符
                        if (!re_path.empty() && re_path.back() == '/') {
                            re_path.pop_back();
                        }
                        return this;
                    } else {
                        // 无查找结果，清空re_path并返回nullptr
                        re_path.clear();
                        return nullptr;
                    }
                }
            }
        }

        /// 使用类型下标设置处理函数
        /// - [in_index] 仅支持一个类型下标
        bool setHandle(std::shared_ptr<HANLDE_TPYE> in_fun, int in_index) {
            if (in_index >= 0 && static_cast<size_t>(in_index) < handles.size()) {
                handles[in_index] = in_fun;
                return true;
            } else {
                return false;
            }
        }

        /// 用数组下标获取函数
        ///   - 下表越界时返回nullptr
        ///   - 指定下标的函数未定义时返回nullptr
        [[nodiscard]] std::shared_ptr<HANLDE_TPYE> getHandle(int in_index) const {
            if (in_index >= 0 && static_cast<size_t>(in_index) < handles.size()) {
                return handles[in_index];
            } else {
                return nullptr;
            }
        }

        /// 清空子节点
        void clearChild() {
            for (auto it = child.begin(); it != child.end();) {
                // 调用子节点的清理
                it->second->clearChild();
                // 释放子节点
                delete (it->second);
                child.erase(it);
                // 重置it的指向
                it = child.begin();
            }
        }

        ~RouterTreePort() {
            this->clearChild();
        }
    };

    struct _RouterCacheValue_s {
        /// 路由路径
        std::string path;

        /// 对应节点
        RouterTreePort* treeptr = nullptr;
    };

    /// 路由查找缓存容量
    static constexpr size_t routerCacheCapacity = 1024;
    /// 路由查找 LRU 缓存 (函数内 thread_local 避免 inline 变量在 MinGW+libc++ 下的重复符号)
    static agentxx::util::LruCache<std::string, _RouterCacheValue_s>& getCacheMap() {
        thread_local agentxx::util::LruCache<std::string, _RouterCacheValue_s> instance{
            routerCacheCapacity
        };
        return instance;
    }

    // 路由字典树
    RouterTreePort routerTree;

    /// 哈希缓存
    /// 如果使用缓存接口获取路由位置，获取成功将留下缓存
    /// 下次使用相同的path获取时直接提取缓存
    /// 添加或删除路由位置将清空缓存重新生成

    /// 不经过缓存，直接搜索对应路径[in_path]、对应Http方法[in_index]的节点
    ///
    /// - [in_path] 待查找路径
    /// - [re_path] 返回该节点的真实路由路径；节点不存在时返回空
    /// - [loose] 精确子节点不存在时回退到最深的已注册父节点（最长前缀匹配）
    ///
    /// - return: 返回查找结果节点
    RouterTreePort*
        getTreepNocache(std::string_view in_path, std::string& re_path, bool loose = false) {
        // 路径自顶向下累积，必须先清空（调用方可能复用 re_path 字符串）
        re_path.clear();
        const char* strp = in_path.data();
        while (*strp == '/') {
            ++strp;
        }
        auto treep = this->routerTree.getChild(strp, re_path, false, loose);
        if (false == re_path.empty()) {
            re_path = fmt::format("/{}", re_path);
        }
        return treep;
    }

public:

    XXRouter() noexcept :
        routerTree("/") {}

    /// 析构时清空共享缓存:
    /// cacheMap 为同类所有实例共享的 thread_local 缓存, 其中保存的是本路由树
    /// 的原始节点指针; 树销毁后必须清空, 否则后续其他路由实例 (如按模式重建的
    /// 权限中间件) 可能命中悬空指针导致 use-after-free
    ~XXRouter() {
        getCacheMap().clear();
    }

    /// 添加路由
    /// - 允许使用通配符 *
    /// - 允许同时设置多个类型枚举
    /// - 路径中连续的 / 将被视为仅一个 /，结尾的 / 将被忽略（文件夹路径与文件路径等价注册）
    /// - 即 /a//b///c 等同于 /a/b/c；/a/b/ 等同于 /a/b
    bool add(std::string_view in_path, int in_index, std::shared_ptr<HANLDE_TPYE> in_fun) {
        getCacheMap().clear();
        const char* strp = in_path.data();
        while ('/' == *strp) {
            ++strp;
        }
        std::string re_path{};
        auto        treep = this->routerTree.getChild(strp, re_path, true);
        return treep->setHandle(in_fun, in_index);
    }

    /// 判断是否存在路由位置，使用缓存
    /// - 有，且对应方法有处理函数：返回其处理函数指针，并赋值re_path
    /// - 有，但对应方法没有处理函数：返回nullptr，并赋值re_path
    /// - 没有:	返回nullptr，re_path置空
    ///
    /// - [prefix_fallback] 最长前缀匹配:
    ///   精确路径节点不存在时回退到最深的已注册父节点（* 通配符优先于回退）;
    ///   若该节点没有对应 [in_index] 的处理函数，继续沿父链向上查找;
    ///   [re_path] 始终为最深匹配节点的真实路径。
    ///   该模式命中父链回退的结果不写入缓存。
    std::shared_ptr<HANLDE_TPYE>
        get(const std::string& in_path,
            int                in_index,
            std::string&       re_path,
            bool               prefix_fallback = false) {
        auto                      cached  = getCacheMap().get(in_path);
        XXRouter::RouterTreePort* treeptr = nullptr;
        if (cached.has_value()) {
            treeptr = cached->treeptr;
            re_path = cached->path;
        } else {
            treeptr = this->getTreepNocache(in_path, re_path, prefix_fallback);
        }
        if (treeptr != nullptr) {
            auto handles = treeptr->getHandle(in_index);
            if (handles) {
                getCacheMap().put(in_path, {re_path, treeptr});
            } else if (prefix_fallback) {
                // 节点自身无对应处理函数时，沿父链向上回退查找
                // 父链回退命中的结果不写缓存（缓存条目需保证节点自身持有处理函数）
                for (auto parent = treeptr->parent; parent != nullptr && handles == nullptr;
                     parent      = parent->parent) {
                    handles = parent->getHandle(in_index);
                }
            }
            return handles;
        }
        return nullptr;
    }

    /// 判断是否存在路由位置，不使用缓存
    /// - 有，且对应方法有处理函数：返回其处理函数指针，并赋值[re_path]
    /// - 有，但对应方法没有处理函数：返回[nullptr]，并赋值[re_path]
    /// - 没有:	返回[nullptr]，[re_path]置空
    ///
    /// - [prefix_fallback] 最长前缀匹配，语义同 [get]
    std::shared_ptr<HANLDE_TPYE> getNocache(
        const std::string& in_path,
        int                in_index,
        std::string&       re_path,
        bool               prefix_fallback = false
    ) {
        auto treep = this->getTreepNocache(in_path, re_path, prefix_fallback);
        if (treep != nullptr) {
            auto handles = treep->getHandle(in_index);
            if (handles == nullptr && prefix_fallback) {
                // 节点自身无对应处理函数时，沿父链向上回退查找
                for (auto parent = treep->parent; parent != nullptr && handles == nullptr;
                     parent      = parent->parent) {
                    handles = parent->getHandle(in_index);
                }
            }
            return handles;
        }
        return nullptr;
    }

    /// 移除指定的路由位置，并返回其处理函数
    /// - [in_index] 一次调用仅支持移除单一请求类型对应的处理函数
    std::shared_ptr<HANLDE_TPYE> remove(std::string_view in_path, int in_index) {
        const char* strp = in_path.data();
        while (*strp == '/') {
            ++strp;
        }
        std::string re_path{};
        auto        treep = this->routerTree.getChild(strp, re_path, false);
        if (treep) {
            getCacheMap().clear();
            auto handles = treep->getHandle(in_index);
            treep->setHandle(nullptr, in_index);
            return handles;
        }
        return nullptr;
    }

    /// 清理缓存
    void clearCache() {
        getCacheMap().clear();
    }

    /// 清空路由
    void clear() {
        this->routerTree.clearChild();
        // 节点已释放, 清空缓存
        getCacheMap().clear();
    }
};
