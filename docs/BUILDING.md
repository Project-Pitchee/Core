# Building PitcheeCore V0

PitcheeCore V0 is built with CMake and links to ONNX Runtime. The validated
runtime version is `1.24.2`. ONNX Runtime is not vendored in this repository.

<https://onnxruntime.ai/docs/install/>

## CMake inputs

CMake accepts either explicit paths:

```text
PITCHEE_ORT_INCLUDE_DIR
PITCHEE_ORT_LIBRARY
```

or an ONNX Runtime root directory:

```text
PITCHEE_ORT_ROOT
```

The library and CLI targets are controlled by:

```text
PITCHEE_BUILD_SHARED=ON|OFF
PITCHEE_BUILD_CLI=ON|OFF
PITCHEE_BUILD_TESTS=ON|OFF
PITCHEE_ENABLE_ORT=ON|OFF
```

## macOS arm64

```bash
mkdir -p third_party
curl -L -o /tmp/onnxruntime-osx.tgz \
  https://github.com/microsoft/onnxruntime/releases/download/v1.24.2/onnxruntime-osx-arm64-1.24.2.tgz
tar -xzf /tmp/onnxruntime-osx.tgz -C third_party
mv third_party/onnxruntime-osx-arm64-1.24.2 third_party/onnxruntime

cmake -S . -B build-v0 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPITCHEE_ORT_INCLUDE_DIR="$PWD/third_party/onnxruntime/include" \
  -DPITCHEE_ORT_LIBRARY="$PWD/third_party/onnxruntime/lib/libonnxruntime.dylib"

cmake --build build-v0 -j 8
ctest --test-dir build-v0 --output-on-failure
```

For distribution, copy `libonnxruntime.dylib` into the application bundle and
set an `@rpath`, or place it next to the executable.

## Windows x64

```powershell
New-Item -ItemType Directory -Force third_party
Invoke-WebRequest `
  -Uri "https://github.com/microsoft/onnxruntime/releases/download/v1.24.2/onnxruntime-win-x64-1.24.2.zip" `
  -OutFile "$env:TEMP\onnxruntime-win.zip"
Expand-Archive "$env:TEMP\onnxruntime-win.zip" -DestinationPath third_party -Force
Rename-Item third_party\onnxruntime-win-x64-1.24.2 third_party\onnxruntime

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DPITCHEE_ORT_INCLUDE_DIR="$PWD\third_party\onnxruntime\include" `
  -DPITCHEE_ORT_LIBRARY="$PWD\third_party\onnxruntime\lib\onnxruntime.lib"

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Copy `onnxruntime.dll` next to the executable, or add its directory to `PATH`.

## Linux x64

```bash
mkdir -p third_party
curl -L -o /tmp/onnxruntime-linux.tgz \
  https://github.com/microsoft/onnxruntime/releases/download/v1.24.2/onnxruntime-linux-x64-1.24.2.tgz
tar -xzf /tmp/onnxruntime-linux.tgz -C third_party
mv third_party/onnxruntime-linux-x64-1.24.2 third_party/onnxruntime

cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPITCHEE_ORT_INCLUDE_DIR="$PWD/third_party/onnxruntime/include" \
  -DPITCHEE_ORT_LIBRARY="$PWD/third_party/onnxruntime/lib/libonnxruntime.so"

cmake --build build-linux -j 8
```

Set an rpath or add the ONNX Runtime directory to `LD_LIBRARY_PATH` at runtime.

## Android

```bash
curl -L -o /tmp/onnxruntime-android.aar \
  https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/1.24.2/onnxruntime-android-1.24.2.aar
unzip -q /tmp/onnxruntime-android.aar -d third_party/onnxruntime-android

cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_static \
  -DPITCHEE_BUILD_CLI=OFF \
  -DPITCHEE_ORT_INCLUDE_DIR="$PWD/third_party/onnxruntime-android/headers" \
  -DPITCHEE_ORT_LIBRARY="$PWD/third_party/onnxruntime-android/jni/arm64-v8a/libonnxruntime.so"

cmake --build build-android -j 8
```

Package `libpitchee_core.so`, `libonnxruntime.so`, and `models/` in the APK.

## iOS

Download the ONNX Runtime C/C++ pod archive:

```bash
curl -L -o /tmp/onnxruntime-ios.zip \
  https://download.onnxruntime.ai/pod-archive-onnxruntime-c-1.24.2.zip
unzip /tmp/onnxruntime-ios.zip -d third_party/onnxruntime-ios
```

Use the device or simulator slice of `onnxruntime.xcframework`:

```bash
cmake -S . -B build-ios -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1 \
  -DPITCHEE_BUILD_SHARED=OFF \
  -DPITCHEE_BUILD_CLI=OFF \
  -DPITCHEE_ORT_INCLUDE_DIR="$PWD/third_party/onnxruntime-ios/onnxruntime.framework/Headers" \
  -DPITCHEE_ORT_LIBRARY="$PWD/third_party/onnxruntime-ios/onnxruntime.framework/onnxruntime"

cmake --build build-ios -j 8
```

## Model placement

The host application must make this directory readable at runtime:

```text
models/
  wavlm-large-l12-hqq4-b128-convw8.onnx
  manifest.json
```

Pass that directory to `pitchee_v0_create()`. Core does not search an app bundle,
does not download models, and does not require a network connection.
