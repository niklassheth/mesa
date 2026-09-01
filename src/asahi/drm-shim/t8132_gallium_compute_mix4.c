/* SPDX-License-Identifier: MIT */

/*
 * Exact-output Gallium corpus for the four-SSBO integer package derived from
 * EXP-M4-26's native T8132 mix capture.  The byte-identical native case is:
 *
 *   out[i] = (a[i] * 3u + b[i]) ^ (c[i] + 0x55aa55aau)
 *
 * The package's native argument order is A, B, C, output.  Gallium compacts
 * that interface as output=0, C=1, B=2, A=3.  Every BO is checked in full:
 * all three inputs must remain immutable, both output ranges must be exact,
 * and poison-filled leading, trailing, and inter-dispatch gaps must survive.
 * Additional cases exercise the bounded integer compiler surface carried by
 * the same capture-backed package.
 */

#include "asahi/compiler/agx_compile.h"
#include "asahi/compiler/agx_compile_apple9.h"
#include "compiler/nir/nir_builder.h"
#include "gallium/drivers/asahi/agx_apple9.h"
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

#define ELEMENTS 256u
#define LOCAL_SIZE 32u
#define GROUPS (ELEMENTS / LOCAL_SIZE)
#define PAYLOAD_BYTES (ELEMENTS * sizeof(uint32_t))
#define BUFFER_BYTES 0x5000u
#define BUFFER_COUNT 4u
#define MAX_DISPATCHES 2u
#define MIX_LITERAL 0x55aa55aau
#define STATE_SELECTOR_OFFSET 0x20u
#define MIX4_LAUNCH_ALLOCATION 0x100u

enum buffer_index {
   BUFFER_A = 0,
   BUFFER_B,
   BUFFER_C,
   BUFFER_OUTPUT,
};

enum formula {
   FORMULA_NATIVE_MIX,
   FORMULA_ADD3,
   FORMULA_FANOUT,
   FORMULA_XOR3,
   FORMULA_LOGIC_ADD,
   FORMULA_SHIFT_ADD,
   FORMULA_LOGIC_SHIFT,
   FORMULA_MINMAX,
   FORMULA_DIRECT_A,
   FORMULA_DIRECT_B,
   FORMULA_DIRECT_C,
   FORMULA_C_MUL3,
   FORMULA_C_MIN,
   FORMULA_COUNT,
};

static const char *
formula_name(enum formula formula)
{
   static const char *const names[FORMULA_COUNT] = {
      [FORMULA_NATIVE_MIX] = "native-mix",
      [FORMULA_ADD3] = "add3",
      [FORMULA_FANOUT] = "fanout",
      [FORMULA_XOR3] = "xor3",
      [FORMULA_LOGIC_ADD] = "logic-add",
      [FORMULA_SHIFT_ADD] = "shift-add",
      [FORMULA_LOGIC_SHIFT] = "logic-shift",
      [FORMULA_MINMAX] = "minmax",
      [FORMULA_DIRECT_A] = "direct-a",
      [FORMULA_DIRECT_B] = "direct-b",
      [FORMULA_DIRECT_C] = "direct-c",
      [FORMULA_C_MUL3] = "c-mul3",
      [FORMULA_C_MIN] = "c-min",
   };
   return formula < FORMULA_COUNT ? names[formula] : "invalid";
}

/* Every dispatch and resource deliberately starts at a distinct, nonzero
 * offset.  The two ranges in each BO leave a large poison-filled interior
 * gap, while BUFFER_BYTES also leaves leading and trailing guards. */
static const size_t payload_offsets[MAX_DISPATCHES][BUFFER_COUNT] = {
   {0x0140u, 0x0780u, 0x0f40u, 0x17c0u},
   {0x2940u, 0x3100u, 0x38c0u, 0x4140u},
};

/* At least one input traverses this corpus without transformation.  The
 * values cover carry/borrow edges, multiplication overflow, signed extrema,
 * sparse bits, alternating patterns, byte patterns, and familiar sentinels. */
static const uint32_t adversarial_words[] = {
   0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u,
   0x00000004u, 0x00000007u, 0x00000008u, 0x0000000fu,
   0x00000010u, 0x0000001fu, 0x00000020u, 0x0000003fu,
   0x00000040u, 0x0000007fu, 0x00000080u, 0x000000ffu,
   0x00000100u, 0x000001ffu, 0x00000200u, 0x000003ffu,
   0x00000400u, 0x00007fffu, 0x00008000u, 0x0000ffffu,
   0x00010000u, 0xffff0000u, 0x00ff00ffu, 0xff00ff00u,
   0x55555555u, 0xaaaaaaaau, 0x33333333u, 0xccccccccu,
   0x0f0f0f0fu, 0xf0f0f0f0u, 0x01010101u, 0x80808080u,
   0x7f7f7f7fu, 0x80000000u, 0x7fffffffu, 0xffffffffu,
   0xfffffffeu, 0xfffffffdu, 0x40000000u, 0xc0000000u,
   0x3fffffffu, 0xbfffffffu, 0x55555556u, 0xaaaaaaa9u,
   0xdeadbeefu, 0xcafebabeu, 0x01234567u, 0x89abcdefu,
   0x10203040u, 0x55667788u, 0x55aa55aau, 0xaa55aa55u,
   0x00010001u, 0x10000001u, 0x01000000u, 0x00000101u,
   0x80000001u, 0x7ffffffeu, 0x6d2b79f5u, 0x9e3779b9u,
};

_Static_assert(ARRAY_SIZE(adversarial_words) == 64,
               "mix4 base corpus must retain 64 adversarial words");
_Static_assert(ELEMENTS >= ARRAY_SIZE(adversarial_words),
               "mix4 needs at least one full adversarial wave");
_Static_assert(ELEMENTS % LOCAL_SIZE == 0,
               "mix4 payload must contain whole workgroups");

struct oracle {
   uint8_t seed[BUFFER_COUNT][BUFFER_BYTES];
   uint8_t expected[BUFFER_COUNT][BUFFER_BYTES];
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_MIX4_FAIL: %s\n", message);
   exit(1);
}

static uint32_t
rotate_left(uint32_t value, unsigned amount)
{
   amount &= 31;
   return amount ? (value << amount) | (value >> (32 - amount)) : value;
}

static uint8_t
poison_byte(unsigned buffer, size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0xa5c39e17u + buffer * 0x31415927u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static uint32_t
input_word(unsigned dispatch, unsigned buffer, unsigned lane)
{
   if (dispatch >= MAX_DISPATCHES || buffer > BUFFER_C || lane >= ELEMENTS)
      fail("invalid mix4 input coordinate");

   uint32_t value;
   switch (buffer) {
   case BUFFER_A:
      /* The first wave preserves all 64 hand-picked words exactly.  Later
       * waves transform them so the 256-invocation gate is not repetition. */
      value = adversarial_words[lane % ARRAY_SIZE(adversarial_words)];
      if (lane >= ARRAY_SIZE(adversarial_words)) {
         unsigned wave = lane / ARRAY_SIZE(adversarial_words);
         value = rotate_left(value ^ (0x9e3779b9u * wave),
                             (lane + wave * 5u) & 31u);
      }
      break;
   case BUFFER_B:
      value = adversarial_words[
         (lane * 13u + 7u) % ARRAY_SIZE(adversarial_words)];
      value = rotate_left(value, (lane % 15u) + 1u) ^ 0x13579bdfu;
      break;
   case BUFFER_C:
      value = adversarial_words[
         (lane * 29u + 11u) % ARRAY_SIZE(adversarial_words)];
      value = rotate_left(~value, (lane % 11u) + 3u) + 0x2468ace1u;
      break;
   default:
      fail("invalid mix4 input resource");
      return 0;
   }

   if (dispatch)
      value = rotate_left(value ^ (0x6d2b79f5u + buffer * 0x11111111u),
                          5u + buffer * 3u);
   return value;
}

static uint32_t
formula_value(enum formula formula, uint32_t a, uint32_t b, uint32_t c)
{
   switch (formula) {
   case FORMULA_NATIVE_MIX:
      return (a * 3u + b) ^ (c + MIX_LITERAL);
   case FORMULA_ADD3:
      return a + b + c;
   case FORMULA_FANOUT:
      return (a + b) ^ (a + c) ^ (b + c);
   case FORMULA_XOR3:
      return a ^ b ^ c;
   case FORMULA_LOGIC_ADD:
      return (a ^ c) + b;
   case FORMULA_SHIFT_ADD:
      return (a << 7) + b + c;
   case FORMULA_LOGIC_SHIFT:
      return (a & b) | ((a ^ c) << 7);
   case FORMULA_MINMAX: {
      uint32_t imin_ab = (int32_t)a < (int32_t)b ? a : b;
      uint32_t imax_bc = (int32_t)b > (int32_t)c ? b : c;
      uint32_t umin_ac = a < c ? a : c;
      uint32_t umax_ab = a > b ? a : b;
      return imin_ab ^ imax_bc ^ umin_ac ^ umax_ab;
   }
   case FORMULA_DIRECT_A:
      return a;
   case FORMULA_DIRECT_B:
      return b;
   case FORMULA_DIRECT_C:
      return c;
   case FORMULA_C_MUL3:
      return c * 3u;
   case FORMULA_C_MIN:
      return c < a ? c : a;
   case FORMULA_COUNT:
      break;
   }
   fail("invalid four-SSBO formula");
   return 0;
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

static void
oracle_init(struct oracle *oracle, unsigned dispatch_count,
            enum formula formula)
{
   if (!oracle || dispatch_count == 0 || dispatch_count > MAX_DISPATCHES)
      fail("invalid mix4 oracle dispatch count");

   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer) {
      for (size_t offset = 0; offset < BUFFER_BYTES; ++offset)
         oracle->seed[buffer][offset] = poison_byte(buffer, offset);
      memcpy(oracle->expected[buffer], oracle->seed[buffer], BUFFER_BYTES);
   }

   /* Seed both dispatch input ranges even in one-dispatch mode.  The unused
    * range then becomes additional immutable interior-gap coverage. */
   for (unsigned dispatch = 0; dispatch < MAX_DISPATCHES; ++dispatch) {
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         for (unsigned buffer = BUFFER_A; buffer <= BUFFER_C; ++buffer) {
            write_word(oracle->seed[buffer],
                       payload_offsets[dispatch][buffer] +
                          lane * sizeof(uint32_t),
                       input_word(dispatch, buffer, lane));
         }
      }
   }

   /* Inputs are immutable, including both initialized payloads. */
   for (unsigned buffer = BUFFER_A; buffer <= BUFFER_C; ++buffer)
      memcpy(oracle->expected[buffer], oracle->seed[buffer], BUFFER_BYTES);

   for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t a = input_word(dispatch, BUFFER_A, lane);
         uint32_t b = input_word(dispatch, BUFFER_B, lane);
         uint32_t c = input_word(dispatch, BUFFER_C, lane);
         write_word(oracle->expected[BUFFER_OUTPUT],
                    payload_offsets[dispatch][BUFFER_OUTPUT] +
                       lane * sizeof(uint32_t),
                    formula_value(formula, a, b, c));
      }
   }
}

static void
check_oracle(enum formula formula)
{
   struct oracle one, two;
   oracle_init(&one, 1, formula);
   oracle_init(&two, 2, formula);

   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer) {
      size_t first_end = payload_offsets[0][buffer] + PAYLOAD_BYTES;
      size_t second = payload_offsets[1][buffer];
      size_t second_end = second + PAYLOAD_BYTES;
      if (!payload_offsets[0][buffer] || !second ||
          (payload_offsets[0][buffer] & 3) || (second & 3) ||
          first_end >= second || second_end >= BUFFER_BYTES)
         fail("guarded mix4 range layout invariant");

      for (size_t offset = 0; offset < BUFFER_BYTES; ++offset) {
         if (!poison_byte(buffer, offset))
            fail("mix4 poison must be nonzero");
      }
   }

   for (unsigned buffer = BUFFER_A; buffer <= BUFFER_C; ++buffer) {
      if (memcmp(one.seed[buffer], one.expected[buffer], BUFFER_BYTES) != 0 ||
          memcmp(two.seed[buffer], two.expected[buffer], BUFFER_BYTES) != 0)
         fail("mix4 CPU oracle mutated an input allocation");
      if (memcmp(one.seed[buffer], two.seed[buffer], BUFFER_BYTES) != 0)
         fail("mix4 input initialization depends on dispatch count");
   }

   const size_t second_output = payload_offsets[1][BUFFER_OUTPUT];
   if (memcmp(one.seed[BUFFER_OUTPUT] + second_output,
              one.expected[BUFFER_OUTPUT] + second_output,
              PAYLOAD_BYTES) != 0 ||
       memcmp(two.seed[BUFFER_OUTPUT] + second_output,
              two.expected[BUFFER_OUTPUT] + second_output,
              PAYLOAD_BYTES) == 0)
      fail("two-dispatch output oracle does not distinguish dispatch two");

   bool dispatch_data_differs[3] = {false, false, false};
   bool dispatch_outputs_differ = false;
   for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
      uint32_t values[MAX_DISPATCHES][3];
      for (unsigned dispatch = 0; dispatch < MAX_DISPATCHES; ++dispatch) {
         for (unsigned buffer = BUFFER_A; buffer <= BUFFER_C; ++buffer) {
            values[dispatch][buffer] =
               input_word(dispatch, buffer, lane);
            dispatch_data_differs[buffer] |=
               values[0][buffer] != values[dispatch][buffer];
         }
         uint32_t expected = formula_value(
            formula, values[dispatch][BUFFER_A],
            values[dispatch][BUFFER_B], values[dispatch][BUFFER_C]);
         uint32_t actual = read_word(
            two.expected[BUFFER_OUTPUT],
            payload_offsets[dispatch][BUFFER_OUTPUT] +
               lane * sizeof(uint32_t));
         if (actual != expected)
            fail("mix4 CPU formula oracle mismatch");
      }
      dispatch_outputs_differ |=
         read_word(two.expected[BUFFER_OUTPUT],
                   payload_offsets[0][BUFFER_OUTPUT] +
                      lane * sizeof(uint32_t)) !=
         read_word(two.expected[BUFFER_OUTPUT],
                   payload_offsets[1][BUFFER_OUTPUT] +
                      lane * sizeof(uint32_t));
   }
   if (!dispatch_data_differs[BUFFER_A] ||
       !dispatch_data_differs[BUFFER_B] ||
       !dispatch_data_differs[BUFFER_C] || !dispatch_outputs_differ)
      fail("mix4 dispatch corpora are not distinguishable");
}

static nir_shader *
build_shader(const nir_shader_compiler_options *options, enum formula formula)
{
   nir_builder n = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "t8132_mix4_u32");
   n.shader->info.workgroup_size[0] = LOCAL_SIZE;
   n.shader->info.workgroup_size[1] = 1;
   n.shader->info.workgroup_size[2] = 1;
   n.shader->info.num_ssbos = BUFFER_COUNT;

   nir_def *gid = nir_channel(
      &n, nir_load_global_invocation_id(&n, 32), 0);
   nir_def *offset = nir_imul_imm(&n, gid, sizeof(uint32_t));
   nir_def *a = nir_load_ssbo(
      &n, 1, 32, nir_imm_int(&n, 3), offset);
   nir_def *b = nir_load_ssbo(
      &n, 1, 32, nir_imm_int(&n, 2), offset);
   nir_def *c = nir_load_ssbo(
      &n, 1, 32, nir_imm_int(&n, 1), offset);
   nir_def *result = NULL;
   switch (formula) {
   case FORMULA_NATIVE_MIX: {
      nir_def *lhs = nir_iadd(&n, nir_imul_imm(&n, a, 3), b);
      nir_def *rhs = nir_iadd(&n, c, nir_imm_int(&n, MIX_LITERAL));
      result = nir_ixor(&n, lhs, rhs);
      break;
   }
   case FORMULA_ADD3:
      result = nir_iadd(&n, nir_iadd(&n, a, b), c);
      break;
   case FORMULA_FANOUT:
      result = nir_ixor(
         &n, nir_ixor(&n, nir_iadd(&n, a, b), nir_iadd(&n, a, c)),
         nir_iadd(&n, b, c));
      break;
   case FORMULA_XOR3:
      result = nir_ixor(&n, nir_ixor(&n, a, b), c);
      break;
   case FORMULA_LOGIC_ADD:
      result = nir_iadd(&n, nir_ixor(&n, a, c), b);
      break;
   case FORMULA_SHIFT_ADD:
      result = nir_iadd(
         &n, nir_iadd(&n, nir_ishl_imm(&n, a, 7), b), c);
      break;
   case FORMULA_LOGIC_SHIFT:
      result = nir_ior(
         &n, nir_iand(&n, a, b),
         nir_ishl_imm(&n, nir_ixor(&n, a, c), 7));
      break;
   case FORMULA_MINMAX:
      result = nir_ixor(
         &n, nir_ixor(&n, nir_imin(&n, a, b), nir_imax(&n, b, c)),
         nir_ixor(&n, nir_umin(&n, a, c), nir_umax(&n, a, b)));
      break;
   case FORMULA_DIRECT_A:
      result = a;
      break;
   case FORMULA_DIRECT_B:
      result = b;
      break;
   case FORMULA_DIRECT_C:
      result = c;
      break;
   case FORMULA_C_MUL3:
      result = nir_imul_imm(&n, c, 3);
      break;
   case FORMULA_C_MIN:
      result = nir_umin(&n, c, a);
      break;
   case FORMULA_COUNT:
      fail("invalid four-SSBO shader formula");
      return NULL;
   }
   if (!result) {
      fail("unhandled four-SSBO shader formula");
      return NULL;
   }
   nir_store_ssbo(&n, result, nir_imm_int(&n, 0), offset,
                  .write_mask = 1, .align_mul = sizeof(uint32_t));

   nir_shader_gather_info(n.shader, nir_shader_get_entrypoint(n.shader));
   n.shader->info.num_ssbos = BUFFER_COUNT;
   nir_validate_shader(n.shader, "T8132 four-SSBO mix fixture");
   return n.shader;
}

static void
check_profile(const struct agx_apple9_compute_profile *profile)
{
   static const unsigned expected_binding[BUFFER_COUNT] = {3, 2, 1, 0};

   if (!profile ||
       profile->abi != AGX_APPLE9_COMPUTE_ABI_SSBO4_INTEGER_U32 ||
       profile->local_size[0] != LOCAL_SIZE ||
       profile->local_size[1] != 1 || profile->local_size[2] != 1 ||
       profile->index_rank != 1 || profile->index_stride[0] != 1 ||
       profile->index_stride[1] != 0 || profile->index_stride[2] != 0 ||
       agx_apple9_compute_resource_count(profile) != BUFFER_COUNT ||
       agx_apple9_compute_read_mask(profile) != 0x7 ||
       agx_apple9_compute_write_mask(profile) != 0x8 ||
       !agx_apple9_compute_has_dynamic_state(profile) ||
       agx_apple9_compute_state_uniform_base(profile) !=
          AGX_APPLE9_SSBO4_STATE_UNIFORM_BASE ||
       agx_apple9_compute_state_literal_capacity(profile) !=
          AGX_APPLE9_SSBO4_STATE_LITERAL_CAPACITY ||
       profile->state_literal_count != 0 ||
       agx_apple9_compute_launch_size(profile) != MIX4_LAUNCH_ALLOCATION ||
       agx_apple9_compute_archive_call_offset(profile) != 0x54)
      fail("four-SSBO package profile invariant");

   for (unsigned argument = 0; argument < BUFFER_COUNT; ++argument) {
      if (agx_apple9_compute_resource_binding(profile, argument) !=
          expected_binding[argument])
         fail("four-SSBO argument-to-compact-slot mapping invariant");
   }
}

static void
check_state_image(const uint8_t state[AGX_APPLE9_COMPUTE_STATE_STRIDE])
{
   uint8_t expected[AGX_APPLE9_COMPUTE_STATE_STRIDE] = {
      AGX_APPLE9_COMPUTE_STATE_STRIDE,
   };
   if (memcmp(state, expected, sizeof(expected)) != 0)
      fail("four-SSBO dynamic-state image invariant");
}

static int
self_test(void)
{
   check_oracle(FORMULA_NATIVE_MIX);
   check_oracle(FORMULA_ADD3);
   check_oracle(FORMULA_FANOUT);
   check_oracle(FORMULA_XOR3);
   check_oracle(FORMULA_LOGIC_ADD);
   check_oracle(FORMULA_SHIFT_ADD);
   check_oracle(FORMULA_LOGIC_SHIFT);
   check_oracle(FORMULA_MINMAX);
   check_oracle(FORMULA_DIRECT_A);
   check_oracle(FORMULA_DIRECT_B);
   check_oracle(FORMULA_DIRECT_C);
   check_oracle(FORMULA_C_MUL3);
   check_oracle(FORMULA_C_MIN);

   uint32_t native_binary_size = 0;
   for (enum formula formula = 0; formula < FORMULA_COUNT; ++formula) {
      nir_shader *nir = build_shader(&agx_nir_options, formula);
      struct agx_shader_part part = {0};
      struct agx_apple9_compute_profile profile = {0};
      const char *reason = NULL;
      if (!agx_compile_apple9_tiny(nir, &part, &profile, &reason)) {
         fprintf(stderr, "%s compile failed: %s\n", formula_name(formula),
                 reason ? reason : "no diagnostic");
         fail("compile four-SSBO corpus in offline self-test");
      }
      if (!part.binary || !part.info.binary_size)
         fail("four-SSBO compiler produced an empty main");
      check_profile(&profile);

      uint8_t state[AGX_APPLE9_COMPUTE_STATE_STRIDE];
      memset(state, 0x5a, sizeof(state));
      if (!agx_apple9_build_compute_state(state, sizeof(state), &profile))
         fail("build four-SSBO state in offline self-test");
      check_state_image(state);

      if (formula == FORMULA_NATIVE_MIX)
         native_binary_size = part.info.binary_size;
      free(part.binary);
      ralloc_free(nir);
   }

   printf("T8132_GALLIUM_COMPUTE_MIX4_SELF_TEST_OK "
          "native_bytes=%u formulas=%u buffers=4 elements=%u local=32 "
          "mapping=3,2,1,0 "
          "masks=0x7/0x8 launch=0x100 state_literals=0\n",
          native_binary_size, FORMULA_COUNT, ELEMENTS);
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
          strcmp(devices[i]->driver_name, "asahi") == 0) {
         selected = devices[i];
      } else {
         pipe_loader_release(&devices[i], 1);
      }
   }
   free(devices);
   return selected;
}

static struct agx_compiled_shader *
compiled_shader_from_state(void *state)
{
   struct agx_uncompiled_shader *shader = state;
   struct hash_entry *entry =
      _mesa_hash_table_next_entry(shader->variants, NULL);
   if (!entry)
      fail("mix4 state has no compiled variant");
   return entry->data;
}

static void
check_compiled_state(const struct agx_compiled_shader *compiled,
                     enum formula formula)
{
   if (!compiled || !compiled->apple9_tiny ||
       !compiled->apple9_state_bo || !compiled->apple9_state_address)
      fail("Gallium mix4 lacks persistent dynamic state");
   check_profile(&compiled->apple9_compute_profile);

   uint64_t slab_base = compiled->apple9_state_bo->va->addr;
   if (compiled->apple9_state_address < slab_base + STATE_SELECTOR_OFFSET)
      fail("mix4 state selector precedes its slab");
   uint64_t record_offset = compiled->apple9_state_address - slab_base -
                            STATE_SELECTOR_OFFSET;
   if ((record_offset & (AGX_APPLE9_COMPUTE_STATE_STRIDE - 1)) ||
       record_offset + AGX_APPLE9_COMPUTE_STATE_STRIDE >
          compiled->apple9_state_bo->size)
      fail("mix4 state selector does not name a complete aligned record");
   check_state_image((const uint8_t *)agx_bo_map(compiled->apple9_state_bo) +
                     record_offset);
}

static const char *
buffer_role(unsigned buffer)
{
   static const char *const roles[BUFFER_COUNT] = {
      "input-a", "input-b", "input-c", "output",
   };
   return buffer < BUFFER_COUNT ? roles[buffer] : "unknown";
}

static const char *
buffer_region(unsigned buffer, size_t offset)
{
   for (unsigned dispatch = 0; dispatch < MAX_DISPATCHES; ++dispatch) {
      size_t start = payload_offsets[dispatch][buffer];
      if (offset >= start && offset < start + PAYLOAD_BYTES)
         return dispatch ? "dispatch-1-payload" : "dispatch-0-payload";
   }
   return "guard-or-interior-gap";
}

static void
verify_results(const struct oracle *oracle,
               const uint8_t actual[BUFFER_COUNT][BUFFER_BYTES])
{
   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer) {
      if (memcmp(actual[buffer], oracle->expected[buffer],
                 BUFFER_BYTES) == 0)
         continue;

      for (size_t offset = 0; offset < BUFFER_BYTES; ++offset) {
         if (actual[buffer][offset] == oracle->expected[buffer][offset])
            continue;
         size_t word_offset = offset & ~(sizeof(uint32_t) - 1);
         fprintf(stderr,
                 "T8132_GALLIUM_COMPUTE_MIX4_FAIL: first differing byte "
                 "%s %s offset=%#zx actual=%#x expected=%#x "
                 "actual_word=%#x expected_word=%#x\n",
                 buffer_role(buffer), buffer_region(buffer, offset), offset,
                 actual[buffer][offset], oracle->expected[buffer][offset],
                 read_word(actual[buffer], word_offset),
                 read_word(oracle->expected[buffer], word_offset));
         exit(1);
      }
      fail("mix4 mismatch diagnostic found no differing byte");
   }
}

static void
run_formula(struct pipe_screen *screen, struct pipe_context *ctx,
            enum formula formula, unsigned dispatch_count,
            const char *renderer)
{
   struct oracle oracle;
   oracle_init(&oracle, dispatch_count, formula);

   nir_shader *nir =
      build_shader(screen->nir_options[MESA_SHADER_COMPUTE], formula);
   if (screen->finalize_nir)
      screen->finalize_nir(screen, nir, true);
   struct pipe_compute_state cso = {
      .ir_type = PIPE_SHADER_IR_NIR,
      .prog = nir,
   };
   void *state = ctx->create_compute_state(ctx, &cso);
   /* Asahi consumes the NIR passed to create_compute_state. */
   nir = NULL;
   if (!state)
      fail("compile mix4 through Gallium");

   struct agx_compiled_shader *compiled =
      compiled_shader_from_state(state);
   check_compiled_state(compiled, formula);
   uint64_t state_selector = compiled->apple9_state_address;
   ctx->bind_compute_state(ctx, state);

   struct pipe_resource *resources[BUFFER_COUNT] = {0};
   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer) {
      enum pipe_resource_usage usage =
         buffer == BUFFER_OUTPUT ? PIPE_USAGE_DEFAULT : PIPE_USAGE_IMMUTABLE;
      resources[buffer] = pipe_buffer_create(
         screen, PIPE_BIND_SHADER_BUFFER, usage, BUFFER_BYTES);
      if (!resources[buffer])
         fail("create guarded mix4 shader buffer");
      pipe_buffer_write(ctx, resources[buffer], 0, BUFFER_BYTES,
                        oracle.seed[buffer]);
   }

   static const unsigned compact_slot_to_buffer[BUFFER_COUNT] = {
      BUFFER_OUTPUT, BUFFER_C, BUFFER_B, BUFFER_A,
   };
   struct pipe_shader_buffer bindings[BUFFER_COUNT] = {0};
   const struct pipe_grid_info grid = {
      .block = {LOCAL_SIZE, 1, 1},
      .grid = {GROUPS, 1, 1},
   };

   /* Both launches use this one compiled state and are enqueued before the
    * sole explicit flush.  Changing all four offsets between them exercises
    * two launch allocations and two resource records in one compute batch. */
   for (unsigned dispatch = 0; dispatch < dispatch_count; ++dispatch) {
      for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot) {
         unsigned buffer = compact_slot_to_buffer[slot];
         bindings[slot] = (struct pipe_shader_buffer){
            .buffer = resources[buffer],
            .buffer_offset = payload_offsets[dispatch][buffer],
            .buffer_size = PAYLOAD_BYTES,
         };
      }
      ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT,
                              bindings, BITFIELD_BIT(0));
      ctx->launch_grid(ctx, &grid);
   }

   struct agx_batch *queued = agx_get_compute_batch(agx_context(ctx));
   uint32_t expected_launch_next = AGX_APPLE9_COMPUTE_LAUNCH_OFFSET +
                                   dispatch_count *
                                      MIX4_LAUNCH_ALLOCATION;
   uint32_t expected_resource_next =
      AGX_APPLE9_COMPUTE_RESOURCE_OFFSET +
      AGX_APPLE9_COMPUTE_RESOURCE_TABLE_OFFSET +
      dispatch_count * AGX_APPLE9_COMPUTE_RESOURCE_STRIDE;
   if (!queued || queued->apple9_dispatch_count != dispatch_count ||
       queued->apple9_launch_next != expected_launch_next ||
       queued->apple9_resource_next != expected_resource_next)
      fail("mix4 dispatches did not remain in one correctly sized batch");

   struct pipe_fence_handle *fence = NULL;
   ctx->flush(ctx, &fence, PIPE_FLUSH_END_OF_FRAME);
   if (!fence ||
       !screen->fence_finish(screen, ctx, fence, OS_TIMEOUT_INFINITE))
      fail("wait for mix4 dispatch batch");
   screen->fence_reference(screen, &fence, NULL);

   uint8_t actual[BUFFER_COUNT][BUFFER_BYTES];
   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer) {
      struct pipe_transfer *transfer = NULL;
      const uint8_t *mapped = pipe_buffer_map_range(
         ctx, resources[buffer], 0, BUFFER_BYTES, PIPE_MAP_READ, &transfer);
      if (!mapped)
         fail("map full mix4 allocation for verification");
      memcpy(actual[buffer], mapped, BUFFER_BYTES);
      pipe_buffer_unmap(ctx, transfer);
   }
   verify_results(&oracle, actual);

   /* Compilation state is immutable across both publications. */
   if (compiled_shader_from_state(state) != compiled ||
       compiled->apple9_state_address != state_selector)
      fail("mix4 compiled state changed between dispatches");
   check_compiled_state(compiled, formula);

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT,
                           NULL, 0);
   ctx->bind_compute_state(ctx, NULL);
   ctx->delete_compute_state(ctx, state);
   for (unsigned buffer = 0; buffer < BUFFER_COUNT; ++buffer)
      pipe_resource_reference(&resources[buffer], NULL);

   printf("T8132_GALLIUM_COMPUTE_MIX4_OK formula=%s dispatches=%u batch=one "
          "compiled_states=1 exact_bytes=%#x inputs_immutable=yes "
          "guards_and_gaps=yes launch_stride=0x100 state=%#llx "
          "renderer=\"%s\"\n",
          formula_name(formula),
          dispatch_count, BUFFER_COUNT * BUFFER_BYTES,
          (unsigned long long)state_selector, renderer);
}

static void
usage(const char *program)
{
   fprintf(stderr,
           "usage: %s [--self-test | --suite | --two-dispatch] "
           "[--add3 | --fanout | --xor3 | --logic-add | --shift-add | "
           "--logic-shift | --minmax | --direct-a | --direct-b | "
           "--direct-c | --c-mul3 | --c-min]\n",
           program);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
      return self_test();

   bool two_dispatch = false;
   bool suite = false;
   enum formula formula = FORMULA_NATIVE_MIX;
   bool formula_selected = false;
   for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--two-dispatch") == 0) {
         two_dispatch = true;
         continue;
      }
      if (strcmp(argv[i], "--suite") == 0) {
         if (suite || formula_selected) {
            usage(argv[0]);
            return 2;
         }
         suite = true;
         continue;
      }

      enum formula selected = FORMULA_COUNT;
      if (strcmp(argv[i], "--add3") == 0)
         selected = FORMULA_ADD3;
      else if (strcmp(argv[i], "--fanout") == 0)
         selected = FORMULA_FANOUT;
      else if (strcmp(argv[i], "--xor3") == 0)
         selected = FORMULA_XOR3;
      else if (strcmp(argv[i], "--logic-add") == 0)
         selected = FORMULA_LOGIC_ADD;
      else if (strcmp(argv[i], "--shift-add") == 0)
         selected = FORMULA_SHIFT_ADD;
      else if (strcmp(argv[i], "--logic-shift") == 0)
         selected = FORMULA_LOGIC_SHIFT;
      else if (strcmp(argv[i], "--minmax") == 0)
         selected = FORMULA_MINMAX;
      else if (strcmp(argv[i], "--direct-a") == 0)
         selected = FORMULA_DIRECT_A;
      else if (strcmp(argv[i], "--direct-b") == 0)
         selected = FORMULA_DIRECT_B;
      else if (strcmp(argv[i], "--direct-c") == 0)
         selected = FORMULA_DIRECT_C;
      else if (strcmp(argv[i], "--c-mul3") == 0)
         selected = FORMULA_C_MUL3;
      else if (strcmp(argv[i], "--c-min") == 0)
         selected = FORMULA_C_MIN;

      if (selected == FORMULA_COUNT || formula_selected || suite) {
         usage(argv[0]);
         return 2;
      }
      formula = selected;
      formula_selected = true;
   }

   const unsigned dispatch_count = two_dispatch ? 2 : 1;
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

   if (suite) {
      for (enum formula current = 0; current < FORMULA_COUNT; ++current)
         run_formula(screen, ctx, current, dispatch_count, renderer);
   } else {
      run_formula(screen, ctx, formula, dispatch_count, renderer);
   }

   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   if (suite) {
      printf("T8132_GALLIUM_COMPUTE_MIX4_SUITE_OK formulas=%u "
             "dispatches_per_formula=%u publications=%u elements=%u "
             "local=%u renderer=\"%s\"\n",
             FORMULA_COUNT, dispatch_count, FORMULA_COUNT, ELEMENTS,
             LOCAL_SIZE, renderer);
   }
   return 0;
}
