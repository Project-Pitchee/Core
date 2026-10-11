cmake_minimum_required(VERSION 3.20)

project(
    PitcheeCore
    VERSION 0.5.0
    DESCRIPTION "Cross-platform on-device voice analysis core"
    LANGUAGES C CXX
)

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS"
   AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 15.1)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.1" CACHE STRING
        "Minimum iOS deployment target" FORCE)
endif()

option(PITCHEE_BUILD_SHARED "Build pitchee_core as a shared library" ON)
option(PITCHEE_BUILD_CLI "Build the desktop validation CLI" ON)
option(PITCHEE_BUILD_TESTS "Build core tests" ON)
option(PITCHEE_ENABLE_ORT "Enable ONNX Runtime model inference" ON)
option(PITCHEE_ENABLE_NCNN "Enable the Android ncnn model backend" OFF)
option(PITCHEE_NCNN_VULKAN "Enable ncnn Vulkan GPU support" OFF)
set(PITCHEE_ORT_ROOT "" CACHE PATH "Directory containing ONNX Runtime headers and libraries")
set(PITCHEE_ORT_INCLUDE_DIR "" CACHE PATH "Explicit ONNX Runtime include directory")
set(PITCHEE_ORT_LIBRARY "" CACHE FILEPATH "Explicit ONNX Runtime library path")
set(PITCHEE_NCNN_ROOT "" CACHE PATH "Directory containing ncnn headers and libraries")
set(PITCHEE_NCNN_INCLUDE_DIR "" CACHE PATH "Explicit ncnn include directory")
set(PITCHEE_NCNN_LIBRARY "" CACHE FILEPATH "Explicit ncnn library path")

if(ANDROID)
    set(PITCHEE_ENABLE_ORT OFF CACHE BOOL "Android builds are ncnn-only" FORCE)
    if(NOT PITCHEE_ENABLE_NCNN)
        message(FATAL_ERROR "Android builds require PITCHEE_ENABLE_NCNN=ON")
    endif()
endif()

if(PITCHEE_BUILD_SHARED)
    set(PITCHEE_LIBRARY_TYPE SHARED)
else()
    set(PITCHEE_LIBRARY_TYPE STATIC)
endif()

add_library(pitchee_core ${PITCHEE_LIBRARY_TYPE}
    src/analyzer.cpp
    src/audio.cpp
    src/hnr.cpp
    src/json.cpp
    src/naturalness.cpp
    src/model_runtime.cpp
    src/scoring.cpp
    src/spectrum.cpp
    src/vad.cpp
    src/voice_health.cpp
    src/wav_reader.cpp
)

add_library(Pitchee::Core ALIAS pitchee_core)

target_compile_features(pitchee_core PUBLIC cxx_std_17)
target_compile_definitions(pitchee_core PRIVATE PITCHEE_CORE_BUILD=1)
if(NOT PITCHEE_BUILD_SHARED)
    target_compile_definitions(pitchee_core PUBLIC PITCHEE_STATIC=1)
endif()
target_include_directories(pitchee_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(pitchee_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME pitchee_core
)

if(MSVC)
    target_compile_options(pitchee_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(pitchee_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PITCHEE_ENABLE_ORT)
    if(PITCHEE_ORT_INCLUDE_DIR)
        set(PITCHEE_ORT_INCLUDE "${PITCHEE_ORT_INCLUDE_DIR}")
    else()
        find_path(PITCHEE_ORT_INCLUDE onnxruntime_cxx_api.h
            HINTS
                "${PITCHEE_ORT_ROOT}/include"
                "$ENV{ONNXRUNTIME_ROOT}/include"
        )
    endif()

    if(PITCHEE_ORT_LIBRARY)
        set(PITCHEE_ORT_LIB "${PITCHEE_ORT_LIBRARY}")
    else()
        find_library(PITCHEE_ORT_LIB
            NAMES onnxruntime
            HINTS
                "${PITCHEE_ORT_ROOT}/lib"
                "${PITCHEE_ORT_ROOT}/lib64"
                "${PITCHEE_ORT_ROOT}/bin"
                "$ENV{ONNXRUNTIME_ROOT}/lib"
                "$ENV{ONNXRUNTIME_ROOT}/lib64"
        )
    endif()

    if(PITCHEE_ORT_INCLUDE AND PITCHEE_ORT_LIB)
        add_library(pitchee_onnxruntime UNKNOWN IMPORTED)
        set_target_properties(pitchee_onnxruntime PROPERTIES
            IMPORTED_LOCATION "${PITCHEE_ORT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PITCHEE_ORT_INCLUDE}"
        )
        target_link_libraries(pitchee_core PRIVATE pitchee_onnxruntime)
        target_compile_definitions(pitchee_core PRIVATE PITCHEE_ENABLE_ORT=1)
        message(STATUS "PitcheeCore: ONNX Runtime enabled")
        message(STATUS "  include: ${PITCHEE_ORT_INCLUDE}")
        message(STATUS "  library: ${PITCHEE_ORT_LIB}")
    else()
        message(WARNING "PitcheeCore: ONNX Runtime not found; model inference returns PITCHEE_ERROR_ORT_UNAVAILABLE")
    endif()
endif()

if(PITCHEE_ENABLE_NCNN)
    if(NOT ANDROID)
        message(FATAL_ERROR "PITCHEE_ENABLE_NCNN is currently Android-only")
    endif()

    if(PITCHEE_NCNN_INCLUDE_DIR)
        set(PITCHEE_NCNN_INCLUDE "${PITCHEE_NCNN_INCLUDE_DIR}")
    else()
        find_path(PITCHEE_NCNN_INCLUDE ncnn/net.h
            HINTS
                "${PITCHEE_NCNN_ROOT}/include"
                "$ENV{NCNN_ROOT}/include"
        )
    endif()

    if(PITCHEE_NCNN_LIBRARY)
        set(PITCHEE_NCNN_LIB "${PITCHEE_NCNN_LIBRARY}")
    else()
        find_library(PITCHEE_NCNN_LIB
            NAMES ncnn
            HINTS
                "${PITCHEE_NCNN_ROOT}/lib"
        )
    endif()

    if(NOT PITCHEE_NCNN_INCLUDE OR NOT PITCHEE_NCNN_LIB)
        message(FATAL_ERROR "ncnn headers or library were not found")
    endif()

    find_library(ANDROID_LOG_LIBRARY log REQUIRED)
    find_library(ANDROID_LIBRARY android REQUIRED)
    find_library(DL_LIBRARY dl REQUIRED)
    target_include_directories(pitchee_core PRIVATE "${PITCHEE_NCNN_INCLUDE}")
    target_sources(pitchee_core PRIVATE src/ncnn_pitchee_layers.cpp)
    target_compile_options(pitchee_core PRIVATE -fno-rtti)
    target_link_libraries(pitchee_core PRIVATE
        "${PITCHEE_NCNN_LIB}"
        "${ANDROID_LOG_LIBRARY}"
        "${ANDROID_LIBRARY}"
        "${DL_LIBRARY}"
        -fopenmp
    )
    target_compile_definitions(pitchee_core PRIVATE PITCHEE_ENABLE_NCNN=1)
    if(PITCHEE_NCNN_VULKAN)
        get_filename_component(PITCHEE_NCNN_LIBRARY_DIR "${PITCHEE_NCNN_LIB}" DIRECTORY)
        find_library(VULKAN_LIBRARY vulkan REQUIRED)
        set(PITCHEE_NCNN_VULKAN_DEPS
            "${PITCHEE_NCNN_LIBRARY_DIR}/libglslang.a"
            "${PITCHEE_NCNN_LIBRARY_DIR}/libSPIRV.a"
            "${PITCHEE_NCNN_LIBRARY_DIR}/libMachineIndependent.a"
            "${PITCHEE_NCNN_LIBRARY_DIR}/libGenericCodeGen.a"
            "${PITCHEE_NCNN_LIBRARY_DIR}/libOSDependent.a"
            "${PITCHEE_NCNN_LIBRARY_DIR}/libglslang-default-resource-limits.a"
            "${VULKAN_LIBRARY}"
        )
        target_link_libraries(pitchee_core PRIVATE ${PITCHEE_NCNN_VULKAN_DEPS})
        target_compile_definitions(pitchee_core PRIVATE PITCHEE_NCNN_VULKAN=1)
        message(STATUS "PitcheeCore: ncnn Vulkan support enabled")
    endif()
    message(STATUS "PitcheeCore: Android ncnn backend enabled")
    message(STATUS "  include: ${PITCHEE_NCNN_INCLUDE}")
    message(STATUS "  library: ${PITCHEE_NCNN_LIB}")
endif()

if(APPLE)
    target_link_libraries(pitchee_core PRIVATE
        "-framework Foundation"
        "-framework CoreML"
        "-framework Accelerate"
        "-framework CoreGraphics"
    )
endif()

if(ANDROID)
    target_link_options(pitchee_core PUBLIC "-Wl,-z,max-page-size=16384")
    target_link_options(pitchee_core PUBLIC "-static-openmp")
endif()

if(PITCHEE_BUILD_CLI)
    if(APPLE)
        enable_language(OBJCXX)
        add_executable(pitchee_cli
            tools/pitchee_cli.cpp
            tools/mac_recorder.mm
        )
        target_link_libraries(pitchee_cli PRIVATE
            "-framework AVFoundation"
            "-framework Foundation"
        )
    else()
        add_executable(pitchee_cli tools/pitchee_cli.cpp)
    endif()
    target_link_libraries(pitchee_cli PRIVATE pitchee_core)
endif()

if(PITCHEE_BUILD_TESTS)
    enable_testing()
    add_executable(pitchee_core_tests tests/core_tests.cpp)
    target_link_libraries(pitchee_core_tests PRIVATE pitchee_core)
    add_test(NAME pitchee_core_tests COMMAND pitchee_core_tests)

    add_executable(pitchee_resample_test tests/resample_test.cpp src/audio.cpp)
    target_compile_features(pitchee_resample_test PRIVATE cxx_std_17)
    target_include_directories(pitchee_resample_test PRIVATE src)
    add_test(NAME pitchee_resample_test COMMAND pitchee_resample_test)

    add_executable(pitchee_json_test tests/json_test.cpp src/json.cpp)
    target_compile_features(pitchee_json_test PRIVATE cxx_std_17)
    target_include_directories(pitchee_json_test PRIVATE src)
    add_test(NAME pitchee_json_test COMMAND pitchee_json_test)

    add_executable(pitchee_hnr_test tests/hnr_test.cpp src/hnr.cpp)
    target_compile_features(pitchee_hnr_test PRIVATE cxx_std_17)
    target_compile_definitions(pitchee_hnr_test PRIVATE PITCHEE_STATIC=1)
    target_include_directories(pitchee_hnr_test PRIVATE src include)
    add_test(NAME pitchee_hnr_test COMMAND pitchee_hnr_test)

    add_executable(pitchee_hnr_header_test tests/hnr_header_test.c src/hnr.cpp)
    target_compile_features(pitchee_hnr_header_test PRIVATE cxx_std_17)
    target_compile_definitions(pitchee_hnr_header_test PRIVATE PITCHEE_STATIC=1)
    target_include_directories(pitchee_hnr_header_test PRIVATE include)
    add_test(NAME pitchee_hnr_header_test COMMAND pitchee_hnr_header_test)

    add_executable(pitchee_spectrum_test tests/spectrum_test.cpp)
    target_link_libraries(pitchee_spectrum_test PRIVATE pitchee_core)
    add_test(NAME pitchee_spectrum_test COMMAND pitchee_spectrum_test)

    add_executable(pitchee_realtime_f0_test tests/realtime_f0_test.cpp)
    target_link_libraries(pitchee_realtime_f0_test PRIVATE pitchee_core)
    add_test(
        NAME pitchee_realtime_f0_test
        COMMAND pitchee_realtime_f0_test
            "${CMAKE_CURRENT_SOURCE_DIR}/models"
    )

    add_executable(pitchee_wav_reader_test tests/wav_reader_test.cpp src/wav_reader.cpp)
    target_compile_features(pitchee_wav_reader_test PRIVATE cxx_std_17)
    target_include_directories(pitchee_wav_reader_test PRIVATE src)
    add_test(NAME pitchee_wav_reader_test COMMAND pitchee_wav_reader_test)

    add_executable(pitchee_c_abi_test tests/c_abi_test.c)
    target_link_libraries(pitchee_c_abi_test PRIVATE pitchee_core)
    add_test(NAME pitchee_c_abi_test COMMAND pitchee_c_abi_test)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/PitcheeCoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PitcheeCoreConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PitcheeCore
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PitcheeCoreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(TARGETS pitchee_core
    EXPORT PitcheeCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY models/ DESTINATION share/pitchee/models)
install(FILES LICENSE DESTINATION share/pitchee)
install(EXPORT PitcheeCoreTargets
    FILE PitcheeCoreTargets.cmake
    NAMESPACE Pitchee::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PitcheeCore
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PitcheeCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PitcheeCoreConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PitcheeCore
)
