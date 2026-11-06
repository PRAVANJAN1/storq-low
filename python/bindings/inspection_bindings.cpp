#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/storq.hpp>

namespace pybind11 {
namespace py = pybind11;

void bind_inspection(module_& module) {
    using namespace storq;

    py::class_<FormatInspectionResult>(module, "FormatInspectionResult")
        .def_readonly("format", &FormatInspectionResult::format)
        .def_readonly("file_size", &FormatInspectionResult::file_size)
        .def_readonly("page_size", &FormatInspectionResult::page_size)
        .def_readonly("page_count", &FormatInspectionResult::page_count)
        .def_readonly("header_valid", &FormatInspectionResult::header_valid)
        .def_readonly("structure_valid", &FormatInspectionResult::structure_valid);

    module.def("inspect_format", [](const std::filesystem::path& path) {
        Runtime runtime;
        auto initialized = runtime.initialize();
        if (!initialized) throw StorqException(initialized.error());
        FormatInspector inspector(runtime);
        auto result = inspector.inspect(path);
        static_cast<void>(runtime.shutdown());
        if (!result) throw StorqException(result.error());
        return result.value();
    });
}
}
