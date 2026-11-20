#ifndef PITCHEE_MODEL_RUNTIME_HPP
#define PITCHEE_MODEL_RUNTIME_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace pitchee {

struct Tensor {
    enum class DataType { Float32, Int64 };
    std::vector<int64_t> shape;
    std::vector<float> values;
    std::vector<int64_t> int64_values;
    DataType data_type = DataType::Float32;
};

class ModelRuntime {
public:
    ModelRuntime(
        const std::filesystem::path& path,
        int intra_op_threads,
        bool use_coreml
    );
    ~ModelRuntime();

    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;

    Tensor run(const std::unordered_map<std::string, Tensor>& inputs) const;
    std::vector<Tensor> run_all(
        const std::unordered_map<std::string, Tensor>& inputs
    ) const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

bool model_runtime_available();

}  // namespace pitchee

#endif
