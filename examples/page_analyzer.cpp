#include <iostream>
#include <memory>

#include <storq/storq.hpp>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: storq_page_analyzer <database.sqlite>\n";
        return 2;
    }

    storq::Runtime runtime;
    storq::StorageManager storage;
    auto format_inspector = std::make_shared<storq::FormatInspector>(runtime, storage);
    auto analyzer = std::make_shared<storq::PageAnalyzer>(
        runtime, storage, *format_inspector);
    auto format_module = runtime.modules().register_module(format_inspector);
    if (!format_module) {
        std::cerr << format_module.error().message << '\n';
        return 1;
    }
    auto analyzer_module = runtime.modules().register_module(analyzer);
    if (!analyzer_module) {
        std::cerr << analyzer_module.error().message << '\n';
        return 1;
    }
    auto initialized = runtime.initialize();
    if (!initialized) {
        std::cerr << initialized.error().message << '\n';
        return 1;
    }

    storq::StorageOpenOptions open_options;
    open_options.access = storq::StorageAccess::ReadOnly;
    auto file = storage.open_file(argv[1], open_options);
    if (!file) {
        std::cerr << file.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto detected = format_inspector->detect(*file.value());
    if (!detected) {
        std::cerr << detected.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << "Detected format: "
              << storq::format_name(detected.value().format) << '\n';

    auto page = analyzer->inspect_page(*file.value(), 1);
    if (!page) {
        std::cerr << page.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_page_analysis_report(page.value());
    auto scan = analyzer->scan_database(*file.value());
    if (!scan) {
        std::cerr << scan.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_database_analysis_report(scan.value());

    auto closed = file.value()->close();
    if (!closed) {
        std::cerr << closed.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return scan.value().complete_analysis ? 0 : 2;
}
