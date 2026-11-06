#include <pybind11/pybind11.h>

namespace pybind11 { void bind_core(module_&); void bind_adapters(module_&); void bind_workload(module_&); void bind_inspection(module_&); }

PYBIND11_MODULE(_native, module) {
    module.doc() = "Storq native database reliability engine";
    pybind11::bind_core(module);
    pybind11::bind_adapters(module);
    pybind11::bind_workload(module);
    pybind11::bind_inspection(module);
}
