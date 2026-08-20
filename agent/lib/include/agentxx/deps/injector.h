#pragma once

#include <any>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace agentxx {
namespace deps {

/// 依赖注入容器（DI Container）
/// - 支持按类型和名称注册/解析依赖项
/// - 提供工厂方法和单例管理
/// - 用于解耦组件间的直接耦合，提高可测试性
class DependencyContainer {
public:

    using FactoryFn = std::function<std::any()>;

    DependencyContainer()  = default;
    ~DependencyContainer() = default;

    /// 禁用拷贝构造和赋值操作
    DependencyContainer(const DependencyContainer&)            = delete;
    DependencyContainer& operator=(const DependencyContainer&) = delete;

    // 允许移动
    DependencyContainer(DependencyContainer&&)            = default;
    DependencyContainer& operator=(DependencyContainer&&) = default;

    /// 注册一个依赖项（单例模式，默认延迟初始化）
    /// T 依赖项类型
    /// - [factory] 工厂函数，返回 std::any 包装的 T 实例
    /// - [registerAsEmptyName] 是否以空名注册（作为默认实例）
    template<typename T>
    void registerSingleton(FactoryFn factory, bool registerAsEmptyName = true) {
        auto typeId            = typeid(T).name();
        registrations_[typeId] = std::move(factory);
        if (registerAsEmptyName) {
            defaultRegistrations_.erase(typeId); // 清除默认的有名字注册
        }
    }

    /// 注册一个依赖项（有名称）
    /// T 依赖项类型
    /// - [name] 注册名称（用于同名不同类型）
    /// - [factory] 工厂函数
    template<typename T>
    void registerNamedSingleton(std::string name, FactoryFn factory) {
        auto typeId = typeid(T).name();

        // 构建复合 key: "TypeId::Name"
        std::string compositeKey = typeId;
        if (!name.empty()) {
            compositeKey += fmt::format("::{}", name);
        }

        registrations_[compositeKey]  = std::move(factory);
        defaultRegistrations_[typeId] = compositeKey;
    }

    /// 解析依赖项（按类型和默认名称）
    /// T 要解析的类型
    /// - `return` T 实例的智能指针
    /// - `throw` std::runtime_error 如果找不到注册项或类型转换失败
    template<typename T>
    std::shared_ptr<T> resolve() {
        auto        typeId = typeid(T).name();
        std::string key    = typeId;

        // 如果有默认注册，使用默认的
        auto it = defaultRegistrations_.find(typeId);
        if (it != defaultRegistrations_.end()) {
            key = it->second;
        }

        return resolveTyped<T>(key);
    }

    /// 解析有名称的依赖项
    /// T 要解析的类型
    /// - [name] 注册名称
    /// - `return` T 实例的智能指针
    template<typename T>
    std::shared_ptr<T> resolveNamed(std::string_view name) {
        auto        typeId       = typeid(T).name();
        std::string compositeKey = fmt::format("{}::{}", typeId, name);
        return resolveTyped<T>(compositeKey);
    }

    /// 检查某个类型是否存在于容器中
    /// T 要检查的类型
    /// - `return` 存在返回 true
    template<typename T>
    bool hasType() const {
        auto typeId = typeid(T).name();
        auto it     = registrations_.find(typeId);
        return it != registrations_.end()
               || defaultRegistrations_.find(typeId) != defaultRegistrations_.end();
    }

    /// 检查某个特定注册的依赖项是否存在
    /// T 要检查的类型
    /// - [name] 注册名称
    /// - `return` 存在返回 true
    template<typename T>
    bool hasNamed(std::string_view name) const {
        auto        typeId       = typeid(T).name();
        std::string compositeKey = fmt::format("{}::{}", typeId, name);
        return registrations_.find(compositeKey) != registrations_.end();
    }

private:

    /// 解析指定 key 的类型
    template<typename T>
    std::shared_ptr<T> resolveTyped(std::string_view key) {
        auto it = registrations_.find(std::string(key));
        if (it == registrations_.end()) {
            throw std::runtime_error(fmt::format("Dependency not registered: {}", key));
        }

        auto instance = it->second();
        try {
            return std::dynamic_pointer_cast<T>(std::any_cast<std::shared_ptr<void>>(instance));
        } catch (...) {
            throw std::runtime_error(
                fmt::format("Cannot cast dependency to requested type: {}", key)
            );
        }
    }

    /// 存储所有注册：key="TypeId"或"TypeId::Name", value=Factory 返回 std::any
    std::unordered_map<std::string, FactoryFn> registrations_;

    /// 每个类型的默认注册名（如果没有显式注册）
    std::unordered_map<std::string, std::string> defaultRegistrations_;
};

// ========================================================================
// 用于快速注册常用类型
// ========================================================================

#define DEPS_REGISTER_SINGLETON(container, Type)          \
    container.register_singleton<Type>([]() -> std::any { \
        return std::make_shared<Type>();                  \
    })

#define DEPS_REGISTER_NAMED_SINGLETON(container, Type, name)          \
    container.register_named_singleton<Type>(name, []() -> std::any { \
        return std::make_shared<Type>();                              \
    })

#define DEPS_RESOLVE(container, Type) container.resolve<Type>()

#define DEPS_RESOLVE_NAMED(container, Type, name) container.resolveNamed<Type>(name)

} // namespace deps
} // namespace agentxx
