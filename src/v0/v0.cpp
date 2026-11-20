#include "pitchee/v0.h"

#include "../model_runtime.hpp"
#include "../sha256.hpp"
#include "../wav_reader.hpp"
#include "v0_audio.hpp"
#include "v0_head_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct pitchee_v0_t {
    std::unique_ptr<pitchee::ModelRuntime> model;
};

namespace {

constexpr double kSoftnessK = 3.0;
constexpr double kPassThreshold = 50.0;
constexpr double kConfidenceFloor = 0.85;
constexpr double kLowConfidenceCap = 49.9;
constexpr double kReferenceSoftnessK = 3.0;
constexpr char kExpectedModelSha256[] =
    "54f1499a09d88e585c0bebe02e049be46d4bc3bcc15afa42d6aec981ecbb81a9";

struct ScoreAnchor {
    double probability;
    double score;
};

constexpr ScoreAnchor kScoreAnchors[] = {
    {0.0001, 0.0},
    {0.001, 2.0},
    {0.01, 6.0},
    {0.05, 13.0},
    {0.10, 19.0},
    {0.30, 31.0},
    {0.50, 39.0},
    {0.70, 50.0},
    {0.82, 60.0},
    {0.90, 72.0},
    {0.92, 80.0},
    {0.97, 85.0},
    {0.985, 88.0},
    {0.995, 94.0},
    {0.999, 97.0},
};

void set_error(char* message, size_t capacity, const char* value) {
    if (!message || capacity == 0) return;
    const size_t length = std::min(std::strlen(value), capacity - 1);
    std::memcpy(message, value, length);
    message[length] = '\0';
}

pitchee_status_t status_for_exception(const std::exception& error) {
    const std::string message = error.what();
    if (message.find("ONNX Runtime") != std::string::npos
        || message.find("ORT") != std::string::npos) {
        return PITCHEE_ERROR_ORT_UNAVAILABLE;
    }
    if (message.find("open") != std::string::npos
        || message.find("read") != std::string::npos
        || message.find("WAV") != std::string::npos) {
        return PITCHEE_ERROR_IO;
    }
    if (message.find("model") != std::string::npos
        || message.find("input") != std::string::npos
        || message.find("output") != std::string::npos) {
        return PITCHEE_ERROR_MODEL;
    }
    return PITCHEE_ERROR_INTERNAL;
}

std::filesystem::path resolve_model_path(const char* model_directory) {
    std::filesystem::path path(model_directory);
    if (std::filesystem::is_directory(path)) {
        path /= PITCHEE_V0_DEFAULT_MODEL;
    }
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("model file not found: " + path.string());
    }
    return path;
}

void verify_model_file(const std::filesystem::path& path) {
    const std::string actual = pitchee::sha256_hex(pitchee::sha256_file(path));
    if (actual != kExpectedModelSha256) {
        throw std::runtime_error(
            "V0 model SHA-256 mismatch: expected "
            + std::string(kExpectedModelSha256) + ", got " + actual
        );
    }
}

double clip_probability(double probability) {
    return std::clamp(probability, 1e-6, 1.0 - 1e-6);
}

double sigmoid_score(double probability, double softness_k) {
    const double logit = std::log(clip_probability(probability) / (1.0 - clip_probability(probability)));
    const double k = softness_k > 0.05 ? softness_k : kSoftnessK;
    const double z = std::clamp(logit / k, -40.0, 40.0);
    return std::clamp(100.0 / (1.0 + std::exp(-z)), 1.0, 99.0);
}

double reference_timbre_score(double probability, double softness_k) {
    const double logit = std::log(clip_probability(probability) / (1.0 - clip_probability(probability)));
    double score = kScoreAnchors[std::size(kScoreAnchors) - 1].score;
    for (size_t index = 1; index < std::size(kScoreAnchors); ++index) {
        const ScoreAnchor& left = kScoreAnchors[index - 1];
        const ScoreAnchor& right = kScoreAnchors[index];
        const double left_logit = std::log(left.probability / (1.0 - left.probability));
        const double right_logit = std::log(right.probability / (1.0 - right.probability));
        if (logit <= left_logit) {
            score = left.score;
            break;
        }
        if (logit <= right_logit) {
            score = left.score
                + (logit - left_logit) / (right_logit - left_logit)
                    * (right.score - left.score);
            break;
        }
    }
    const double k = softness_k > 0.05 ? softness_k : kSoftnessK;
    const double softened = 50.0 + (score - 50.0) * (kReferenceSoftnessK / k);
    return std::clamp(softened, 1.0, 99.0);
}

double confidence_ceiling(double confidence) {
    const double value = std::clamp(confidence, 0.0, 1.0);
    if (value < kConfidenceFloor) return kLowConfidenceCap;
    return std::min(
        100.0,
        kPassThreshold
            + (100.0 - kPassThreshold) * (value - kConfidenceFloor)
                / (1.0 - kConfidenceFloor)
    );
}

std::array<double, 3> classify(const std::vector<float>& embedding) {
    if (embedding.size() != pitchee::v0::kFeatureCount) {
        throw std::runtime_error("WavLM embedding has an invalid dimension");
    }
    std::array<double, 3> logits = {
        pitchee::v0::kIntercept[0],
        pitchee::v0::kIntercept[1],
        pitchee::v0::kIntercept[2],
    };
    for (int feature = 0; feature < pitchee::v0::kFeatureCount; ++feature) {
        const double raw = static_cast<double>(embedding[static_cast<size_t>(feature)]);
        const double imputed = std::isnan(raw)
            ? pitchee::v0::kImputerStatistics[feature]
            : raw;
        const double scaled = (imputed - pitchee::v0::kScalerMean[feature])
            / pitchee::v0::kScalerScale[feature];
        for (int category = 0; category < 3; ++category) {
            logits[static_cast<size_t>(category)] += pitchee::v0::kCoefficients[
                static_cast<size_t>(category) * pitchee::v0::kFeatureCount + feature
            ] * scaled;
        }
    }
    double maximum = std::max({logits[0], logits[1], logits[2]});
    std::array<double, 3> probabilities{};
    double sum = 0.0;
    for (int category = 0; category < 3; ++category) {
        probabilities[static_cast<size_t>(category)] = std::exp(
            logits[static_cast<size_t>(category)] / pitchee::v0::kTemperature - maximum
        );
        sum += probabilities[static_cast<size_t>(category)];
    }
    for (double& probability : probabilities) probability /= sum;
    return probabilities;
}

pitchee_status_t analyze_impl(
    pitchee_v0_t* core,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_v0_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    if (!core || !samples || sample_count == 0 || sample_rate <= 0 || channels <= 0
        || !out_result) {
        set_error(error_message, error_message_capacity, "invalid V0 analysis argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    if (out_result->version != PITCHEE_V0_RESULT_VERSION
        || out_result->struct_size != sizeof(pitchee_v0_result_t)) {
        set_error(error_message, error_message_capacity, "unsupported V0 result layout");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    if (sample_count % static_cast<size_t>(channels) != 0) {
        set_error(error_message, error_message_capacity, "PCM sample count is not frame-aligned");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < sample_count; ++index) {
        if (!std::isfinite(samples[index])) {
            set_error(error_message, error_message_capacity, "PCM contains a non-finite sample");
            return PITCHEE_ERROR_INVALID_ARGUMENT;
        }
    }

    const auto overall_started = std::chrono::steady_clock::now();
    const auto preprocess_started = overall_started;
    pitchee::v0::ProcessedAudio audio;
    std::vector<float> normalized;
    try {
        audio = pitchee::v0::prepare_audio(
            samples,
            sample_count,
            sample_rate,
            channels
        );
        normalized = pitchee::v0::normalize_waveform(audio.samples);
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
    const auto inference_started = std::chrono::steady_clock::now();

    pitchee::Tensor hidden;
    try {
        pitchee::Tensor input;
        input.shape = {1, static_cast<int64_t>(normalized.size())};
        input.values = std::move(normalized);
        pitchee::Tensor mask;
        mask.shape = input.shape;
        mask.data_type = pitchee::Tensor::DataType::Int64;
        mask.int64_values.assign(static_cast<size_t>(input.shape[1]), 1);
        hidden = core->model->run({
            {"input_values", std::move(input)},
            {"attention_mask", std::move(mask)},
        });
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
    const auto postprocess_started = std::chrono::steady_clock::now();
    int64_t frames = 0;
    if (hidden.shape.size() == 3 && hidden.shape[0] == 1
        && hidden.shape[2] == pitchee::v0::kFeatureCount) {
        frames = hidden.shape[1];
    } else if (hidden.shape.size() == 2
               && hidden.shape[1] == pitchee::v0::kFeatureCount) {
        frames = hidden.shape[0];
    }
    if (frames <= 0) {
        set_error(error_message, error_message_capacity, "WavLM returned an invalid hidden-state shape");
        return PITCHEE_ERROR_MODEL;
    }
    std::vector<float> embedding(pitchee::v0::kFeatureCount, 0.0f);
    for (int64_t frame = 0; frame < frames; ++frame) {
        const float* row = hidden.values.data()
            + static_cast<size_t>(frame) * pitchee::v0::kFeatureCount;
        for (int feature = 0; feature < pitchee::v0::kFeatureCount; ++feature) {
            embedding[static_cast<size_t>(feature)] += row[feature];
        }
    }
    for (float& value : embedding) value /= static_cast<float>(frames);

    std::array<double, 3> probabilities{};
    try {
        probabilities = classify(embedding);
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_MODEL;
    }

    int predicted = 0;
    for (int category = 1; category < 3; ++category) {
        if (probabilities[static_cast<size_t>(category)]
            > probabilities[static_cast<size_t>(predicted)]) {
            predicted = category;
        }
    }
    const double confidence = probabilities[static_cast<size_t>(predicted)];
    const double timbre_base = reference_timbre_score(probabilities[2], kSoftnessK);
    const double timbre_cap = confidence_ceiling(confidence);
    const double timbre = std::min(timbre_base, timbre_cap);
    const double naturalness_probability = probabilities[0] + probabilities[2];
    const auto finished = std::chrono::steady_clock::now();

    out_result->predicted_class = predicted;
    out_result->passed = timbre >= kPassThreshold ? 1 : 0;
    for (int category = 0; category < 3; ++category) {
        out_result->probabilities[category] = probabilities[static_cast<size_t>(category)];
    }
    out_result->confidence = confidence;
    out_result->timbre_score = timbre;
    out_result->timbre_base = timbre_base;
    out_result->timbre_cap = timbre_cap;
    out_result->naturalness_score = sigmoid_score(naturalness_probability, kSoftnessK);
    out_result->source_seconds = audio.metadata.source_seconds;
    out_result->analyzed_seconds = audio.metadata.analyzed_seconds;
    out_result->speech_seconds = audio.metadata.speech_seconds;
    out_result->preprocess_ms = std::chrono::duration<double, std::milli>(
        inference_started - preprocess_started
    ).count();
    out_result->inference_ms = std::chrono::duration<double, std::milli>(
        postprocess_started - inference_started
    ).count();
    out_result->postprocess_ms = std::chrono::duration<double, std::milli>(
        finished - postprocess_started
    ).count();
    return PITCHEE_SUCCESS;
}

}  // namespace

extern "C" {

const char* pitchee_v0_version(void) {
    return pitchee::v0::kModelVersion;
}

const char* pitchee_v0_expected_model_sha256(void) {
    return kExpectedModelSha256;
}

pitchee_status_t pitchee_v0_create(
    const char* model_directory,
    int32_t intra_op_threads,
    int32_t use_coreml,
    pitchee_v0_t** out_core,
    char* error_message,
    size_t error_message_capacity
) {
    if (!model_directory || !out_core) {
        set_error(error_message, error_message_capacity, "invalid V0 create argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    *out_core = nullptr;
    try {
        const std::filesystem::path path = resolve_model_path(model_directory);
        verify_model_file(path);
        auto core = std::make_unique<pitchee_v0_t>();
        core->model = std::make_unique<pitchee::ModelRuntime>(
            path,
            intra_op_threads > 0 ? intra_op_threads : 2,
            use_coreml != 0
        );
        *out_core = core.release();
        return PITCHEE_SUCCESS;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

void pitchee_v0_destroy(pitchee_v0_t* core) {
    delete core;
}

pitchee_status_t pitchee_v0_analyze_pcm(
    pitchee_v0_t* core,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_v0_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    return analyze_impl(
        core,
        samples,
        sample_count,
        sample_rate,
        channels,
        out_result,
        error_message,
        error_message_capacity
    );
}

pitchee_status_t pitchee_v0_analyze_wav_file(
    pitchee_v0_t* core,
    const char* wav_path,
    pitchee_v0_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    if (!core || !wav_path || !out_result) {
        set_error(error_message, error_message_capacity, "invalid V0 WAV argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    try {
        const pitchee::WavData wav = pitchee::read_wav(wav_path);
        return analyze_impl(
            core,
            wav.samples.data(),
            wav.samples.size(),
            wav.sample_rate,
            wav.channels,
            out_result,
            error_message,
            error_message_capacity
        );
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

double pitchee_v0_timbre_score(double female_probability, double softness_k) {
    return reference_timbre_score(female_probability, softness_k);
}

double pitchee_v0_naturalness_score(double natural_probability, double softness_k) {
    return sigmoid_score(natural_probability, softness_k);
}

}  // extern "C"
