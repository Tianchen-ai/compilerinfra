#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ci_compiler ci_compiler;
typedef struct {
  void *data;
  size_t size;
} ci_buffer;
// ABI v1. Each non-null error string and each returned buffer belongs to the
// caller and must be released with ci_free in the same library.
uint32_t ci_abi_version(void);
const char *ci_open(const char *configuration_json, ci_compiler **out);
const char *ci_derive(const ci_compiler *, const char *configuration_json,
                      ci_compiler **out);
const char *ci_describe(const ci_compiler *, ci_buffer *json);
const char *ci_compile(const ci_compiler *, const char *exit_name,
                       const void *input, size_t size, const char *after,
                       const char *through, int assembly_text,
                       ci_buffer *output, ci_buffer *events_json);
void ci_close(ci_compiler *);
void ci_free(const void *);
#ifdef __cplusplus
}
#endif
