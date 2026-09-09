// ModuleFactory.hpp

#pragma once
#include <memory>
#include <type_traits>
#include "IModule.h"

// struct ModuleFactory {
//   template <typename T, typename... Args>
//   static std::unique_ptr<IModule> create(Args&&... args) {
//     static_assert(std::is_base_of<IModule, T>::value, "T must derive from IModule");
//     auto mod = std::make_unique<T>(std::forward<Args>(args)...);
//     if (!mod->validate()) throw std::runtime_error("Validation failed for module");
//     return mod;
//   }
// };



//--------------------------------------------------------------------------------------------------
// Simple ModuleFactory that returns std::unique_ptr<IModule>
// (Guarantees type matches std::vector<std::unique_ptr<IModule>>)
//--------------------------------------------------------------------------------------------------
class ModuleFactory {
public:
    template <typename T, typename... Args>
    static std::unique_ptr<IModule> create(Args&&... args) {
        static_assert(std::is_base_of<IModule, T>::value, "T must derive from IModule");
        auto mod = std::make_unique<T>(std::forward<Args>(args)...);
        if (!mod->validate()) {
            throw std::runtime_error("Validation failed for module");
        }
        return mod; // upcasts to unique_ptr<IModule> if T : IModule
    }
};
