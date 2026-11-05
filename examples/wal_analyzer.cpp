#include <iostream>
#include <memory>

#include <storq/storq.hpp>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: storq_wal_analyzer <database.sqlite-wal>\n";
        return 2;
    }

    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::WALAnalyzer>(runtime);
    auto registered = runtime.modules().register_module(analyzer);
    if (!registered) {
        std::cerr << registered.error().message << '\n';
        return 1;
    }
    auto initialized = runtime.initialize();
    if (!initialized) {
        std::cerr << initialized.error().message << '\n';
        return 1;
    }

    auto result = analyzer->inspect(argv[1]);
    if (!result) {
        std::cerr << result.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_wal_report(result.value());
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return result.value().structurally_valid ? 0 : 2;
}
