/* SPDX-License-Identifier: MIT */

/*
 * Direct Gallium exact-output test for a true rank-2 Apple9 dispatch.
 *
 * Both the local shape and the partial workgroup grid are asymmetric:
 *
 *   block      = 5 x 3 x 1
 *   grid       = 4 x 4 x 1 workgroups
 *   last_block = 2 x 2 x 1
 *   global     = 17 x 11 x 1 invocations
 *
 * The shader stores a coordinate signature through the rank-2 affine index
 * x + 17*y.  Flattening the grid, exchanging x/y, dropping last_block, or
 * publishing the wrong direct-CDM dimensions therefore produces holes,
 * overwrites, or wrong coordinate words.  Two non-page-aligned payload ranges
 * and the entire poison-filled BO are checked byte-for-byte.
 */

#include "asahi/compiler/agx_compile.h"
#include "asahi/compiler/agx_compile_apple9.h"
#include "compiler/nir/nir_builder.h"
#include "gallium/drivers/asahi/agx_apple9.h"
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

#define BLOCK_X 5u
#define BLOCK_Y 3u
#define BLOCK_Z 1u
#define GRID_X 4u
#define GRID_Y 4u
#define GRID_Z 1u
#define LAST_X 2u
#define LAST_Y 2u
#define LAST_Z 1u
#define GLOBAL_X (((GRID_X - 1u) * BLOCK_X) + LAST_X)
#define GLOBAL_Y (((GRID_Y - 1u) * BLOCK_Y) + LAST_Y)
#define GLOBAL_Z 1u
#define PAYLOAD_WORDS (GLOBAL_X * GLOBAL_Y)
#define PAYLOAD_BYTES (PAYLOAD_WORDS * sizeof(uint32_t))
#define BUFFER_BYTES 0x8000u
#define MAX_DISPATCHES 2u

static const size_t payload_offsets[MAX_DISPATCHES] = {
   0x13c0u,
   0x54c0u,
};

_Static_assert(GLOBAL_X == 17 && GLOBAL_Y == 11,
               "rank-2 geometry must remain asymmetric");
_Static_assert(PAYLOAD_WORDS == 187,
               "rank-2 payload must cover every invocation");
_Static_assert(0x54c0u + PAYLOAD_BYTES < BUFFER_BYTES,
               "rank-2 payloads require a trailing guard");

struct oracle {
   uint8_t seed[BUFFER_BYTES];
   uint8_t expected[BUFFER_BYTES];
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_XY_FAIL: %s\n", message);
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

static unsigned
coordinate_index(unsigned x, unsigned y)
{
   return x + GLOBAL_X * y;
}

static uint32_t
coordinate_value(unsigned x, unsigned y)
{
   unsigned index = coordinate_index(x, y);
   return 0xd2000000u | (index << 16) | (y << 8) | x;
}

static void
write_word(uint8_t *bytes, size_t offset, uint32_t value)
{
   memcpy(bytes + offset, &value, sizeof(value));
}

static uint32_t
read_word(const uint8_t *bytes, size_t offset)
{
   uint32_t value;
   memcpy(&value, bytes + offset, sizeof(value));
   return value;
}

static bool
offset_in_payload(size_t offset, unsigned dispatch_count)
{
   for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
      size_t start = payload_offsets[dispatch];
      if (offset >= start && offset < start + PAYLOAD_BYTES)
         return true;
   }
   return false;
}

static void
oracle_init(struct oracle *oracle, unsigned dispatch_count)
{
   if (!oracle || dispatch_count == 0 || dispatch_count > MAX_DISPATCHES)
      fail("invalid rank-2 oracle dispatch count");

   for (size_t offset = 0; offset < BUFFER_BYTES; ++offset)
      oracle->seed[offset] = oracle->expected[offset] = poison_byte(offset);

   for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
      bool visited[PAYLOAD_WORDS] = {false};
      for (unsigned y = 0; y < GLOBAL_Y; ++y) {
         for (unsigned x = 0; x < GLOBAL_X; ++x) {
            unsigned index = coordinate_index(x, y);
            if (index >= PAYLOAD_WORDS || visited[index])
               fail("rank-2 coordinate index is not a dense bijection");
            visited[index] = true;
            write_word(oracle->expected,
                       payload_offsets[dispatch] +
                          index * sizeof(uint32_t),
                       coordinate_value(x, y));
         }
      }

      for (unsigned i = 0; i < PAYLOAD_WORDS; ++i) {
         if (!visited[i])
            fail("rank-2 coordinate oracle left a word unassigned");
      }
   }
}

static void
check_oracle(void)
{
   struct oracle one, two;
   oracle_init(&one, 1);
   oracle_init(&two, 2);

   if (!payload_offsets[0] || !payload_offsets[1] ||
       (payload_offsets[0] & 0x3f) || (payload_offsets[1] & 0x3f) ||
       payload_offsets[0] + PAYLOAD_BYTES > payload_offsets[1] ||
       payload_offsets[1] + PAYLOAD_BYTES >= BUFFER_BYTES)
      fail("rank-2 payload layout invariant");

   for (size_t offset = 0; offset < BUFFER_BYTES; ++offset) {
      if (!offset_in_payload(offset, 1) &&
          one.expected[offset] != one.seed[offset])
         fail("single-dispatch rank-2 oracle changed a guard byte");
      if (!offset_in_payload(offset, 2) &&
          two.expected[offset] != two.seed[offset])
         fail("two-dispatch rank-2 oracle changed a guard byte");
   }

   if (memcmp(one.expected + payload_offsets[0],
              two.expected + payload_offsets[0], PAYLOAD_BYTES) != 0 ||
       memcmp(one.expected + payload_offsets[1],
              two.expected + payload_offsets[1], PAYLOAD_BYTES) == 0)
      fail("rank-2 one/two-dispatch oracles are not distinguishable");

   for (unsigned dispatch = 0; dispatch < MAX_DISPATCHES; ++dispatch) {
      for (unsigned index = 0; index < PAYLOAD_WORDS; ++index) {
         size_t offset = payload_offsets[dispatch] + index * sizeof(uint32_t);
         if (read_word(two.expected, offset) == read_word(two.seed, offset))
            fail("rank-2 payload word does not differ from poison");
      }
   }
}

static nir_shader *
build_shader(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "t8132_rank2_xy");
   b.shader->info.workgroup_size[0] = BLOCK_X;
   b.shader->info.workgroup_size[1] = BLOCK_Y;
   b.shader->info.workgroup_size[2] = BLOCK_Z;
   b.shader->info.num_ssbos = 1;

   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, gid, 0);
   nir_def *y = nir_channel(&b, gid, 1);
   nir_def *index = nir_iadd(&b, x, nir_imul_imm(&b, y, GLOBAL_X));
   nir_def *value = nir_ior(
      &b, nir_imm_int(&b, 0xd2000000u),
      nir_ior(&b, x,
              nir_ior(&b, nir_ishl_imm(&b, y, 8),
                       nir_ishl_imm(&b, index, 16))));
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0),
                  nir_imul_imm(&b, index, sizeof(uint32_t)),
                  .write_mask = 1, .align_mul = sizeof(uint32_t));

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   b.shader->info.num_ssbos = 1;
   nir_validate_shader(b.shader, "T8132 rank-2 XY fixture");
   return b.shader;
}

static uint32_t
get_u32(const uint8_t *bytes)
{
   uint32_t value;
   memcpy(&value, bytes, sizeof(value));
   return value;
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

static int
self_test(void)
{
   check_oracle();

   nir_shader *nir = build_shader(&agx_nir_options);
   if (!nir || nir->info.workgroup_size[0] != BLOCK_X ||
       nir->info.workgroup_size[1] != BLOCK_Y ||
       nir->info.workgroup_size[2] != BLOCK_Z || nir->info.num_ssbos != 1)
      fail("offline rank-2 NIR invariant");

   struct agx_shader_part compiled = {0};
   struct agx_apple9_compute_profile profile = {0};
   const char *reason = NULL;
   if (!agx_compile_apple9_tiny(nir, &compiled, &profile, &reason)) {
      fprintf(stderr, "Apple9 rank-2 compile failed: %s\n",
              reason ? reason : "no diagnostic");
      fail("offline rank-2 Apple9 compile");
   }
   if (!compiled.binary || !compiled.info.binary_size ||
       profile.abi != AGX_APPLE9_COMPUTE_ABI_SSBO0_STORE_U32 ||
       profile.local_size[0] != BLOCK_X ||
       profile.local_size[1] != BLOCK_Y ||
       profile.local_size[2] != BLOCK_Z || profile.index_rank != 2 ||
       profile.index_stride[0] != 1 ||
       profile.index_stride[1] != GLOBAL_X ||
       profile.index_stride[2] != 0)
      fail("offline rank-2 Apple9 profile invariant");

   const uint32_t global[3] = {GLOBAL_X, GLOBAL_Y, GLOBAL_Z};
   const uint32_t local[3] = {BLOCK_X, BLOCK_Y, BLOCK_Z};
   uint8_t cdm[AGX_APPLE9_COMPUTE_CDM_RECORD_SIZE] = {0};
   if (!agx_apple9_compute_grid_supported(&profile, global, local) ||
       !agx_apple9_emit_direct_dispatch(
          cdm, UINT64_C(0x100000), global, local, &profile) ||
       get_u32(cdm + 0x10) != GLOBAL_X ||
       get_u32(cdm + 0x14) != GLOBAL_Y ||
       get_u32(cdm + 0x18) != GLOBAL_Z ||
       get_u32(cdm + 0x1c) != BLOCK_X ||
       get_u32(cdm + 0x20) != BLOCK_Y ||
       get_u32(cdm + 0x24) != BLOCK_Z)
      fail("offline rank-2 direct-dispatch package invariant");

   struct agx_apple9_compute_profile flattened = profile;
   flattened.index_rank = 1;
   flattened.index_stride[1] = 0;
   uint8_t rejected[AGX_APPLE9_COMPUTE_CDM_RECORD_SIZE];
   uint8_t rejected_before[sizeof(rejected)];
   memset(rejected, 0xa5, sizeof(rejected));
   memcpy(rejected_before, rejected, sizeof(rejected));
   if (agx_apple9_emit_direct_dispatch(
          rejected, UINT64_C(0x100000), global, local, &flattened) ||
       memcmp(rejected, rejected_before, sizeof(rejected)) != 0)
      fail("rank-2 package accepted or mutated a flattened profile");

   size_t binary_size = compiled.info.binary_size;
   free(compiled.binary);
   ralloc_free(nir);

   printf("T8132_GALLIUM_COMPUTE_XY_SELF_TEST_OK global=17x11x1 "
          "local=5x3x1 rank=2 words=187 offsets=%#zx,%#zx "
          "main=%zu\n",
          payload_offsets[0], payload_offsets[1], binary_size);
   return 0;
}

static void
report_mismatch(const uint8_t actual[BUFFER_BYTES],
                const uint8_t expected[BUFFER_BYTES],
                unsigned dispatch_count)
{
   for (size_t offset = 0; offset < BUFFER_BYTES; ++offset) {
      if (actual[offset] == expected[offset])
         continue;

      for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
         size_t start = payload_offsets[dispatch];
         if (offset < start || offset >= start + PAYLOAD_BYTES)
            continue;

         unsigned lane = (offset - start) / sizeof(uint32_t);
         unsigned x = lane % GLOBAL_X;
         unsigned y = lane / GLOBAL_X;
         size_t word_offset = start + lane * sizeof(uint32_t);
         fprintf(stderr,
                 "T8132_GALLIUM_COMPUTE_XY_FAIL: payload%u x=%u y=%u "
                 "byte=%zu actual_word=%#x expected_word=%#x\n",
                 dispatch, x, y, offset - word_offset,
                 read_word(actual, word_offset),
                 read_word(expected, word_offset));
         exit(1);
      }

      fprintf(stderr,
              "T8132_GALLIUM_COMPUTE_XY_FAIL: guard byte %#zx=%#x "
              "expected=%#x\n",
              offset, actual[offset], expected[offset]);
      exit(1);
   }
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
      return self_test();
   bool two_dispatch =
      argc == 2 && strcmp(argv[1], "--two-dispatch") == 0;
   if (argc != 1 && !two_dispatch) {
      fprintf(stderr, "usage: %s [--self-test | --two-dispatch]\n", argv[0]);
      return 2;
   }
   unsigned dispatch_count = two_dispatch ? 2 : 1;

   struct oracle oracle;
   uint8_t actual[BUFFER_BYTES];
   oracle_init(&oracle, dispatch_count);

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

   nir_shader *nir = build_shader(screen->nir_options[MESA_SHADER_COMPUTE]);
   if (screen->finalize_nir)
      screen->finalize_nir(screen, nir, true);
   struct pipe_compute_state state = {
      .ir_type = PIPE_SHADER_IR_NIR,
      .prog = nir,
   };
   void *compute = ctx->create_compute_state(ctx, &state);
   /* The Asahi create_compute_state entrypoint consumes the NIR. */
   nir = NULL;
   if (!compute)
      fail("compile Apple9 rank-2 shader");
   ctx->bind_compute_state(ctx, compute);

   struct pipe_resource *buffer =
      pipe_buffer_create(screen, PIPE_BIND_SHADER_BUFFER, PIPE_USAGE_DEFAULT,
                         BUFFER_BYTES);
   if (!buffer)
      fail("create guarded rank-2 SSBO");
   pipe_buffer_write(ctx, buffer, 0, BUFFER_BYTES, oracle.seed);

   const struct pipe_grid_info grid = {
      .block = {BLOCK_X, BLOCK_Y, BLOCK_Z},
      .grid = {GRID_X, GRID_Y, GRID_Z},
      .last_block = {LAST_X, LAST_Y, LAST_Z},
   };
   for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
      struct pipe_shader_buffer binding = {
         .buffer = buffer,
         .buffer_offset = payload_offsets[dispatch],
         .buffer_size = PAYLOAD_BYTES,
      };
      ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, 1, &binding, 1);
      ctx->launch_grid(ctx, &grid);
   }

   struct pipe_fence_handle *fence = NULL;
   ctx->flush(ctx, &fence, PIPE_FLUSH_END_OF_FRAME);
   if (!fence ||
       !screen->fence_finish(screen, ctx, fence, OS_TIMEOUT_INFINITE))
      fail("wait for rank-2 dispatch");
   screen->fence_reference(screen, &fence, NULL);

   struct pipe_transfer *transfer = NULL;
   const uint8_t *mapped = pipe_buffer_map_range(
      ctx, buffer, 0, BUFFER_BYTES, PIPE_MAP_READ, &transfer);
   if (!mapped)
      fail("map guarded rank-2 SSBO for verification");
   memcpy(actual, mapped, BUFFER_BYTES);
   pipe_buffer_unmap(ctx, transfer);
   report_mismatch(actual, oracle.expected, dispatch_count);

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, 1, NULL, 0);
   ctx->bind_compute_state(ctx, NULL);
   ctx->delete_compute_state(ctx, compute);
   pipe_resource_reference(&buffer, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   printf("T8132_GALLIUM_COMPUTE_XY_OK dispatches=%u global=17x11x1 "
          "local=5x3x1 grid=4x4x1 last=2x2x1 rank=2 words=187 "
          "offsets=%#zx,%#zx renderer=\"%s\"\n",
          dispatch_count, payload_offsets[0], payload_offsets[1], renderer);
   return 0;
}
