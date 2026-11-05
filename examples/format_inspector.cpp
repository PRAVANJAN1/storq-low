#include <iostream>
#include <memory>

#include <storq/storq.hpp>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: storq_format_inspector <database-file>\n";
        return 2;
    }

    storq::Runtime runtime;
    auto inspector = std::make_shared<storq::FormatInspector>(runtime);
    auto registered = runtime.modules().register_module(inspector);
    if (!registered) {
        std::cerr << registered.error().message << '\n';
        return 1;
    }
    auto initialized = runtime.initialize();
    if (!initialized) {
        std::cerr << initialized.error().message << '\n';
        return 1;
    }

    auto result = inspector->inspect(argv[1]);
    if (!result) {
        std::cerr << result.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_inspection_report(result.value());
    const bool success = result.value().status == storq::FormatStatus::Valid ||
                         result.value().status == storq::FormatStatus::ValidWithWarnings;
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return success ? 0 : 2;
}
