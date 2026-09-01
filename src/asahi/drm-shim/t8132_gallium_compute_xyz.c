/* SPDX-License-Identifier: MIT */

/*
 * Direct Gallium exact-output test for an Apple9 partial 3-D workgroup.
 *
 * GLES cannot populate pipe_grid_info::last_block.  This fixture deliberately
 * bypasses the GL state tracker while retaining the normal Gallium Asahi
 * shader compilation, resource binding, batch construction, and submission
 * paths.  The requested grid is:
 *
 *   block      = 4 x 3 x 3
 *   grid       = 2 x 2 x 2 workgroups
 *   last_block = 3 x 2 x 1
 *   global     = 7 x 5 x 4 invocations
 *
 * A 36-thread full workgroup deliberately crosses the 32-lane SIMD boundary,
 * while the asymmetric partial edge makes every XYZ system value observable.
 */

#include "asahi/compiler/agx_compile.h"
#include "asahi/compiler/agx_compile_apple9.h"
#include "compiler/nir/nir_builder.h"
#include "pipe-loader/pipe_loader.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/os_time.h"
#include "util/u_inlines.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_X 4u
#define BLOCK_Y 3u
#define BLOCK_Z 3u
#define GRID_X 2u
#define GRID_Y 2u
#define GRID_Z 2u
#define LAST_X 3u
#define LAST_Y 2u
#define LAST_Z 1u
#define GLOBAL_X (((GRID_X - 1u) * BLOCK_X) + LAST_X)
#define GLOBAL_Y (((GRID_Y - 1u) * BLOCK_Y) + LAST_Y)
#define GLOBAL_Z (((GRID_Z - 1u) * BLOCK_Z) + LAST_Z)
#define PAYLOAD_WORDS (GLOBAL_X * GLOBAL_Y * GLOBAL_Z)
#define PAYLOAD_BYTES (PAYLOAD_WORDS * sizeof(uint32_t))
#define PAYLOAD_OFFSET 0x1000u
#define TRAILING_GUARD_BYTES 0x1000u
#define BUFFER_BYTES (PAYLOAD_OFFSET + PAYLOAD_BYTES + TRAILING_GUARD_BYTES)

enum system_formula {
   SYSTEM_GLOBAL_ID,
   SYSTEM_LOCAL_ID,
   SYSTEM_WORKGROUP_ID,
   SYSTEM_WORKGROUP_SIZE,
   SYSTEM_LOCAL_INDEX,
   SYSTEM_SUBGROUP_INVOCATION,
   SYSTEM_SUBGROUP_ID,
   SYSTEM_SUBGROUP_SIZE,
   SYSTEM_FORMULA_COUNT,
};

static const char *
formula_name(enum system_formula formula)
{
   static const char *const names[SYSTEM_FORMULA_COUNT] = {
      [SYSTEM_GLOBAL_ID] = "global-id",
      [SYSTEM_LOCAL_ID] = "local-id",
      [SYSTEM_WORKGROUP_ID] = "workgroup-id",
      [SYSTEM_WORKGROUP_SIZE] = "workgroup-size",
      [SYSTEM_LOCAL_INDEX] = "local-index",
      [SYSTEM_SUBGROUP_INVOCATION] = "subgroup-invocation",
      [SYSTEM_SUBGROUP_ID] = "subgroup-id",
      [SYSTEM_SUBGROUP_SIZE] = "subgroup-size",
   };
   return formula < SYSTEM_FORMULA_COUNT ? names[formula] : "invalid";
}

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_XYZ_FAIL: %s\n", message);
   exit(1);
}

static uint8_t
poison_byte(size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0xa5c39e17u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static uint32_t
packed_xyz(unsigned x, unsigned y, unsigned z)
{
   return x | (y << 8) | (z << 16);
}

static uint32_t
system_value(enum system_formula formula, unsigned x, unsigned y, unsigned z)
{
   const unsigned local_x = x % BLOCK_X;
   const unsigned local_y = y % BLOCK_Y;
   const unsigned local_z = z % BLOCK_Z;
   const unsigned active_x =
      x / BLOCK_X == GRID_X - 1u ? LAST_X : BLOCK_X;
   const unsigned active_y =
      y / BLOCK_Y == GRID_Y - 1u ? LAST_Y : BLOCK_Y;
   const unsigned active_z =
      z / BLOCK_Z == GRID_Z - 1u ? LAST_Z : BLOCK_Z;
   const unsigned local_index =
      local_x + active_x * (local_y + active_y * local_z);
   uint32_t value;

   switch (formula) {
   case SYSTEM_GLOBAL_ID:
      value = packed_xyz(x, y, z);
      break;
   case SYSTEM_LOCAL_ID:
      value = packed_xyz(local_x, local_y, local_z);
      break;
   case SYSTEM_WORKGROUP_ID:
      value = packed_xyz(x / BLOCK_X, y / BLOCK_Y, z / BLOCK_Z);
      break;
   case SYSTEM_WORKGROUP_SIZE:
      /* Apple9 reports the active edge-group extent, matching the CDM
       * partial-workgroup tuple rather than blindly returning the nominal
       * shader local size. */
      value = packed_xyz(active_x, active_y, active_z);
      break;
   case SYSTEM_LOCAL_INDEX:
      value = local_index;
      break;
   case SYSTEM_SUBGROUP_INVOCATION:
      value = local_index % 32u;
      break;
   case SYSTEM_SUBGROUP_ID:
      value = local_index / 32u;
      break;
   case SYSTEM_SUBGROUP_SIZE:
      value = 32u;
      break;
   case SYSTEM_FORMULA_COUNT:
      abort();
   }

   return 0xa5000000u | ((uint32_t)formula << 24) | value;
}

static unsigned
coordinate_index(unsigned x, unsigned y, unsigned z)
{
   return x + GLOBAL_X * (y + GLOBAL_Y * z);
}

static void
write_word(uint8_t *bytes, size_t offset, uint32_t value)
{
   memcpy(bytes + offset, &value, sizeof(value));
}

static void
initialize_oracle(enum system_formula formula, uint8_t seed[BUFFER_BYTES],
                  uint8_t expected[BUFFER_BYTES])
{
   bool visited[PAYLOAD_WORDS] = {false};

   for (size_t i = 0; i < BUFFER_BYTES; ++i)
      seed[i] = expected[i] = poison_byte(i);

   for (unsigned z = 0; z < GLOBAL_Z; ++z) {
      for (unsigned y = 0; y < GLOBAL_Y; ++y) {
         for (unsigned x = 0; x < GLOBAL_X; ++x) {
            unsigned index = coordinate_index(x, y, z);
            if (index >= PAYLOAD_WORDS || visited[index])
               fail("coordinate oracle is not a dense bijection");

            visited[index] = true;
            write_word(expected,
                       PAYLOAD_OFFSET + index * sizeof(uint32_t),
                       system_value(formula, x, y, z));
         }
      }
   }

   for (unsigned i = 0; i < PAYLOAD_WORDS; ++i) {
      if (!visited[i])
         fail("coordinate oracle left a payload word unassigned");
   }
}

static nir_shader *
build_shader(const nir_shader_compiler_options *options,
             enum system_formula formula)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "t8132_system_%s", formula_name(formula));
   b.shader->info.workgroup_size[0] = BLOCK_X;
   b.shader->info.workgroup_size[1] = BLOCK_Y;
   b.shader->info.workgroup_size[2] = BLOCK_Z;
   b.shader->info.num_ssbos = 1;

   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, gid, 0);
   nir_def *y = nir_channel(&b, gid, 1);
   nir_def *z = nir_channel(&b, gid, 2);
   nir_def *index = nir_iadd(
      &b, x,
      nir_imul_imm(&b, nir_iadd(&b, y, nir_imul_imm(&b, z, GLOBAL_Y)),
                   GLOBAL_X));
   nir_def *value;
   switch (formula) {
   case SYSTEM_GLOBAL_ID:
      value = nir_ior(
         &b, x,
         nir_ior(&b, nir_ishl_imm(&b, y, 8),
                  nir_ishl_imm(&b, z, 16)));
      break;
   case SYSTEM_LOCAL_ID: {
      nir_def *id = nir_load_local_invocation_id(&b);
      value = nir_ior(
         &b, nir_channel(&b, id, 0),
         nir_ior(&b, nir_ishl_imm(&b, nir_channel(&b, id, 1), 8),
                  nir_ishl_imm(&b, nir_channel(&b, id, 2), 16)));
      break;
   }
   case SYSTEM_WORKGROUP_ID: {
      nir_def *id = nir_load_workgroup_id(&b);
      value = nir_ior(
         &b, nir_channel(&b, id, 0),
         nir_ior(&b, nir_ishl_imm(&b, nir_channel(&b, id, 1), 8),
                  nir_ishl_imm(&b, nir_channel(&b, id, 2), 16)));
      break;
   }
   case SYSTEM_WORKGROUP_SIZE: {
      nir_def *size = nir_load_workgroup_size(&b);
      value = nir_ior(
         &b, nir_channel(&b, size, 0),
         nir_ior(&b, nir_ishl_imm(&b, nir_channel(&b, size, 1), 8),
                  nir_ishl_imm(&b, nir_channel(&b, size, 2), 16)));
      break;
   }
   case SYSTEM_LOCAL_INDEX:
      value = nir_load_local_invocation_index(&b);
      break;
   case SYSTEM_SUBGROUP_INVOCATION:
      value = nir_load_subgroup_invocation(&b);
      break;
   case SYSTEM_SUBGROUP_ID:
      value = nir_load_subgroup_id(&b);
      break;
   case SYSTEM_SUBGROUP_SIZE:
      value = nir_load_subgroup_size(&b);
      break;
   case SYSTEM_FORMULA_COUNT:
      abort();
   }
   value = nir_ior_imm(&b, value,
                       0xa5000000u | ((uint32_t)formula << 24));
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0),
                  nir_imul_imm(&b, index, sizeof(uint32_t)),
                  .write_mask = 1, .align_mul = sizeof(uint32_t));

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   b.shader->info.num_ssbos = 1;
   nir_validate_shader(b.shader, "T8132 partial XYZ fixture");
   return b.shader;
}

static struct pipe_loader_device *
find_asahi_device(void)
{
   int count = pipe_loader_probe(NULL, 0, false);
   if (count <= 0)
      return NULL;

   struct pipe_loader_device **devices =
      calloc((size_t)count, sizeof(*devices));
   if (!devices)
      fail("allocate Gallium device list");

   int found = pipe_loader_probe(devices, count, false);
   struct pipe_loader_device *selected = NULL;
   for (int i = 0; i < found; ++i) {
      if (!selected && devices[i]->driver_name &&
          strcmp(devices[i]->driver_name, "asahi") == 0) {
         selected = devices[i];
      } else {
         pipe_loader_release(&devices[i], 1);
      }
   }
   free(devices);
   return selected;
}

static void
report_mismatch(const uint8_t actual[BUFFER_BYTES],
                const uint8_t expected[BUFFER_BYTES])
{
   for (size_t i = 0; i < BUFFER_BYTES; ++i) {
      if (actual[i] == expected[i])
         continue;

      const char *region = i < PAYLOAD_OFFSET
                              ? "leading-guard"
                           : i < PAYLOAD_OFFSET + PAYLOAD_BYTES
                              ? "payload"
                              : "trailing-guard";
      fprintf(stderr,
              "T8132_GALLIUM_COMPUTE_XYZ_FAIL: %s byte %#zx=%#x "
              "expected=%#x\n",
              region, i, actual[i], expected[i]);
      exit(1);
   }
}

static int
self_test(void)
{
   for (unsigned f = 0; f < SYSTEM_FORMULA_COUNT; ++f) {
      enum system_formula formula = (enum system_formula)f;
      uint8_t seed[BUFFER_BYTES], expected[BUFFER_BYTES];
      initialize_oracle(formula, seed, expected);

      if (PAYLOAD_OFFSET == 0 || PAYLOAD_WORDS != 140 ||
          memcmp(seed, expected, PAYLOAD_OFFSET) != 0 ||
          memcmp(seed + PAYLOAD_OFFSET + PAYLOAD_BYTES,
                 expected + PAYLOAD_OFFSET + PAYLOAD_BYTES,
                 TRAILING_GUARD_BYTES) != 0)
         fail("offline layout/oracle invariant");

      nir_shader *nir = build_shader(&agx_nir_options, formula);
      if (!nir || nir->info.workgroup_size[0] != BLOCK_X ||
          nir->info.workgroup_size[1] != BLOCK_Y ||
          nir->info.workgroup_size[2] != BLOCK_Z ||
          nir->info.num_ssbos != 1)
         fail("offline NIR invariant");

      struct agx_shader_part compiled = {0};
      struct agx_apple9_compute_profile profile = {0};
      const char *reason = NULL;
      if (!agx_compile_apple9_tiny(nir, &compiled, &profile, &reason)) {
         fprintf(stderr, "formula=%s Apple9 compile failed: %s\n",
                 formula_name(formula), reason ? reason : "no diagnostic");
         fail("offline Apple9 compile");
      }
      if (!compiled.binary || !compiled.info.binary_size ||
          profile.abi != AGX_APPLE9_COMPUTE_ABI_SSBO0_STORE_U32 ||
          profile.local_size[0] != BLOCK_X ||
          profile.local_size[1] != BLOCK_Y ||
          profile.local_size[2] != BLOCK_Z || profile.index_rank != 3 ||
          profile.index_stride[0] != 1 ||
          profile.index_stride[1] != GLOBAL_X ||
          profile.index_stride[2] != GLOBAL_X * GLOBAL_Y)
         fail("offline Apple9 profile invariant");
      free(compiled.binary);
      ralloc_free(nir);
   }

   printf("T8132_GALLIUM_COMPUTE_XYZ_SELF_TEST_OK cases=8 global=7x5x4 "
          "local=4x3x3 words=140 offset=%#x bytes=%zu\n",
          PAYLOAD_OFFSET, (size_t)BUFFER_BYTES);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
      return self_test();
   if (argc != 1) {
      fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
      return 2;
   }

   struct pipe_loader_device *device = find_asahi_device();
   if (!device)
      fail("Asahi Gallium device not found");

   struct pipe_screen *screen = pipe_loader_create_screen(device, false);
   if (!screen)
      fail("create Asahi Gallium screen");
   const char *name = screen->get_name(screen);
   if (!name || strstr(name, "Apple M4") == NULL)
      fail("unexpected Gallium renderer");
   char renderer[128];
   snprintf(renderer, sizeof(renderer), "%s", name);

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   if (!ctx)
      fail("create Gallium context");

   struct pipe_resource *buffer =
      pipe_buffer_create(screen, PIPE_BIND_SHADER_BUFFER, PIPE_USAGE_DEFAULT,
                         BUFFER_BYTES);
   if (!buffer)
      fail("create guarded SSBO");
   struct pipe_shader_buffer binding = {
      .buffer = buffer,
      .buffer_offset = PAYLOAD_OFFSET,
      .buffer_size = PAYLOAD_BYTES,
   };
   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, 1, &binding, 1);

   const struct pipe_grid_info grid = {
      .block = {BLOCK_X, BLOCK_Y, BLOCK_Z},
      .grid = {GRID_X, GRID_Y, GRID_Z},
      .last_block = {LAST_X, LAST_Y, LAST_Z},
   };
   for (unsigned f = 0; f < SYSTEM_FORMULA_COUNT; ++f) {
      enum system_formula formula = (enum system_formula)f;
      uint8_t seed[BUFFER_BYTES], expected[BUFFER_BYTES], actual[BUFFER_BYTES];
      initialize_oracle(formula, seed, expected);
      pipe_buffer_write(ctx, buffer, 0, BUFFER_BYTES, seed);

      nir_shader *nir =
         build_shader(screen->nir_options[MESA_SHADER_COMPUTE], formula);
      if (screen->finalize_nir)
         screen->finalize_nir(screen, nir, true);
      struct pipe_compute_state state = {
         .ir_type = PIPE_SHADER_IR_NIR,
         .prog = nir,
      };
      void *compute = ctx->create_compute_state(ctx, &state);
      /* The Asahi create_compute_state entrypoint consumes the NIR. */
      if (!compute) {
         fprintf(stderr, "formula=%s\n", formula_name(formula));
         fail("compile Apple9 system-value shader");
      }
      ctx->bind_compute_state(ctx, compute);
      ctx->launch_grid(ctx, &grid);

      struct pipe_fence_handle *fence = NULL;
      ctx->flush(ctx, &fence, PIPE_FLUSH_END_OF_FRAME);
      if (!fence ||
          !screen->fence_finish(screen, ctx, fence, OS_TIMEOUT_INFINITE))
         fail("wait for partial XYZ dispatch");
      screen->fence_reference(screen, &fence, NULL);

      struct pipe_transfer *transfer = NULL;
      const uint8_t *mapped = pipe_buffer_map_range(
         ctx, buffer, 0, BUFFER_BYTES, PIPE_MAP_READ, &transfer);
      if (!mapped)
         fail("map guarded SSBO for verification");
      memcpy(actual, mapped, BUFFER_BYTES);
      pipe_buffer_unmap(ctx, transfer);
      report_mismatch(actual, expected);

      ctx->bind_compute_state(ctx, NULL);
      ctx->delete_compute_state(ctx, compute);
      printf("T8132_GALLIUM_COMPUTE_XYZ_CASE_OK formula=%s bytes=%zu\n",
             formula_name(formula), (size_t)BUFFER_BYTES);
   }

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, 1, NULL, 0);
   pipe_resource_reference(&buffer, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   printf("T8132_GALLIUM_COMPUTE_XYZ_OK cases=8 global=7x5x4 "
          "local=4x3x3 grid=2x2x2 last=3x2x1 words=140 "
          "offset=%#x renderer=\"%s\"\n",
          PAYLOAD_OFFSET, renderer);
   return 0;
}
