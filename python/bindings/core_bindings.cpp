#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/storq.hpp>

namespace pybind11 {
namespace py = pybind11;

void bind_core(module_& module) {
    using namespace storq;

    py::register_exception<StorqException>(module, "StorqNativeError");

    py::class_<Runtime, std::shared_ptr<Runtime>>(module, "Runtime")
        .def(py::init<>())
        .def("initialize", [](Runtime& runtime) {
            auto result = runtime.initialize();
            if (!result) throw StorqException(result.error());
        })
        .def("shutdown", [](Runtime& runtime) {
            auto result = runtime.shutdown();
            if (!result) throw StorqException(result.error());
        })
        .def_property_readonly("state", &Runtime::state)
        .def("__enter__", [](Runtime& runtime) -> Runtime& {
            auto result = runtime.initialize();
            if (!result) throw StorqException(result.error());
            return runtime;
        }, py::return_value_policy::reference)
        .def("__exit__", [](Runtime& runtime, py::object, py::object, py::object) {
            static_cast<void>(runtime.shutdown());
            return false;
        });

    py::enum_<RuntimeState>(module, "RuntimeState")
        .value("Uninitialized", RuntimeState::Uninitialized)
        .value("Initializing", RuntimeState::Initializing)
        .value("Running", RuntimeState::Running)
        .value("ShuttingDown", RuntimeState::ShuttingDown)
        .value("Stopped", RuntimeState::Stopped)
        .value("Failed", RuntimeState::Failed)
        .export_values();

    py::class_<VersionInfo>(module, "VersionInfo")
        .def_readonly("major", &VersionInfo::major)
        .def_readonly("minor", &VersionInfo::minor)
        .def_readonly("patch", &VersionInfo::patch)
        .def_property_readonly("string", [](const VersionInfo& value) { return std::string(value.string); });
}
}
