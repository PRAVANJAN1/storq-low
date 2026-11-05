#pragma once

#include <string_view>

#include <storq/common/result.hpp>

namespace storq {

class IModule {
public:
    virtual ~IModule() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<void> initialize() = 0;
    [[nodiscard]] virtual Result<void> shutdown() = 0;
    [[nodiscard]] virtual bool is_initialized() const noexcept = 0;
};

} // namespace storq
