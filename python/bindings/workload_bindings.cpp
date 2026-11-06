#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <storq/storq.hpp>

namespace pybind11 {
namespace py = pybind11;

void bind_workload(module_& module) {
    using namespace storq;

    py::enum_<KeyDistribution>(module, "KeyDistribution")
        .value("Uniform", KeyDistribution::Uniform)
        .value("Sequential", KeyDistribution::Sequential)
        .value("Hot", KeyDistribution::Hot)
        .value("Zipf", KeyDistribution::Zipf);

    py::enum_<SchedulingPolicy>(module, "SchedulingPolicy")
        .value("Sequential", SchedulingPolicy::Sequential)
        .value("DeterministicConcurrent", SchedulingPolicy::DeterministicConcurrent)
        .value("Concurrent", SchedulingPolicy::Concurrent);

    py::class_<WorkloadConfig>(module, "WorkloadConfig")
        .def(py::init<>())
        .def_readwrite("random_seed", &WorkloadConfig::random_seed)
        .def_readwrite("operation_count", &WorkloadConfig::operation_count)
        .def_readwrite("transaction_count", &WorkloadConfig::transaction_count)
        .def_readwrite("concurrency_level", &WorkloadConfig::concurrency_level)
        .def_readwrite("deterministic_execution", &WorkloadConfig::deterministic_execution)
        .def_readwrite("key_distribution", &WorkloadConfig::key_distribution)
        .def_readwrite("scheduling_policy", &WorkloadConfig::scheduling_policy)
        .def_readwrite("profile_name", &WorkloadConfig::profile_name);

    py::class_<WorkloadOperation>(module, "WorkloadOperation")
        .def_readonly("operation_id", &WorkloadOperation::operation_id)
        .def_readonly("transaction_id", &WorkloadOperation::transaction_id)
        .def_readonly("target_key", &WorkloadOperation::target_key)
        .def_readonly("sequence_number", &WorkloadOperation::sequence_number);

    py::class_<WorkloadTransaction>(module, "WorkloadTransaction")
        .def_readonly("transaction_id", &WorkloadTransaction::transaction_id)
        .def_readonly("operations", &WorkloadTransaction::operations)
        .def_readonly("expects_commit", &WorkloadTransaction::expects_commit)
        .def_readonly("commit_synchronized", &WorkloadTransaction::commit_synchronized);

    py::class_<TransactionGenerator>(module, "TransactionGenerator")
        .def(py::init<const WorkloadConfig&>())
        .def("generate", [](TransactionGenerator& generator) {
            auto result = generator.generate();
            if (!result) throw StorqException(result.error());
            return result.value();
        });

    py::class_<TransactionOracle>(module, "TransactionOracle")
        .def(py::init<>())
        .def("register_transaction", [](TransactionOracle& oracle, const WorkloadTransaction& transaction) {
            auto result = oracle.register_transaction(transaction);
            if (!result) throw StorqException(result.error());
        })
        .def("record_commit", [](TransactionOracle& oracle, const std::string& id, bool synchronized) {
            auto result = oracle.record_commit(id, synchronized);
            if (!result) throw StorqException(result.error());
        })
        .def("record_rollback", [](TransactionOracle& oracle, const std::string& id) {
            auto result = oracle.record_rollback(id);
            if (!result) throw StorqException(result.error());
        })
        .def_property_readonly("history", &TransactionOracle::get_transactions);
}
}
