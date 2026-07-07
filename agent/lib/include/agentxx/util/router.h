#pragma once
#include <array>
#include <map>
#include <string>
#include <unordered_map>

/// 路由
/// 程序启动后基本固定，因此 map 不需要处理线程安全问题
template <typename HANLDE_TPYE, size_t HANLDE_NUM> class XXRouter {
protected:
  /// 路由 树节点
  struct RouterTreePort {
  protected:
  public:
    /// 路径
    std::string path;
    /// 对应Http方法的处理函数
    std::array<std::shared_ptr<HANLDE_TPYE>, HANLDE_NUM> handles;
    /// 子节点
    std::map<std::string, RouterTreePort *> child;

    RouterTreePort(std::string_view in_path = "") noexcept : path(in_path) {
      for (int i = handles.size(); i-- > 0;) {
        handles[i] = nullptr;
      }
    }

    /// 返回对应路径节点，不存在则返回nullptr
    RouterTreePort *getChild(const std::string &in_path, std::string &re_path,
                             bool do_add = false) {
      return this->getChild(in_path.c_str(), re_path, do_add);
    }

    /// 获取对应路径[in_path]的节点，并返回路由路径到[re_path]
    ///
    /// - [in_path] 待查找路径
    /// - [re_path] 节点真实路由路径；无对应节点时返回空
    /// - [do_add] 当路径对应节点不存在时，是否添加节点
    ///
    /// - return: 对应路径节点，不存在则返回nullptr
    RouterTreePort *getChild(const char *in_path, std::string &re_path,
                             bool do_add = false) {
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
          // strp = '\0'，是最后一个子节点
          auto it = child.find(in_path);
          if (it != child.end()) {
            // 存在子节点
            re_path = in_path;
            return it->second;
          } else {
            if (do_add) {
              auto treeptr = new RouterTreePort(in_path);
              child[in_path] = treeptr;
              re_path = in_path;
              return treeptr;
            } else {
              it = child.find("*");
              if (it != child.end()) {
                re_path = "*";
                return it->second;
              } else {
                re_path.clear();
                return nullptr;
              }
            }
          }
        }
      }

      // *strp = '/'，不是最后一个子节点
      std::string str{in_path, strptr};
      ++nextptr;
      auto it = child.find(str);
      if (it != child.end()) {
        // 如果存在子节点
        auto re_ptr = it->second->getChild(nextptr, re_path, do_add);
        if (re_ptr != nullptr) {
          // 有找到匹配的路径
          re_path = str + "/" + re_path;
        }
        return re_ptr;
      } else {
        if (do_add) {
          // 如果需要新建子节点
          auto treeptr = new RouterTreePort(str);
          child[str] = treeptr;
          auto re_ptr = treeptr->getChild(nextptr, re_path, do_add);
          if (re_ptr != nullptr) {
            re_path = str + "/" + re_path;
          }
          return re_ptr;
        } else {
          // 查找是否有通配符
          it = child.find("*");
          if (it != child.end()) {
            re_path = "*";
            return it->second;
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
      if (in_index >= 0 && in_index < handles.size()) {
        handles[in_index] = in_fun;
        return true;
      } else {
        return false;
      }
    }

    /// 用数组下标获取函数
    ///   - 下表越界时返回nullptr
    ///   - 指定下标的函数未定义时返回nullptr
    std::shared_ptr<HANLDE_TPYE> getHandle(int in_index) const {
      if (in_index >= 0 && in_index < handles.size()) {
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

    ~RouterTreePort() { this->clearChild(); }
  };

  struct _RouterCacheValue_s {
    /// 路由路径
    std::string path;

    /// 对应节点
    RouterTreePort *treeptr = nullptr;
  };

  inline static thread_local std::unordered_map<std::string,
                                                _RouterCacheValue_s>
      cacheMap{};

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
  ///
  /// - return: 返回查找结果节点
  RouterTreePort *getTreepNocache(std::string_view in_path,
                                  std::string &re_path) {
    const char *strp = in_path.data();
    while (*strp == '/') {
      ++strp;
    }
    auto treep = this->routerTree.getChild(strp, re_path);
    if (false == re_path.empty()) {
      re_path = "/" + re_path;
    }
    return treep;
  }

public:
  XXRouter() noexcept : routerTree("/") {}

  /// 添加路由
  /// - 允许使用通配符 *
  /// - 允许同时设置多个类型枚举
  /// - 路径中连续的 / 将被视为仅一个 /
  /// - 即 /a//b///c 等同于 /a/b/c
  bool add(std::string_view in_path, int in_index,
           std::shared_ptr<HANLDE_TPYE> in_fun) {
    this->cacheMap.clear();
    const char *strp = in_path.data();
    while ('/' == *strp) {
      ++strp;
    }
    std::string re_path{};
    auto treep = this->routerTree.getChild(strp, re_path, true);
    return treep->setHandle(in_fun, in_index);
  }

  /// 判断是否存在路由位置，使用缓存
  /// - 有，且对应方法有处理函数：返回其处理函数指针，并赋值re_path
  /// - 有，但对应方法没有处理函数：返回nullptr，并赋值re_path
  /// - 没有:	返回nullptr，re_path置空
  std::shared_ptr<HANLDE_TPYE> get(const std::string &in_path, int in_index,
                                   std::string &re_path) {
    auto refind = this->cacheMap.find(in_path);
    XXRouter::RouterTreePort *treeptr = nullptr;
    if (refind != this->cacheMap.end()) {
      treeptr = refind->second.treeptr;
      re_path = refind->second.path;
    } else {
      treeptr = this->getTreepNocache(in_path, re_path);
    }
    if (treeptr != nullptr) {
      auto handles = treeptr->getHandle(in_index);
      if (handles) {
        this->cacheMap.insert({in_path, {re_path, treeptr}});
      }
      return handles;
    }
    return nullptr;
  }

  /// 判断是否存在路由位置，不使用缓存
  /// - 有，且对应方法有处理函数：返回其处理函数指针，并赋值[re_path]
  /// - 有，但对应方法没有处理函数：返回[nullptr]，并赋值[re_path]
  /// - 没有:	返回[nullptr]，[re_path]置空
  std::shared_ptr<HANLDE_TPYE> getNocache(const std::string &in_path,
                                          int in_index, std::string &re_path) {
    auto treep = this->getTreepNocache(in_path, re_path);
    if (treep != nullptr) {
      return treep->getHandle(in_index);
    }
    return nullptr;
  }

  /// 移除指定的路由位置，并返回其处理函数
  /// - [in_index] 一次调用仅支持移除单一请求类型对应的处理函数
  std::shared_ptr<HANLDE_TPYE> remove(std::string_view in_path, int in_index) {
    const char *strp = in_path.data();
    while (*strp == '/') {
      ++strp;
    }
    std::string re_path{};
    auto treep = this->routerTree.getChild(strp, re_path, false);
    if (treep) {
      this->cacheMap.clear();
      auto handles = treep->getHandle(in_index);
      treep->setHandle(nullptr, in_index);
      return handles;
    }
  }

  /// 清理缓存
  void clearCache() { this->cacheMap.clear(); }

  /// 清空路由
  void clear() { this->routerTree.clearChild(); }
};
