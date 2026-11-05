#include <storq/storq.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestModule final : public storq::IModule {
public:
    TestModule(std::string module_name, std::vector<std::string>& events, bool should_fail = false,
               int shutdown_failures = 0)
        : name_(std::move(module_name)),
          events_(events),
          should_fail_(should_fail),
          shutdown_failures_(shutdown_failures) {}

    std::string_view name() const noexcept override { return name_; }

    storq::Result<void> initialize() override {
        events_.push_back("init:" + name_);
        if (should_fail_) {
            return storq::StorqError{storq::ErrorCategory::Module,
                                     storq::ErrorCode::InitializationFailed,
                                     "intentional test failure", {}};
        }
        initialized_ = true;
        return {};
    }

    storq::Result<void> shutdown() override {
        events_.push_back("stop:" + name_);
        if (shutdown_failures_ > 0) {
            --shutdown_failures_;
            return storq::StorqError{storq::ErrorCategory::Module,
                                     storq::ErrorCode::ShutdownFailed,
                                     "intentional shutdown failure", {}};
        }
        initialized_ = false;
        return {};
    }

    bool is_initialized() const noexcept override { return initialized_; }

private:
    std::string name_;
    std::vector<std::string>& events_;
    bool should_fail_;
    int shutdown_failures_;
    bool initialized_{false};
};

void test_result() {
    storq::Result<int> success(42);
    require(success && success.value() == 42, "Result value access failed.");
    const storq::Result<int> const_success(7);
    require(const_success.value() == 7, "Const Result value access failed.");

    storq::Result<int> failure(storq::StorqError{
        storq::ErrorCategory::General, storq::ErrorCode::Unknown, "failed", {}});
    require(!failure && failure.error().message == "failed", "Result error access failed.");
    bool threw = false;
    try {
        static_cast<void>(failure.value());
    } catch (const storq::StorqException&) {
        threw = true;
    }
    require(threw, "Invalid Result value access did not throw.");

    storq::Result<void> void_success;
    require(void_success.has_value(), "Result<void> success failed.");
    void_success.value();
    storq::Result<void> void_failure(storq::StorqError{
        storq::ErrorCategory::General, storq::ErrorCode::Unknown, "failed", {}});
    require(!void_failure, "Result<void> failure failed.");
}

void test_configuration() {
    storq::RuntimeConfig defaults;
    require(defaults.application_name == "Storq", "Unexpected default application name.");
    require(static_cast<bool>(storq::validate_config(defaults)), "Default configuration invalid.");

    auto invalid = defaults;
    invalid.application_name.clear();
    require(!storq::validate_config(invalid), "Empty application name was accepted.");
    invalid = defaults;
    invalid.log_destination = storq::LogDestination::File;
    require(!storq::validate_config(invalid), "Missing log path was accepted.");
    invalid = defaults;
    invalid.log_level = static_cast<storq::LogLevel>(99);
    require(!storq::validate_config(invalid), "Invalid log level was accepted.");

    storq::ConfigurationManager manager;
    auto updated = defaults;
    updated.environment = "test";
    require(static_cast<bool>(manager.update(updated)), "Valid configuration update failed.");
    require(manager.get().environment == "test", "Configuration update was not retained.");
}

void test_logger() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("storq-test-" + storq::generate_uuid());
    std::filesystem::create_directories(directory);
    const auto log_path = directory / "runtime.log";
    storq::RuntimeConfig config;
    config.log_destination = storq::LogDestination::File;
    config.log_file_path = log_path;
    config.log_level = storq::LogLevel::Warning;

    auto created = storq::Logger::create(config);
    require(static_cast<bool>(created), "File logger creation failed.");
    auto logger = std::move(created).value();
    require(static_cast<bool>(logger->info("filtered")), "Filtered log returned an error.");
    require(static_cast<bool>(logger->warning("visible warning")), "Warning log failed.");

    std::vector<std::thread> writers;
    std::atomic<bool> write_failed{false};
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([logger, index, &write_failed] {
            for (int entry = 0; entry < 20; ++entry) {
                auto result = logger->error("writer " + std::to_string(index));
                if (!result) {
                    write_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    require(!write_failed.load(std::memory_order_relaxed), "Concurrent logging failed.");
    logger.reset();

    std::ifstream input(log_path);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    require(contents.find("visible warning") != std::string::npos, "Warning absent from log.");
    require(contents.find("filtered") == std::string::npos, "Filtered message was logged.");
    require(contents.find("[Thread: ") != std::string::npos, "Thread ID absent from log.");
    std::size_t error_count = 0;
    auto position = contents.find("[ERROR]");
    while (position != std::string::npos) {
        ++error_count;
        position = contents.find("[ERROR]", position + 1);
    }
    require(error_count == 80, "Concurrent log entries were lost.");
    input.close();

    std::ostringstream console;
    auto* previous = std::clog.rdbuf(console.rdbuf());
    storq::RuntimeConfig console_config;
    console_config.log_destination = storq::LogDestination::Console;
    auto console_logger = storq::Logger::create(console_config);
    require(static_cast<bool>(console_logger), "Console logger creation failed.");
    require(static_cast<bool>(console_logger.value()->info("console message")),
            "Console logging failed.");
    std::clog.rdbuf(previous);
    require(console.str().find("console message") != std::string::npos,
            "Console message was not written.");
    std::filesystem::remove_all(directory);
}

void test_module_registry() {
    std::vector<std::string> events;
    storq::ModuleRegistry registry;
    auto first = std::make_shared<TestModule>("first", events);
    auto second = std::make_shared<TestModule>("second", events);
    require(static_cast<bool>(registry.register_module(first)), "Module registration failed.");
    require(!registry.register_module(std::make_shared<TestModule>("first", events)),
            "Duplicate module registration succeeded.");
    require(registry.find("first").value() == first, "Module lookup failed.");
    require(registry.modules().size() == 1, "Module enumeration failed.");
    require(static_cast<bool>(registry.register_module(second)), "Second registration failed.");
    require(static_cast<bool>(registry.initialize_all()), "Module initialization failed.");
    require(events == std::vector<std::string>{"init:first", "init:second"},
            "Modules initialized out of order.");
    require(!registry.unregister_module("first"), "Initialized module was unregistered.");
    require(static_cast<bool>(registry.shutdown_all()), "Module shutdown failed.");
    require(events[2] == "stop:second" && events[3] == "stop:first",
            "Modules did not shut down in reverse order.");

    events.clear();
    storq::ModuleRegistry failing_registry;
    require(static_cast<bool>(failing_registry.register_module(
                std::make_shared<TestModule>("ok", events, false, 1))), "Registration failed.");
    require(static_cast<bool>(failing_registry.register_module(
                std::make_shared<TestModule>("bad", events, true))), "Registration failed.");
    require(!failing_registry.initialize_all(), "Failed module initialization was accepted.");
    require(events == std::vector<std::string>{"init:ok", "init:bad", "stop:ok"},
            "Initialization failure did not roll back prior modules.");
    require(failing_registry.is_initialized(),
            "Registry forgot a module whose rollback failed.");
    require(static_cast<bool>(failing_registry.shutdown_all()),
            "Registry could not retry failed rollback cleanup.");
    require(events.back() == "stop:ok", "Failed rollback was not retried.");
}

void test_runtime() {
    storq::Runtime before_init;
    require(static_cast<bool>(before_init.shutdown()), "Pre-initialization shutdown failed.");
    require(!before_init.set_log_level(storq::LogLevel::Debug),
            "Log level update outside runtime was accepted.");

    storq::Runtime runtime;
    std::vector<std::string> events;
    require(static_cast<bool>(runtime.modules().register_module(
                std::make_shared<TestModule>("runtime-module", events))),
            "Runtime module registration failed.");
    storq::RuntimeConfig config;
    config.log_destination = storq::LogDestination::File;
    config.log_file_path = std::filesystem::temp_directory_path() /
                           ("storq-runtime-" + storq::generate_uuid() + ".log");
    require(static_cast<bool>(runtime.initialize(config)), "Runtime initialization failed.");
    require(runtime.state() == storq::RuntimeState::Running, "Runtime did not enter Running.");
    require(!runtime.initialize(config), "Repeated runtime initialization succeeded.");
    require(runtime.capabilities().contains("core_runtime"),
            "Core runtime capability was not registered.");
    require(!runtime.capabilities().contains("sqlite_adapter"),
            "Unimplemented capability was registered.");
    require(static_cast<bool>(runtime.set_log_level(storq::LogLevel::Debug)),
            "Runtime log-level update failed.");
    require(static_cast<bool>(runtime.shutdown()), "Runtime shutdown failed.");
    require(runtime.state() == storq::RuntimeState::Stopped, "Runtime did not stop.");
    std::filesystem::remove(config.log_file_path);

    storq::Runtime failed_runtime;
    std::vector<std::string> failed_events;
    require(static_cast<bool>(failed_runtime.modules().register_module(
                std::make_shared<TestModule>("failing-module", failed_events, true))),
            "Failing runtime module registration failed.");
    auto failure_config = storq::RuntimeConfig{};
    failure_config.log_destination = storq::LogDestination::File;
    failure_config.log_file_path = std::filesystem::temp_directory_path() /
                                   ("storq-failure-" + storq::generate_uuid() + ".log");
    require(!failed_runtime.initialize(failure_config),
            "Runtime initialization with a failing module succeeded.");
    require(failed_runtime.state() == storq::RuntimeState::Failed,
            "Runtime initialization failure did not enter Failed.");
    require(static_cast<bool>(failed_runtime.shutdown()),
            "Shutdown after failed initialization failed.");
    std::filesystem::remove(failure_config.log_file_path);

    storq::Runtime invalid_runtime;
    auto invalid_config = storq::RuntimeConfig{};
    invalid_config.application_name.clear();
    require(!invalid_runtime.initialize(invalid_config), "Invalid runtime config was accepted.");
    require(invalid_runtime.state() == storq::RuntimeState::Uninitialized,
            "Invalid config changed runtime state.");
}

void test_utilities_and_capabilities() {
    const auto uuid = storq::generate_uuid();
    require(uuid.size() == 36, "UUID has unexpected format.");
    require(storq::trim(" \t Storq \n") == "Storq", "String trimming failed.");
    require(storq::to_lower("StorQ") == "storq", "Lowercase conversion failed.");
    require(storq::format_timestamp().size() == 23, "Timestamp format is unexpected.");

    storq::CapabilityRegistry capabilities;
    require(capabilities.register_capability("test"), "Capability registration failed.");
    require(!capabilities.register_capability("test"), "Duplicate capability was accepted.");
    require(capabilities.contains("test"), "Capability lookup failed.");
    require(capabilities.list().size() == 1, "Capability listing failed.");

    int cleanup_count = 0;
    {
        auto cleanup = storq::make_scope_exit([&cleanup_count] { ++cleanup_count; });
    }
    require(cleanup_count == 1, "Scope cleanup did not execute.");
    require(storq::version().string == "0.1.0", "Version string is incorrect.");
}

} // namespace

int main() {
    try {
        test_result();
        test_configuration();
        test_logger();
        test_module_registry();
        test_runtime();
        test_utilities_and_capabilities();
        std::cout << "All Storq core tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
