#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/storq.hpp>

namespace pybind11 {
namespace py = pybind11;

void bind_adapters(module_& module) {
    using namespace storq;

    py::enum_<DatabaseEngine>(module, "DatabaseEngine")
        .value("SQLite", DatabaseEngine::SQLite)
        .value("Unknown", DatabaseEngine::Unknown)
        .export_values();

    py::enum_<AdapterState>(module, "AdapterState")
        .value("Uninitialized", AdapterState::Uninitialized)
        .value("Initializing", AdapterState::Initializing)
        .value("Ready", AdapterState::Ready)
        .value("Connected", AdapterState::Connected)
        .value("Failed", AdapterState::Failed)
        .value("ShuttingDown", AdapterState::ShuttingDown)
        .value("Stopped", AdapterState::Stopped)
        .export_values();

    py::class_<AdapterConfig>(module, "AdapterConfig")
        .def(py::init<>())
        .def_readwrite("database_path", &AdapterConfig::database_path)
        .def_readwrite("settings", &AdapterConfig::settings);

    py::class_<DatabaseConnectionConfig>(module, "DatabaseConnectionConfig")
        .def(py::init<>())
        .def_readwrite("database_path", &DatabaseConnectionConfig::database_path)
        .def_readwrite("read_only", &DatabaseConnectionConfig::read_only)
        .def_readwrite("timeout", &DatabaseConnectionConfig::timeout);

    py::class_<DatabaseMetadata>(module, "DatabaseMetadata")
        .def_readonly("engine", &DatabaseMetadata::engine)
        .def_readonly("engine_version", &DatabaseMetadata::engine_version)
        .def_readonly("file_size", &DatabaseMetadata::file_size)
        .def_readonly("page_size", &DatabaseMetadata::page_size)
        .def_readonly("page_count", &DatabaseMetadata::page_count)
        .def_readonly("encoding", &DatabaseMetadata::encoding)
        .def_readonly("journal_mode", &DatabaseMetadata::journal_mode);

    py::class_<SQLiteAdapter, std::shared_ptr<SQLiteAdapter>>(module, "SQLiteAdapter")
        .def(py::init<>())
        .def("initialize", [](SQLiteAdapter& adapter, AdapterConfig config) {
            auto result = adapter.initialize(config);
            if (!result) throw StorqException(result.error());
        })
        .def("shutdown", [](SQLiteAdapter& adapter) {
            auto result = adapter.shutdown();
            if (!result) throw StorqException(result.error());
        })
        .def("can_open", [](SQLiteAdapter& adapter, const std::filesystem::path& path) {
            auto result = adapter.can_open(path);
            if (!result) throw StorqException(result.error());
            return result.value();
        })
        .def_property_readonly("engine", &SQLiteAdapter::engine)
        .def_property_readonly("name", &SQLiteAdapter::name)
        .def_property_readonly("version", &SQLiteAdapter::version)
        .def_property_readonly("state", &SQLiteAdapter::state);

    py::class_<SQLiteInspector>(module, "SQLiteInspector")
        .def(py::init<>())
        .def("inspect", [](SQLiteInspector& inspector, const std::filesystem::path& path) {
            auto result = inspector.inspect(path);
            if (!result) throw StorqException(result.error());
            return result.value();
        });
}
}
