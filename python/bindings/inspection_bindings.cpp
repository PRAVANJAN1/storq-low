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

    py::class_<StorqFormatMetadata>(module, "StorqFormatMetadata")
        .def_readonly("database_uuid_string", &StorqFormatMetadata::database_uuid_string)
        .def_property_readonly("version_major", [](const StorqFormatMetadata& metadata) { return metadata.header.major_version; })
        .def_property_readonly("version_minor", [](const StorqFormatMetadata& metadata) { return metadata.header.minor_version; })
        .def_property_readonly("version", [](const StorqFormatMetadata& metadata) {
            return std::to_string(metadata.header.major_version) + "." + std::to_string(metadata.header.minor_version);
        })
        .def_property_readonly("page_size", [](const StorqFormatMetadata& metadata) { return metadata.header.page_size; })
        .def_property_readonly("page_count", [](const StorqFormatMetadata& metadata) { return metadata.header.page_count; })
        .def_property_readonly("physical_file_size", &StorqFormatMetadata::physical_file_size)
        .def_property_readonly("metadata_generation", &StorqFormatMetadata::metadata_generation);

    module.def("supported_storq_page_sizes", []() {
        return std::vector<std::uint32_t>{4096, 8192, 16384, 32768, 65536};
    });
    module.def("create_storq_database", [](const std::filesystem::path& path, std::uint32_t page_size) {
        auto result = StorqFormat::create(path, page_size);
        if (!result) throw StorqException(result.error());
    });
    module.def("inspect_storq_database", [](const std::filesystem::path& path) {
        auto result = StorqFormat::open(path, true);
        if (!result) throw StorqException(result.error());
        return result.value();
    });
}
}
