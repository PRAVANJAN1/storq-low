#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/index/hash_index.hpp>
#include <storq/index/index_key.hpp>
#include <storq/wal/native_wal.hpp>

namespace py = pybind11;

namespace pybind11 {

void bind_module18(module_& module) {
    py::class_<storq::HashIndex>(module, "HashIndex")
        .def(py::init<std::string, std::filesystem::path>())
        .def("load", [](storq::HashIndex& index) {
            auto result = index.load();
            if (!result) throw py::value_error(result.error().message);
        })
        .def("save", [](const storq::HashIndex& index) {
            auto result = index.save();
            if (!result) throw py::value_error(result.error().message);
        });

    py::class_<storq::NativeWAL>(module, "NativeWAL")
        .def(py::init<std::filesystem::path>())
        .def("open", [](storq::NativeWAL& wal) {
            auto result = wal.open();
            if (!result) throw py::value_error(result.error().message);
        })
        .def("sync", [](storq::NativeWAL& wal) {
            auto result = wal.sync();
            if (!result) throw py::value_error(result.error().message);
        })
        .def_property_readonly("last_lsn", &storq::NativeWAL::last_lsn);
}

} // namespace pybind11
