/* SPDX-License-Identifier: MIT */

/* Exact-output Gallium fixture for the native SSBO4 dependent-load graphs:
 *
 * chain:  idx=indices[i]; idx2=indices2[idx]; out[i]=data[idx2]
 * reuse2: idx=indices[i]; x=a[idx]; y=b[idx]; out[i]=(x+y)^(x-y)
 *
 * This uses ordinary NIR only. Both index tables are bounded permutations of
 * 0..63. Native index_chain initialized its second table over 0..127; changing
 * those caller-owned values does not change the shader main or package. */

#include "compiler/nir/nir_builder.h"
#include "gallium/drivers/asahi/agx_state.h"
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

#define ELEMENTS     64u
#define LOCAL_SIZE   32u
#define GROUPS       (ELEMENTS / LOCAL_SIZE)
#define PAYLOAD_SIZE (ELEMENTS * sizeof(uint32_t))
#define BO_SIZE      0x1000u
#define BUFFER_COUNT 4u
#define DISPATCHES   2u
#define POISON       0xccu

enum formula {
   FORMULA_CHAIN,
   FORMULA_REUSE2,
};

enum buffer_index {
   BUFFER_INDEX0,
   BUFFER_INPUT1,
   BUFFER_INPUT2,
   BUFFER_OUTPUT,
};

static const size_t payload_offsets[DISPATCHES] = {
   0x140u,
   BO_SIZE - PAYLOAD_SIZE,
};

struct oracle {
   uint8_t seed[BUFFER_COUNT][BO_SIZE];
   uint8_t expected[BUFFER_COUNT][BO_SIZE];
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_DEVICE_GRAPH_FAIL: %s\n", message);
   exit(1);
}

static const char *
formula_name(enum formula formula)
{
   return formula == FORMULA_CHAIN ? "chain" : "reuse2";
}

static uint32_t
permutation0(unsigned lane)
{
   return (13u * lane + 7u) & 63u;
}

static uint32_t
permutation1(unsigned lane)
{
   return (5u * lane + 11u) & 63u;
}

static uint32_t
data_word(enum formula formula, unsigned dispatch, unsigned plane,
          unsigned lane)
{
   uint32_t value = 0x10203040u ^ formula * 0x6d2b79f5u;
   value ^= dispatch * 0x89abcdefu;
   value ^= plane * 0x31415927u;
   value += lane * (0x01010101u + plane * 0x00110011u);
   value ^= value >> 16;
   return value;
}

static void
write_word(uint8_t *image, size_t offset, uint32_t value)
{
   memcpy(image + offset, &value, sizeof(value));
}

static uint32_t
read_word(const uint8_t *image, size_t offset)
{
   uint32_t value;
   memcpy(&value, image + offset, sizeof(value));
   return value;
}

static uint32_t
reuse_value(uint32_t x, uint32_t y)
{
   return (x + y) ^ (x - y);
}

static void
oracle_init(struct oracle *oracle, enum formula formula)
{
   memset(oracle, formula == FORMULA_CHAIN ? 0xa7 : 0x5b, sizeof(*oracle));
   memset(oracle->seed[BUFFER_OUTPUT], POISON, BO_SIZE);
   memset(oracle->expected[BUFFER_OUTPUT], POISON, BO_SIZE);

   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      size_t base = payload_offsets[dispatch];
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t input0 = permutation0(lane);
         uint32_t input1 = formula == FORMULA_CHAIN
                              ? permutation1(lane)
                              : data_word(formula, dispatch, 0, lane);
         uint32_t input2 = data_word(formula, dispatch, 1, lane);
         size_t byte = base + lane * sizeof(uint32_t);
         write_word(oracle->seed[BUFFER_INDEX0], byte, input0);
         write_word(oracle->expected[BUFFER_INDEX0], byte, input0);
         write_word(oracle->seed[BUFFER_INPUT1], byte, input1);
         write_word(oracle->expected[BUFFER_INPUT1], byte, input1);
         write_word(oracle->seed[BUFFER_INPUT2], byte, input2);
         write_word(oracle->expected[BUFFER_INPUT2], byte, input2);
      }

      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         unsigned index = permutation0(lane);
         uint32_t value;
         if (formula == FORMULA_CHAIN) {
            unsigned index2 =
               read_word(oracle->seed[BUFFER_INPUT1], base + index * 4u);
            value = read_word(oracle->seed[BUFFER_INPUT2], base + index2 * 4u);
         } else {
            uint32_t x =
               read_word(oracle->seed[BUFFER_INPUT1], base + index * 4u);
            uint32_t y =
               read_word(oracle->seed[BUFFER_INPUT2], base + index * 4u);
            value = reuse_value(x, y);
         }
         write_word(oracle->expected[BUFFER_OUTPUT], base + lane * 4u, value);
      }
   }
}

static bool
images_differ(const uint32_t *a, const uint32_t *b)
{
   return memcmp(a, b, PAYLOAD_SIZE) != 0;
}

static void
check_oracle(const struct oracle *oracle, enum formula formula)
{
   for (unsigned b = 0; b < BUFFER_OUTPUT; ++b)
      if (memcmp(oracle->seed[b], oracle->expected[b], BO_SIZE))
         fail("CPU oracle mutates an input image");

   bool seen0[ELEMENTS] = {false};
   bool seen1[ELEMENTS] = {false};
   for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
      unsigned i0 = permutation0(lane), i1 = permutation1(lane);
      if (i0 >= ELEMENTS || i1 >= ELEMENTS || seen0[i0] || seen1[i1])
         fail("index table is not a bounded permutation");
      seen0[i0] = seen1[i1] = true;
   }

   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      size_t base = payload_offsets[dispatch];
      uint32_t actual[ELEMENTS];
      uint32_t wrong[8][ELEMENTS];

      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         actual[lane] = read_word(
            oracle->expected[BUFFER_OUTPUT], base + lane * sizeof(uint32_t));
         unsigned idx = permutation0(lane);
         uint32_t p1_gid =
            read_word(oracle->seed[BUFFER_INPUT1], base + lane * 4u);
         uint32_t p1_idx =
            read_word(oracle->seed[BUFFER_INPUT1], base + idx * 4u);
         uint32_t d_gid =
            read_word(oracle->seed[BUFFER_INPUT2], base + lane * 4u);
         uint32_t d_idx =
            read_word(oracle->seed[BUFFER_INPUT2], base + idx * 4u);

         if (formula == FORMULA_CHAIN) {
            unsigned swapped = permutation0(permutation1(lane));
            wrong[0][lane] = d_gid;
            wrong[1][lane] = d_idx;
            wrong[2][lane] =
               read_word(oracle->seed[BUFFER_INPUT2], base + p1_gid * 4u);
            wrong[3][lane] =
               read_word(oracle->seed[BUFFER_INPUT2], base + swapped * 4u);
            wrong[4][lane] = p1_idx;
            wrong[5][lane] = idx;
         } else {
            uint32_t x = p1_idx, y = d_idx;
            uint32_t x_gid = p1_gid, y_gid = d_gid;
            wrong[0][lane] = x;
            wrong[1][lane] = y;
            wrong[2][lane] = x + y;
            wrong[3][lane] = x - y;
            wrong[4][lane] = x ^ y;
            wrong[5][lane] = reuse_value(x_gid, y_gid);
            wrong[6][lane] = reuse_value(x, y_gid);
            wrong[7][lane] = reuse_value(x_gid, y);
         }
      }

      unsigned candidates = formula == FORMULA_CHAIN ? 6 : 8;
      for (unsigned candidate = 0; candidate < candidates; ++candidate)
         if (!images_differ(actual, wrong[candidate]))
            fail("oracle can false-pass through a wrong dependency graph");
   }
}

static nir_shader *
build_shader(const nir_shader_compiler_options *options, enum formula formula)
{
   nir_builder n = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "t8132_device_%s", formula_name(formula));
   n.shader->info.workgroup_size[0] = LOCAL_SIZE;
   n.shader->info.workgroup_size[1] = 1;
   n.shader->info.workgroup_size[2] = 1;
   n.shader->info.num_ssbos = BUFFER_COUNT;

   nir_def *gid = nir_channel(&n, nir_load_global_invocation_id(&n, 32), 0);
   nir_def *lane_offset = nir_imul_imm(&n, gid, sizeof(uint32_t));
   /* Compact Gallium order reverses native inputs: output=0, input2=1,
    * input1=2, index0=3. */
   nir_def *index = nir_load_ssbo(&n, 1, 32, nir_imm_int(&n, 3), lane_offset,
                                  .align_mul = sizeof(uint32_t));
   nir_def *index_offset = nir_imul_imm(&n, index, sizeof(uint32_t));
   nir_def *first = nir_load_ssbo(&n, 1, 32, nir_imm_int(&n, 2), index_offset,
                                  .align_mul = sizeof(uint32_t));
   nir_def *second_offset = formula == FORMULA_CHAIN
                               ? nir_imul_imm(&n, first, sizeof(uint32_t))
                               : index_offset;
   nir_def *second = nir_load_ssbo(&n, 1, 32, nir_imm_int(&n, 1), second_offset,
                                   .align_mul = sizeof(uint32_t));
   nir_def *value = second;
   if (formula == FORMULA_REUSE2)
      value =
         nir_ixor(&n, nir_iadd(&n, first, second), nir_isub(&n, first, second));
   nir_store_ssbo(&n, value, nir_imm_int(&n, 0), lane_offset, .write_mask = 1,
                  .align_mul = sizeof(uint32_t));

   nir_shader_gather_info(n.shader, nir_shader_get_entrypoint(n.shader));
   n.shader->info.num_ssbos = BUFFER_COUNT;
   nir_validate_shader(n.shader, "T8132 SSBO4 device-graph fixture");
   return n.shader;
}

static int
self_test(void)
{
   struct oracle chain, reuse;
   oracle_init(&chain, FORMULA_CHAIN);
   oracle_init(&reuse, FORMULA_REUSE2);
   check_oracle(&chain, FORMULA_CHAIN);
   check_oracle(&reuse, FORMULA_REUSE2);
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      bool seeds_distinct =
         b == BUFFER_OUTPUT || memcmp(chain.seed[b], reuse.seed[b], BO_SIZE);
      if (!seeds_distinct ||
          !memcmp(chain.expected[b], reuse.expected[b], BO_SIZE))
         fail("chain/reuse2 full images are not distinct");
   }
   if (!payload_offsets[0] || payload_offsets[0] + PAYLOAD_SIZE >= BO_SIZE ||
       payload_offsets[1] + PAYLOAD_SIZE != BO_SIZE)
      fail("nonzero/end binding layout invariant");
   printf("T8132_GALLIUM_COMPUTE_DEVICE_GRAPH_SELF_TEST_OK "
          "cases=chain,reuse2 elements=64 local=32 spans=0x100 "
          "dispatches=2 offsets=0x140,0xf00 full_bo=0x1000\n");
   return 0;
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
          !strcmp(devices[i]->driver_name, "asahi"))
         selected = devices[i];
      else
         pipe_loader_release(&devices[i], 1);
   }
   free(devices);
   return selected;
}

static void
finish(struct pipe_screen *screen, struct pipe_context *ctx)
{
   struct pipe_fence_handle *fence = NULL;
   ctx->flush(ctx, &fence, PIPE_FLUSH_END_OF_FRAME);
   if (!fence || !screen->fence_finish(screen, ctx, fence, OS_TIMEOUT_INFINITE))
      fail("wait for device-graph dispatches");
   screen->fence_reference(screen, &fence, NULL);
}

static void
verify(struct pipe_context *ctx, struct pipe_resource *resources[BUFFER_COUNT],
       const struct oracle *oracle)
{
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      struct pipe_transfer *transfer = NULL;
      const uint8_t *map = pipe_buffer_map_range(ctx, resources[b], 0, BO_SIZE,
                                                 PIPE_MAP_READ, &transfer);
      if (!map)
         fail("map full device-graph BO");
      if (memcmp(map, oracle->expected[b], BO_SIZE)) {
         pipe_buffer_unmap(ctx, transfer);
         fail(b == BUFFER_OUTPUT ? "full output/guard mismatch"
                                 : "input BO mutation");
      }
      pipe_buffer_unmap(ctx, transfer);
   }
}

static void
set_bindings(struct pipe_context *ctx,
             struct pipe_resource *resources[BUFFER_COUNT], unsigned dispatch,
             int undersized_slot)
{
   static const unsigned slot_to_buffer[BUFFER_COUNT] = {
      BUFFER_OUTPUT,
      BUFFER_INPUT2,
      BUFFER_INPUT1,
      BUFFER_INDEX0,
   };
   struct pipe_shader_buffer bindings[BUFFER_COUNT] = {0};
   for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot) {
      bindings[slot] = (struct pipe_shader_buffer){
         .buffer = resources[slot_to_buffer[slot]],
         .buffer_offset = payload_offsets[dispatch],
         .buffer_size =
            PAYLOAD_SIZE - (slot == (unsigned)undersized_slot ? 4u : 0u),
      };
   }
   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT, bindings,
                           BITFIELD_BIT(0));
}

static void
check_underbind(struct pipe_context *ctx,
                struct pipe_resource *resources[BUFFER_COUNT],
                const struct pipe_grid_info *grid, unsigned slot)
{
   set_bindings(ctx, resources, 0, (int)slot);
   struct agx_batch *before = agx_get_compute_batch(agx_context(ctx));
   if (!before)
      fail("obtain batch for SSBO4 underbind gate");
   uint32_t count = before->apple9_dispatch_count;
   uint32_t launch = before->apple9_launch_next;
   uint32_t resource = before->apple9_resource_next;
   ctx->launch_grid(ctx, grid);
   struct agx_batch *after = agx_get_compute_batch(agx_context(ctx));
   if (after != before || after->apple9_dispatch_count != count ||
       after->apple9_launch_next != launch ||
       after->apple9_resource_next != resource)
      fail("one-u32 SSBO4 underbind published work");
}

static void
run_formula(struct pipe_screen *screen, struct pipe_context *ctx,
            enum formula formula)
{
   struct oracle oracle;
   oracle_init(&oracle, formula);
   check_oracle(&oracle, formula);
   nir_shader *nir =
      build_shader(screen->nir_options[MESA_SHADER_COMPUTE], formula);
   if (screen->finalize_nir)
      screen->finalize_nir(screen, nir, true);
   struct pipe_compute_state cso = {
      .ir_type = PIPE_SHADER_IR_NIR,
      .prog = nir,
   };
   void *state = ctx->create_compute_state(ctx, &cso);
   if (!state)
      fail("compile SSBO4 device graph through Gallium");
   ctx->bind_compute_state(ctx, state);

   struct pipe_resource *resources[BUFFER_COUNT] = {0};
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      resources[b] = pipe_buffer_create(
         screen, PIPE_BIND_SHADER_BUFFER,
         b == BUFFER_OUTPUT ? PIPE_USAGE_DEFAULT : PIPE_USAGE_IMMUTABLE,
         BO_SIZE);
      if (!resources[b])
         fail("create SSBO4 guarded resource");
      pipe_buffer_write(ctx, resources[b], 0, BO_SIZE, oracle.seed[b]);
   }
   const struct pipe_grid_info grid = {
      .block = {LOCAL_SIZE, 1, 1},
      .grid = {GROUPS, 1, 1},
   };
   for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot)
      check_underbind(ctx, resources, &grid, slot);

   struct agx_batch *batch = agx_get_compute_batch(agx_context(ctx));
   if (!batch)
      fail("obtain SSBO4 valid batch");
   uint32_t before = batch->apple9_dispatch_count;
   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      set_bindings(ctx, resources, dispatch, -1);
      ctx->launch_grid(ctx, &grid);
   }
   struct agx_batch *queued = agx_get_compute_batch(agx_context(ctx));
   if (queued != batch || queued->apple9_dispatch_count != before + DISPATCHES)
      fail("SSBO4 valid dispatch accounting");
   finish(screen, ctx);
   verify(ctx, resources, &oracle);

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT, NULL, 0);
   ctx->bind_compute_state(ctx, NULL);
   ctx->delete_compute_state(ctx, state);
   for (unsigned b = 0; b < BUFFER_COUNT; ++b)
      pipe_resource_reference(&resources[b], NULL);
   printf("T8132_GALLIUM_COMPUTE_DEVICE_GRAPH_CASE_OK formula=%s "
          "dispatches=2 rejections=4 exact_full_bos=yes\n",
          formula_name(formula));
}

int
main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "--self-test"))
      return self_test();
   if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "--suite"))) {
      fprintf(stderr, "usage: %s [--self-test | --suite]\n", argv[0]);
      return 2;
   }

   struct pipe_loader_device *device = find_asahi_device();
   if (!device)
      fail("Asahi Gallium device not found");
   struct pipe_screen *screen = pipe_loader_create_screen(device, false);
   if (!screen)
      fail("create Asahi Gallium screen");
   const char *renderer = screen->get_name(screen);
   if (!renderer || !strstr(renderer, "Apple M4"))
      fail("unexpected Gallium renderer");
   char renderer_copy[128];
   snprintf(renderer_copy, sizeof(renderer_copy), "%s", renderer);
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   if (!ctx)
      fail("create Gallium context");

   run_formula(screen, ctx, FORMULA_CHAIN);
   run_formula(screen, ctx, FORMULA_REUSE2);

   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);
   printf("T8132_GALLIUM_COMPUTE_DEVICE_GRAPH_OK cases=2 dispatches=4 "
          "underbind_rejections=8 renderer=\"%s\"\n",
          renderer_copy);
   return 0;
}
