# PitcheeCore V0

`experiment` 分支是一个独立的 V0 三分类语音内核。它只做一件事：输入一段
语音，判断它属于 `male_natural`、`male_unnatural` 或 `female_natural`，并
返回置信度、音色分、自然度分以及是否通过。它不包含原 Core 的 Analyzer、
VAD、实时 F0、嗓音健康或旧模型流水线。

## 输出

- `male_natural`：自然男声
- `male_unnatural`：不自然男声
- `female_natural`：跨女或顺女共用
- `confidence`：三分类最大概率
- `timbre_score`：音色分，受置信度上限约束
- `naturalness_score`：自然度分
- `passed`：音色分是否达到 50

所有平台使用同一个约 95 MB 的 ONNX 模型和 ONNX Runtime。没有第二套推理图，
没有额外推理后端，也没有静默回退路径。

## 音频契约

- 输入为单声道/交错 Float32 PCM，或 PCM16/PCM32/Float32 WAV。
- 原生采样率为 16 kHz；其它采样率在 Core 内重采样。
- 每次只分析一个最多 12 秒的窗口，不聚合多窗。
- 短音频不补到 12 秒，只补到 0.4 秒的最小输入；长音频按语音密度裁剪。
- M4A/AAC/MP3/Opus 必须先由平台解码为 PCM。

详细规则、评分公式和模型哈希见 [docs/V0_CORE.md](docs/V0_CORE.md)。

## 构建

依赖 CMake 3.20+、C++17 和 ONNX Runtime 1.24.2。ONNX Runtime 不随仓库分发。

```bash
cmake -S . -B build-v0 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPITCHEE_ORT_INCLUDE_DIR=/path/to/onnxruntime/include \
  -DPITCHEE_ORT_LIBRARY=/path/to/onnxruntime/lib/libonnxruntime.dylib

cmake --build build-v0 -j 8
ctest --test-dir build-v0 --output-on-failure
```

各平台下载地址和 Android/iOS 集成方式见
[docs/BUILDING.md](docs/BUILDING.md)。

## CLI

```bash
./build-v0/pitchee_v0_cli --json --model ./models ./voice.wav
```

## C API

```c
#include <pitchee/v0.h>

pitchee_v0_t* core = NULL;
char error[1024] = {0};
pitchee_v0_create("models", 2, 0, &core, error, sizeof(error));

pitchee_v0_result_t result = {
    .struct_size = sizeof(result),
    .version = PITCHEE_V0_RESULT_VERSION,
};
pitchee_v0_analyze_wav_file(
    core, "voice.wav", &result, error, sizeof(error)
);
pitchee_v0_destroy(core);
```

`pitchee_v0_analyze_pcm()` 用于平台已经解码好的 Float32 PCM。同一个实例的调用
必须串行；多线程服务应为每个 worker 创建独立实例。

## 模型目录

```text
models/
  wavlm-large-l12-hqq4-b128-convw8.onnx
  manifest.json
```

模型 SHA-256 在创建会话前校验。模型文件约 94.5 MiB，低于 GitHub 普通文件
100 MB 限制。

## 许可

Core 代码使用 GPL-3.0，见 [LICENSE](LICENSE)。模型与训练数据的上游许可需在
分发前单独核对。
