/* SPDX-License-Identifier: MIT */

#ifndef T8132_APPLE9_COMPUTE_CASES_H
#define T8132_APPLE9_COMPUTE_CASES_H

#include <stddef.h>

#include <stddef.h>

const char *const *t8132_apple9_memory_case_names(size_t *count);
void t8132_apple9_run_memory_case(const char *name);

const char *const *t8132_apple9_geometry_case_names(size_t *count);
void t8132_apple9_run_geometry_case(const char *name);

const char *const *t8132_apple9_math_case_names(size_t *count);
int t8132_apple9_run_math_case(const char *name);

#endif
