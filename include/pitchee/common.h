#ifndef PITCHEE_COMMON_H
#define PITCHEE_COMMON_H

#include <stddef.h>
#include <stdint.h>

#if defined(PITCHEE_STATIC)
#  define PITCHEE_API
#elif defined(_WIN32)
#  if defined(PITCHEE_CORE_BUILD)
#    define PITCHEE_API __declspec(dllexport)
#  else
#    define PITCHEE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define PITCHEE_API __attribute__((visibility("default")))
#else
#  define PITCHEE_API
#endif

typedef enum pitchee_status_t {
    PITCHEE_SUCCESS = 0,
    PITCHEE_ERROR_INVALID_ARGUMENT = 1,
    PITCHEE_ERROR_IO = 2,
    PITCHEE_ERROR_ORT_UNAVAILABLE = 3,
    PITCHEE_ERROR_MODEL = 4,
    PITCHEE_ERROR_NO_SPEECH = 5,
    PITCHEE_ERROR_UNSUPPORTED_FORMAT = 6,
    PITCHEE_ERROR_INTERNAL = 7
} pitchee_status_t;

#endif
