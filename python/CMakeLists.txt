find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_native bindings/module.cpp bindings/core_bindings.cpp
    bindings/adapter_bindings.cpp bindings/workload_bindings.cpp bindings/inspection_bindings.cpp
    bindings/record_bindings.cpp bindings/module18_bindings.cpp)
target_link_libraries(_native PRIVATE Storq::core)
target_compile_features(_native PRIVATE cxx_std_20)
