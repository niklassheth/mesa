/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AGX_APPLE9_PROFILE_H
#define AGX_APPLE9_PROFILE_H

#include <stdint.h>

/*
 * A main program and its client package form one Apple9 ABI. The generic
 * compiler currently selects one package shape from the number of caller
 * resources. Keep this value opaque until those package programs can be
 * generated semantically.
 */
enum agx_apple9_compute_abi {
   AGX_APPLE9_COMPUTE_ABI_INVALID = 0,
   AGX_APPLE9_COMPUTE_ABI_SSBO0_STORE_U32,
   AGX_APPLE9_COMPUTE_ABI_SSBO2_INTEGER_U32,
   AGX_APPLE9_COMPUTE_ABI_SSBO3_STATE_U6,
   AGX_APPLE9_COMPUTE_ABI_SSBO4_INTEGER_U32,
};

#define AGX_APPLE9_COMPUTE_STATE_LITERAL_STORAGE_CAPACITY 8
#define AGX_APPLE9_SSBO0_STATE_LITERAL_CAPACITY           2
#define AGX_APPLE9_SSBO3_STATE_U6_LITERAL_CAPACITY        2
#define AGX_APPLE9_SSBO3_STATE_U6_UNIFORM_BASE            6
#define AGX_APPLE9_SSBO4_STATE_LITERAL_CAPACITY            1
#define AGX_APPLE9_SSBO4_STATE_UNIFORM_BASE                8

enum agx_apple9_compute_access_mode {
   /* One scalar element, or the affine/vector range below, per invocation. */
   AGX_APPLE9_COMPUTE_ACCESS_PER_INVOCATION_U32 = 0,

   /* Every invocation addresses the same scalar element. */
   AGX_APPLE9_COMPUTE_ACCESS_CONSTANT_U32,

   /* A runtime-computed index is bounded by resource_access_add. */
   AGX_APPLE9_COMPUTE_ACCESS_BOUNDED_INDEX,
};

enum agx_apple9_compute_resource_kind {
   AGX_APPLE9_COMPUTE_RESOURCE_SSBO = 0,
   AGX_APPLE9_COMPUTE_RESOURCE_UBO,
};

struct agx_apple9_compute_profile {
   enum agx_apple9_compute_abi abi;

   /* Mapping from native package arguments to API buffer bindings. */
   uint8_t resource_binding_count;
   uint8_t resource_binding[4];
   enum agx_apple9_compute_resource_kind resource_kind[4];

   /* Shader-local dispatch and linear invocation-index contract. */
   uint32_t local_size[3];
   uint32_t index_stride[3];
   uint8_t index_rank;

   uint32_t required_threadgroup_memory_bytes;

   /* Bounds for each package resource, expressed in scalar elements. */
   uint32_t resource_access_tail[4];
   uint32_t resource_access_scale[4];
   uint32_t resource_access_add[4];
   uint8_t resource_access_element_size[4];
   enum agx_apple9_compute_access_mode resource_access_mode[4];

   /* Caller state published by package ABIs with a uniform window. */
   uint32_t state_literals[AGX_APPLE9_COMPUTE_STATE_LITERAL_STORAGE_CAPACITY];
   uint8_t state_literal_count;
};

#define AGX_APPLE9_TINY_COMPUTE_PROFILE                                      \
   ((struct agx_apple9_compute_profile){                                     \
      .abi = AGX_APPLE9_COMPUTE_ABI_SSBO0_STORE_U32,                         \
      .local_size = {256, 1, 1},                                            \
      .index_stride = {1, 0, 0},                                            \
      .index_rank = 1,                                                       \
   })

#define AGX_APPLE9_SSBO2_COMPUTE_PROFILE                                     \
   ((struct agx_apple9_compute_profile){                                     \
      .abi = AGX_APPLE9_COMPUTE_ABI_SSBO2_INTEGER_U32,                       \
      .local_size = {32, 1, 1},                                             \
      .index_stride = {1, 0, 0},                                            \
      .index_rank = 1,                                                       \
   })

#define AGX_APPLE9_SSBO3_STATE_U6_COMPUTE_PROFILE                            \
   ((struct agx_apple9_compute_profile){                                     \
      .abi = AGX_APPLE9_COMPUTE_ABI_SSBO3_STATE_U6,                          \
      .local_size = {32, 1, 1},                                             \
      .index_stride = {1, 0, 0},                                            \
      .index_rank = 1,                                                       \
   })

#define AGX_APPLE9_SSBO4_COMPUTE_PROFILE                                     \
   ((struct agx_apple9_compute_profile){                                     \
      .abi = AGX_APPLE9_COMPUTE_ABI_SSBO4_INTEGER_U32,                       \
      .local_size = {32, 1, 1},                                             \
      .index_stride = {1, 0, 0},                                            \
      .index_rank = 1,                                                       \
   })

#endif
