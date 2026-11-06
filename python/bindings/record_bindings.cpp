#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/records/record_manager.hpp>

namespace py = pybind11;

namespace pybind11 {
namespace {

storq::RecordValue python_value(const py::handle& value) {
    if (value.is_none()) return {};
    if (py::isinstance<py::bool_>(value)) return storq::RecordValue(py::cast<bool>(value));
    if (py::isinstance<py::int_>(value)) return storq::RecordValue(py::cast<std::int64_t>(value));
    if (py::isinstance<py::float_>(value)) return storq::RecordValue(py::cast<double>(value));
    if (py::isinstance<py::str>(value)) return storq::RecordValue(py::cast<std::string>(value));
    if (py::isinstance<py::bytes>(value)) {
        const auto bytes = py::cast<std::string>(value);
        std::vector<std::byte> output(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            output[i] = static_cast<std::byte>(static_cast<unsigned char>(bytes[i]));
        return storq::RecordValue(std::move(output));
    }
    throw py::type_error("Record values must be null, bool, integer, float, string, or bytes.");
}

storq::Record python_record(const py::dict& input) {
    storq::Record result;
    for (const auto& [key, value] : input)
        result.emplace(py::cast<std::string>(key), python_value(value));
    return result;
}

template <typename T>
T unwrap(storq::Result<T> result) {
    if (!result) throw py::value_error(result.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(result).value();
}

template <>
void unwrap<void>(storq::Result<void> result) {
    if (!result) throw py::value_error(result.error().message);
}

py::object python_value(const storq::RecordValue& value) {
    if (std::holds_alternative<std::monostate>(value.value)) return py::none();
    if (std::holds_alternative<bool>(value.value)) return py::cast(std::get<bool>(value.value));
    if (std::holds_alternative<std::int64_t>(value.value)) return py::cast(std::get<std::int64_t>(value.value));
    if (std::holds_alternative<double>(value.value)) return py::cast(std::get<double>(value.value));
    if (std::holds_alternative<std::string>(value.value)) return py::cast(std::get<std::string>(value.value));
    const auto& bytes = std::get<std::vector<std::byte>>(value.value);
    std::string output(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        output[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[i]));
    return py::bytes(output);
}

} // namespace

void bind_records(module_& module) {
    py::class_<storq::RecordID>(module, "RecordID")
        .def_readonly("page_id", &storq::RecordID::page_id)
        .def_readonly("slot_id", &storq::RecordID::slot_id)
        .def_readonly("generation", &storq::RecordID::generation)
        .def("__str__", &storq::RecordID::to_string)
        .def("__eq__", [](const storq::RecordID& a, const storq::RecordID& b) { return a == b; })
        .def("__hash__", [](const storq::RecordID& id) { return py::hash(py::str(id.to_string())); });

    py::class_<storq::RecordManager>(module, "RecordManager")
        .def(py::init<std::filesystem::path>())
        .def("open", [](storq::RecordManager& manager) { unwrap(manager.open()); })
        .def("close", [](storq::RecordManager& manager) { unwrap(manager.close()); })
        .def("insert", [](storq::RecordManager& manager, const py::dict& input) {
            return unwrap(manager.insert(python_record(input))).to_string();
        })
        .def("read", [](storq::RecordManager& manager, const std::string& id_text) {
            auto id = unwrap(storq::RecordManager::parse_record_id(id_text));
            auto record = unwrap(manager.read(id));
            py::dict output;
            for (const auto& [key, value] : record) output[py::str(key)] = python_value(value);
            return output;
        })
        .def("update", [](storq::RecordManager& manager, const std::string& id_text, const py::dict& input) {
            auto id = unwrap(storq::RecordManager::parse_record_id(id_text));
            unwrap(manager.update(id, python_record(input)));
        })
        .def("delete", [](storq::RecordManager& manager, const std::string& id_text) {
            auto id = unwrap(storq::RecordManager::parse_record_id(id_text));
            unwrap(manager.erase(id));
        })
        .def("exists", [](storq::RecordManager& manager, const std::string& id_text) {
            auto id = unwrap(storq::RecordManager::parse_record_id(id_text));
            return unwrap(manager.exists(id));
        });
}

} // namespace pybind11
