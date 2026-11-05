#include <iostream>

#include <storq/storq.hpp>

int main() {
    storq::Runtime runtime;
    storq::RuntimeConfig config;
    config.application_name = "Storq example";
    config.log_level = storq::LogLevel::Info;

    auto initialized = runtime.initialize(config);
    if (!initialized) {
        std::cerr << "Runtime initialization failed: " << initialized.error().message << '\n';
        return 1;
    }

    const auto& build = storq::version();
    std::cout << "Storq " << build.string << " (" << build.build_type << ", "
              << build.compiler << ")\n";
    std::cout << "Registered modules:\n";
    for (const auto& module : runtime.modules().modules()) {
        std::cout << "  " << module->name() << '\n';
    }
    std::cout << "Available capabilities:\n";
    for (const auto& capability : runtime.capabilities().list()) {
        std::cout << "  " << capability << '\n';
    }

    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << "Runtime shutdown failed: " << shutdown.error().message << '\n';
        return 1;
    }
    return 0;
}
