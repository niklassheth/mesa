/* SPDX-License-Identifier: MIT */

/* Exact-output Gallium fixture for EXP-M4-26 index_permute:
 *
 *   idx = indices[i]; out[i] = data[idx]
 *   indices[i] = (13 * i + 7) & 63
 *
 * The shader is ordinary NIR compiled through Gallium.  No captured machine
 * code is embedded.  Two dispatches exercise a nonzero range base and a
 * range ending exactly at the end of each 4-KiB BO. */

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

#define ELEMENTS         64u
#define LOCAL_SIZE       32u
#define GROUPS           (ELEMENTS / LOCAL_SIZE)
#define PAYLOAD_SIZE     (ELEMENTS * sizeof(uint32_t))
#define ADD1_DATA_SIZE   ((ELEMENTS + 1u) * sizeof(uint32_t))
#define AFFINE_DATA_SIZE ((3u * (ELEMENTS - 1u) + 2u) * sizeof(uint32_t))
#define BO_SIZE          0x1000u
#define BUFFER_COUNT     3u
#define DISPATCHES       2u
#define RANGE_POISON     0xccu

enum buffer_index {
   BUFFER_INDICES = 0,
   BUFFER_DATA,
   BUFFER_OUTPUT,
};

enum formula {
   FORMULA_INDEX,
   FORMULA_REUSE_ALU,
   FORMULA_ADD1,
   FORMULA_AFFINE,
   FORMULA_U8_LOAD,
   FORMULA_I8_LOAD,
   FORMULA_U16_LOAD,
   FORMULA_I16_LOAD,
   FORMULA_U8_STORE,
   FORMULA_U16_STORE,
   FORMULA_VARIABLE_SHL,
   FORMULA_VARIABLE_ASHR,
   FORMULA_VARIABLE_USHR,
   FORMULA_UBO,
   FORMULA_PRESSURE40,
   FORMULA_COUNT,
};

static const char *
formula_name(enum formula formula)
{
   switch (formula) {
   case FORMULA_INDEX:
      return "index";
   case FORMULA_REUSE_ALU:
      return "reuse-alu";
   case FORMULA_ADD1:
      return "add1";
   case FORMULA_AFFINE:
      return "affine";
   case FORMULA_U8_LOAD:
      return "u8-load";
   case FORMULA_I8_LOAD:
      return "i8-load";
   case FORMULA_U16_LOAD:
      return "u16-load";
   case FORMULA_I16_LOAD:
      return "i16-load";
   case FORMULA_U8_STORE:
      return "u8-store";
   case FORMULA_U16_STORE:
      return "u16-store";
   case FORMULA_VARIABLE_SHL:
      return "variable-shl";
   case FORMULA_VARIABLE_ASHR:
      return "variable-ashr";
   case FORMULA_VARIABLE_USHR:
      return "variable-ushr";
   case FORMULA_UBO:
      return "ubo";
   case FORMULA_PRESSURE40:
      return "pressure40";
   case FORMULA_COUNT:
      break;
   }
   return "invalid";
}

static unsigned
data_bits(enum formula formula)
{
   switch (formula) {
   case FORMULA_U8_LOAD:
   case FORMULA_I8_LOAD:
      return 8;
   case FORMULA_U16_LOAD:
   case FORMULA_I16_LOAD:
      return 16;
   default:
      return 32;
   }
}

static unsigned
output_bits(enum formula formula)
{
   switch (formula) {
   case FORMULA_U8_STORE:
      return 8;
   case FORMULA_U16_STORE:
      return 16;
   default:
      return 32;
   }
}

static bool
signed_load(enum formula formula)
{
   return formula == FORMULA_I8_LOAD || formula == FORMULA_I16_LOAD;
}

static bool
narrow_formula(enum formula formula)
{
   return formula >= FORMULA_U8_LOAD && formula <= FORMULA_U16_STORE;
}

static bool
variable_shift_formula(enum formula formula)
{
   return formula >= FORMULA_VARIABLE_SHL &&
          formula <= FORMULA_VARIABLE_USHR;
}

static unsigned
access_size(enum formula formula, enum buffer_index buffer)
{
   switch (buffer) {
   case BUFFER_INDICES:
      return sizeof(uint32_t);
   case BUFFER_DATA:
      return data_bits(formula) / 8;
   case BUFFER_OUTPUT:
      return output_bits(formula) / 8;
   }
   abort();
}

/* Dispatch zero is deliberately unaligned to a page; affine additionally
 * separates all three bases. Dispatch one proves each exact end boundary. */
static const size_t payload_offsets[DISPATCHES] = {
   0x140u,
   BO_SIZE - PAYLOAD_SIZE,
};

static size_t
required_span(enum formula formula, enum buffer_index buffer)
{
   if (buffer == BUFFER_DATA) {
      if (formula == FORMULA_ADD1)
         return ADD1_DATA_SIZE;
      if (formula == FORMULA_AFFINE)
         return AFFINE_DATA_SIZE;
   }
   return ELEMENTS * access_size(formula, buffer);
}

static size_t
binding_offset(enum formula formula, unsigned dispatch,
               enum buffer_index buffer)
{
   if (dispatch == 0 && formula == FORMULA_AFFINE) {
      static const size_t affine_offsets[BUFFER_COUNT] = {
         [BUFFER_INDICES] = 0x140u,
         [BUFFER_DATA] = 0x540u,
         [BUFFER_OUTPUT] = 0xa40u,
      };
      return affine_offsets[buffer];
   }
   if (dispatch == 0)
      return payload_offsets[0];
   return BO_SIZE - required_span(formula, buffer);
}

struct oracle {
   uint8_t seed[BUFFER_COUNT][BO_SIZE];
   uint8_t expected[BUFFER_COUNT][BO_SIZE];
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_DEVICE_INDEX_FAIL: %s\n", message);
   exit(1);
}

static uint32_t
data_word(enum formula formula, unsigned dispatch, unsigned lane)
{
   uint32_t salt = dispatch ? 0x89abcdefu : 0x10203040u;
   if (formula == FORMULA_REUSE_ALU)
      salt ^= 0x6d2b79f5u;
   else if (formula == FORMULA_ADD1)
      salt ^= 0xb5297a4du;
   else if (formula == FORMULA_AFFINE)
      salt ^= 0x3c6ef372u;
   else if (formula == FORMULA_U8_LOAD)
      salt ^= 0x147be291u;
   else if (formula == FORMULA_I8_LOAD)
      salt ^= 0x9235a5d3u;
   else if (formula == FORMULA_U16_LOAD)
      salt ^= 0x6a09c5e7u;
   else if (formula == FORMULA_I16_LOAD)
      salt ^= 0xbb67a6f1u;
   else if (formula == FORMULA_U8_STORE)
      salt ^= 0x3c6ef4abu;
   else if (formula == FORMULA_U16_STORE)
      salt ^= 0xa54ff53au;
   else if (formula == FORMULA_VARIABLE_SHL)
      salt ^= 0x510e527fu;
   else if (formula == FORMULA_VARIABLE_ASHR)
      salt ^= 0x9b05688cu;
   else if (formula == FORMULA_VARIABLE_USHR)
      salt ^= 0x1f83d9abu;
   else if (formula == FORMULA_UBO)
      salt ^= 0xcbbb9d5du;
   else if (formula == FORMULA_PRESSURE40)
      salt ^= 0x5be0cd19u;
   uint32_t value =
      salt ^ (lane * (0x01010101u + dispatch * 0x00110011u));
   if (formula == FORMULA_I8_LOAD)
      value |= 0x80;
   else if (formula == FORMULA_I16_LOAD)
      value |= 0x8000;
   else if (formula == FORMULA_VARIABLE_ASHR)
      value |= 0x80000000u;
   return value;
}

static uint32_t
index_word(unsigned lane)
{
   return (13u * lane + 7u) & 63u;
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

static void
write_element(uint8_t *image, size_t offset, unsigned bits, uint32_t value)
{
   switch (bits) {
   case 8: {
      uint8_t narrowed = value;
      memcpy(image + offset, &narrowed, sizeof(narrowed));
      return;
   }
   case 16: {
      uint16_t narrowed = value;
      memcpy(image + offset, &narrowed, sizeof(narrowed));
      return;
   }
   case 32:
      write_word(image, offset, value);
      return;
   default:
      abort();
   }
}

static uint32_t
read_element(const uint8_t *image, size_t offset, unsigned bits, bool sign)
{
   switch (bits) {
   case 8: {
      uint8_t value;
      memcpy(&value, image + offset, sizeof(value));
      return sign ? (uint32_t)(int32_t)(int8_t)value : value;
   }
   case 16: {
      uint16_t value;
      memcpy(&value, image + offset, sizeof(value));
      return sign ? (uint32_t)(int32_t)(int16_t)value : value;
   }
   case 32:
      return read_word(image, offset);
   default:
      abort();
   }
}

static void
oracle_init(struct oracle *oracle, enum formula formula)
{
   uint8_t input_poison = formula == FORMULA_INDEX       ? RANGE_POISON
                          : formula == FORMULA_REUSE_ALU ? 0xa7
                          : formula == FORMULA_ADD1      ? 0x5b
                                                         : 0x39;
   memset(oracle, input_poison, sizeof(*oracle));
   memset(oracle->seed[BUFFER_OUTPUT], RANGE_POISON, BO_SIZE);
   memset(oracle->expected[BUFFER_OUTPUT], RANGE_POISON, BO_SIZE);

   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      size_t index_base = binding_offset(formula, dispatch, BUFFER_INDICES);
      size_t data_base = binding_offset(formula, dispatch, BUFFER_DATA);
      size_t output_base = binding_offset(formula, dispatch, BUFFER_OUTPUT);
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t index = index_word(lane);
         size_t byte = index_base + lane * sizeof(uint32_t);
         write_word(oracle->seed[BUFFER_INDICES], byte, index);
         write_word(oracle->expected[BUFFER_INDICES], byte, index);
      }

      unsigned data_elements = formula == FORMULA_ADD1 ? ELEMENTS + 1u
                               : formula == FORMULA_AFFINE
                                  ? 3u * (ELEMENTS - 1u) + 2u
                                  : ELEMENTS;
      for (unsigned lane = 0; lane < data_elements; ++lane) {
         uint32_t data = data_word(formula, dispatch, lane);
         size_t byte = data_base + lane * access_size(formula, BUFFER_DATA);
         write_element(oracle->seed[BUFFER_DATA], byte, data_bits(formula),
                       data);
         write_element(oracle->expected[BUFFER_DATA], byte,
                       data_bits(formula), data);
      }

      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t index = index_word(lane);
         unsigned data_index = formula == FORMULA_AFFINE
                                  ? 3u * index + 1u
                                  : index + (formula == FORMULA_ADD1);
         uint32_t value = 0;
         if (formula == FORMULA_PRESSURE40) {
            for (unsigned k = 0; k < 40; ++k)
               value += read_word(
                  oracle->seed[BUFFER_DATA],
                  data_base + ((index + k) & 63u) * sizeof(uint32_t));
         } else {
            value = read_element(
               oracle->seed[BUFFER_DATA],
               data_base + data_index * access_size(formula, BUFFER_DATA),
               data_bits(formula), signed_load(formula));
         }
         if (formula == FORMULA_REUSE_ALU)
            value ^= index;
         else if (formula == FORMULA_VARIABLE_SHL)
            value <<= index & 31;
         else if (formula == FORMULA_VARIABLE_ASHR)
            value = (uint32_t)((int32_t)value >> (index & 31));
         else if (formula == FORMULA_VARIABLE_USHR)
            value >>= index & 31;
         write_element(
            oracle->expected[BUFFER_OUTPUT],
            output_base + lane * access_size(formula, BUFFER_OUTPUT),
            output_bits(formula), value);
      }
   }
}

static void
check_oracle(const struct oracle *oracle, enum formula formula)
{
   if (memcmp(oracle->seed[BUFFER_INDICES], oracle->expected[BUFFER_INDICES],
              BO_SIZE) ||
       memcmp(oracle->seed[BUFFER_DATA], oracle->expected[BUFFER_DATA],
              BO_SIZE))
      fail("CPU oracle mutates an input image");

   bool permutation_seen[ELEMENTS] = {false};
   for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
      unsigned index = index_word(lane);
      if (index >= ELEMENTS || permutation_seen[index])
         fail("index pattern is not a bounded permutation");
      permutation_seen[index] = true;
   }

   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      size_t output_base = binding_offset(formula, dispatch, BUFFER_OUTPUT);

      if (narrow_formula(formula)) {
         unsigned changed = 0;
         bool saw_sign_extension = false;
         bool saw_truncation = false;
         for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
            unsigned index = index_word(lane);
            uint32_t source = read_element(
               oracle->seed[BUFFER_DATA],
               binding_offset(formula, dispatch, BUFFER_DATA) +
                  index * access_size(formula, BUFFER_DATA),
               data_bits(formula), signed_load(formula));
            uint32_t actual = read_element(
               oracle->expected[BUFFER_OUTPUT],
               output_base + lane * access_size(formula, BUFFER_OUTPUT),
               output_bits(formula), false);
            uint32_t narrowed =
               output_bits(formula) == 32
                  ? source
                  : source & BITFIELD_MASK(output_bits(formula));
            if (actual != narrowed)
               fail("narrow scalar CPU formula mismatch");
            changed += actual !=
                       (output_bits(formula) == 32
                           ? (uint32_t)(RANGE_POISON * 0x01010101u)
                           : RANGE_POISON & BITFIELD_MASK(output_bits(formula)));
            saw_sign_extension |=
               signed_load(formula) && (source & 0x80000000u) != 0;
            saw_truncation |= output_bits(formula) < 32 && source != narrowed;
         }
         if (changed == 0 || (signed_load(formula) && !saw_sign_extension) ||
             (output_bits(formula) < 32 && !saw_truncation)) {
            fprintf(stderr,
                    "formula=%s changed=%u sign=%u trunc=%u data_bits=%u "
                    "output_bits=%u\n",
                    formula_name(formula), changed, saw_sign_extension,
                    saw_truncation, data_bits(formula), output_bits(formula));
            fail("narrow scalar oracle lacks sensitivity");
         }
         continue;
      }

      if (variable_shift_formula(formula)) {
         unsigned shifted_lanes = 0;
         for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
            unsigned index = index_word(lane);
            uint32_t source = data_word(formula, dispatch, index);
            uint32_t expected =
               formula == FORMULA_VARIABLE_SHL
                  ? source << (index & 31)
               : formula == FORMULA_VARIABLE_ASHR
                  ? (uint32_t)((int32_t)source >> (index & 31))
                  : source >> (index & 31);
            uint32_t actual = read_word(
               oracle->expected[BUFFER_OUTPUT], output_base + lane * 4u);
            if (actual != expected)
               fail("variable-shift CPU formula mismatch");
            shifted_lanes += actual != source;
         }
         if (shifted_lanes < ELEMENTS / 2)
            fail("variable-shift oracle lacks sensitivity");
         continue;
      }
      if (formula == FORMULA_PRESSURE40) {
         unsigned differs_from_single = 0;
         for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
            unsigned index = index_word(lane);
            uint32_t expected = 0;
            for (unsigned k = 0; k < 40; ++k)
               expected += data_word(formula, dispatch, (index + k) & 63u);
            uint32_t actual = read_word(
               oracle->expected[BUFFER_OUTPUT], output_base + lane * 4u);
            if (actual != expected)
               fail("pressure40 CPU formula mismatch");
            differs_from_single +=
               actual != data_word(formula, dispatch, index);
         }
         if (differs_from_single != ELEMENTS)
            fail("pressure40 oracle can false-pass as one load");
         continue;
      }

      unsigned direct_mismatches = 0;
      unsigned base_mismatches = 0;
      unsigned direct_reuse_index_mismatches = 0;
      unsigned direct_reuse_gid_mismatches = 0;
      unsigned direct_add1_mismatches = 0;
      unsigned direct_affine_mismatches = 0;
      unsigned affine_base_mismatches = 0;
      unsigned affine_mul_mismatches = 0;
      unsigned affine_add1_mismatches = 0;
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         unsigned index = index_word(lane);
         uint32_t actual =
            read_word(oracle->expected[BUFFER_OUTPUT], output_base + lane * 4u);
         uint32_t indexed = data_word(formula, dispatch, index);
         uint32_t expected = formula == FORMULA_REUSE_ALU ? indexed ^ index
                             : formula == FORMULA_ADD1
                                ? data_word(formula, dispatch, index + 1u)
                             : formula == FORMULA_AFFINE
                                ? data_word(formula, dispatch, 3u * index + 1u)
                                : indexed;
         direct_mismatches += actual != data_word(formula, dispatch, lane);
         base_mismatches += actual != indexed;
         direct_reuse_index_mismatches +=
            actual != (data_word(formula, dispatch, lane) ^ index);
         direct_reuse_gid_mismatches +=
            actual != (data_word(formula, dispatch, lane) ^ lane);
         direct_add1_mismatches +=
            actual != data_word(formula, dispatch, lane + 1u);
         direct_affine_mismatches +=
            actual != data_word(formula, dispatch, 3u * lane + 1u);
         affine_base_mismatches += actual != indexed;
         affine_mul_mismatches +=
            actual != data_word(formula, dispatch, 3u * index);
         affine_add1_mismatches +=
            actual != data_word(formula, dispatch, index + 1u);
         if (actual != expected)
            fail("dependent-load CPU formula mismatch");
      }
      unsigned expected_direct_mismatches =
         formula == FORMULA_ADD1     ? ELEMENTS - 4u
         : formula == FORMULA_AFFINE ? ELEMENTS - 1u
                                     : ELEMENTS;
      if (direct_mismatches != expected_direct_mismatches)
         fail("oracle direct-gid discrimination changed");
      if (formula == FORMULA_REUSE_ALU && base_mismatches == 0)
         fail("reuse-alu oracle can false-pass as base index formula");
      if (formula == FORMULA_REUSE_ALU &&
          (direct_reuse_index_mismatches != ELEMENTS ||
           direct_reuse_gid_mismatches != ELEMENTS))
         fail("reuse-alu oracle can false-pass as direct gid data access");
      if (formula == FORMULA_ADD1 &&
          (base_mismatches != ELEMENTS || direct_add1_mismatches != ELEMENTS))
         fail("add1 oracle can false-pass without dependent index plus one");
      if (formula == FORMULA_AFFINE &&
          (direct_mismatches != ELEMENTS - 1u ||
           direct_affine_mismatches != ELEMENTS ||
           affine_base_mismatches != ELEMENTS ||
           affine_mul_mismatches != ELEMENTS ||
           affine_add1_mismatches != ELEMENTS - 1u))
         fail("affine oracle address-term discrimination changed");
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

   /* Apple9's compact Gallium contract is output=0, data=1, indices=2. */
   nir_def *index = nir_load_ssbo(&n, 1, 32, nir_imm_int(&n, 2), lane_offset,
                                  .align_mul = sizeof(uint32_t));
   /* The machine compiler cannot and must not infer a resource bound from
    * CPU-initialized contents.  Carry the semantic bound in NIR so arbitrary
    * runtime indices remain safe independently of the fixture's oracle. */
   index = nir_iand_imm(&n, index, ELEMENTS - 1u);
   nir_def *data_index = index;
   if (formula == FORMULA_ADD1)
      data_index = nir_iadd_imm(&n, index, 1);
   else if (formula == FORMULA_AFFINE)
      data_index = nir_iadd_imm(&n, nir_imul_imm(&n, index, 3), 1);
   unsigned input_bits = data_bits(formula);
   unsigned result_bits = output_bits(formula);
   nir_def *value;
   if (formula == FORMULA_PRESSURE40) {
      nir_def *loads[40];
      for (unsigned k = 0; k < ARRAY_SIZE(loads); ++k) {
         nir_def *load_index =
            nir_iand_imm(&n, nir_iadd_imm(&n, index, k), ELEMENTS - 1u);
         loads[k] = nir_load_ssbo(
            &n, 1, 32, nir_imm_int(&n, 1),
            nir_imul_imm(&n, load_index, sizeof(uint32_t)),
            .align_mul = sizeof(uint32_t));
      }
      value = loads[0];
      for (unsigned k = 1; k < ARRAY_SIZE(loads); ++k)
         value = nir_iadd(&n, value, loads[k]);
   } else if (formula == FORMULA_UBO) {
      nir_def *data_offset =
         nir_imul_imm(&n, data_index, sizeof(uint32_t));
      value = nir_load_ubo(&n, 1, 32, nir_imm_int(&n, 0), data_offset,
                           .align_mul = sizeof(uint32_t),
                           .range = ELEMENTS * sizeof(uint32_t));
   } else {
      nir_def *data_offset =
         nir_imul_imm(&n, data_index, input_bits / 8);
      value = nir_load_ssbo(&n, 1, input_bits, nir_imm_int(&n, 1), data_offset,
                            .align_mul = input_bits / 8);
   }
   if (input_bits < 32)
      value = signed_load(formula) ? nir_i2i32(&n, value)
                                   : nir_u2u32(&n, value);
   if (formula == FORMULA_REUSE_ALU)
      value = nir_ixor(&n, value, index);
   else if (formula == FORMULA_VARIABLE_SHL)
      value = nir_ishl(&n, value, index);
   else if (formula == FORMULA_VARIABLE_ASHR)
      value = nir_ishr(&n, value, index);
   else if (formula == FORMULA_VARIABLE_USHR)
      value = nir_ushr(&n, value, index);
   if (result_bits == 8)
      value = nir_u2u8(&n, value);
   else if (result_bits == 16)
      value = nir_u2u16(&n, value);
   nir_def *output_offset = nir_imul_imm(&n, gid, result_bits / 8);
   nir_store_ssbo(&n, value, nir_imm_int(&n, 0), output_offset,
                  .write_mask = 1, .align_mul = result_bits / 8);

   nir_shader_gather_info(n.shader, nir_shader_get_entrypoint(n.shader));
   n.shader->info.num_ssbos = BUFFER_COUNT;
   nir_validate_shader(n.shader, "T8132 dependent-load Gallium fixture");
   return n.shader;
}

static int
self_test(void)
{
   struct oracle oracles[FORMULA_COUNT];
   for (unsigned formula = 0; formula < FORMULA_COUNT; ++formula) {
      oracle_init(&oracles[formula], formula);
      check_oracle(&oracles[formula], formula);
   }

   for (unsigned left = 0; left < FORMULA_COUNT; ++left) {
      for (unsigned right = left + 1; right < FORMULA_COUNT; ++right) {
         /* Every case intentionally shares the same index permutation; only
          * the data and output images distinguish semantic formulas. */
         for (unsigned b = BUFFER_DATA; b < BUFFER_COUNT; ++b) {
            bool seeds_distinct =
               b == BUFFER_OUTPUT ||
               memcmp(oracles[left].seed[b], oracles[right].seed[b], BO_SIZE);
            if (!seeds_distinct ||
                !memcmp(oracles[left].expected[b], oracles[right].expected[b],
                        BO_SIZE)) {
               fprintf(stderr, "left=%s right=%s buffer=%u seeds_distinct=%u\n",
                       formula_name(left), formula_name(right), b,
                       seeds_distinct);
               fail("device-index formula full images are not distinct");
            }
         }
      }
   }

   for (unsigned formula = 0; formula < FORMULA_COUNT; ++formula) {
      for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
         if (binding_offset((enum formula)formula, 0, (enum buffer_index)b) ==
                0 ||
             binding_offset((enum formula)formula, 0, (enum buffer_index)b) +
                   required_span((enum formula)formula, (enum buffer_index)b) >=
                BO_SIZE ||
             binding_offset((enum formula)formula, 1, (enum buffer_index)b) +
                   required_span((enum formula)formula, (enum buffer_index)b) !=
                BO_SIZE)
            fail("binding-offset coverage invariant");
      }
   }

   printf("T8132_GALLIUM_COMPUTE_DEVICE_INDEX_SELF_TEST_OK "
          "cases=index,reuse-alu,add1,affine,u8-load,i8-load,u16-load,"
          "i16-load,u8-store,u16-store,variable-shl,variable-ashr,"
          "variable-ushr,ubo,pressure40 elements=64 local=32 "
          "dispatches=2 offsets=nonzero,end data_add1=0x104 "
          "data_affine=0x2fc "
          "full_bo=0x1000 guard=0xcc\n");
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
      fail("wait for dependent-load dispatches");
   screen->fence_reference(screen, &fence, NULL);
}

static void
verify_resources(struct pipe_context *ctx,
                 struct pipe_resource *resources[BUFFER_COUNT],
                 const struct oracle *oracle)
{
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      struct pipe_transfer *transfer = NULL;
      const uint8_t *map = pipe_buffer_map_range(ctx, resources[b], 0, BO_SIZE,
                                                 PIPE_MAP_READ, &transfer);
      if (!map)
         fail("map complete BO for exact verification");
      if (memcmp(map, oracle->expected[b], BO_SIZE)) {
         for (size_t byte = 0; byte < BO_SIZE; ++byte) {
            if (map[byte] != oracle->expected[b][byte]) {
               fprintf(stderr,
                       "buffer=%u first_byte=%#zx got=%#x expected=%#x\n", b,
                       byte, map[byte], oracle->expected[b][byte]);
               break;
            }
         }
         pipe_buffer_unmap(ctx, transfer);
         fail(b == BUFFER_OUTPUT ? "full output/guard mismatch"
                                 : "input BO was mutated");
      }
      pipe_buffer_unmap(ctx, transfer);
   }
}

static void
set_bindings(struct pipe_context *ctx,
             struct pipe_resource *resources[BUFFER_COUNT],
             enum formula formula, unsigned dispatch, int undersized_slot)
{
   static const unsigned slot_to_buffer[BUFFER_COUNT] = {
      BUFFER_OUTPUT,
      BUFFER_DATA,
      BUFFER_INDICES,
   };
   struct pipe_shader_buffer bindings[BUFFER_COUNT] = {0};
   for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot) {
      unsigned b = slot_to_buffer[slot];
      if (formula == FORMULA_UBO && b == BUFFER_DATA)
         continue;
      size_t span = required_span(formula, (enum buffer_index)b);
      bindings[slot] = (struct pipe_shader_buffer){
         .buffer = resources[b],
         .buffer_offset =
            binding_offset(formula, dispatch, (enum buffer_index)b),
         .buffer_size =
            span - (slot == (unsigned)undersized_slot
                       ? access_size(formula, (enum buffer_index)b)
                       : 0u),
      };
   }
   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT, bindings,
                           BITFIELD_BIT(0));

   if (formula == FORMULA_UBO) {
      const size_t span = required_span(formula, BUFFER_DATA);
      const struct pipe_constant_buffer constant = {
         .buffer = resources[BUFFER_DATA],
         .buffer_offset = binding_offset(formula, dispatch, BUFFER_DATA),
         .buffer_size =
            span - (undersized_slot == 1
                       ? access_size(formula, BUFFER_DATA)
                       : 0u),
      };
      ctx->set_constant_buffer(ctx, MESA_SHADER_COMPUTE, 0, &constant);
   } else {
      ctx->set_constant_buffer(ctx, MESA_SHADER_COMPUTE, 0, NULL);
   }
}

static void
check_underbind(struct pipe_context *ctx,
                struct pipe_resource *resources[BUFFER_COUNT],
                const struct pipe_grid_info *grid, enum formula formula,
                unsigned slot)
{
   set_bindings(ctx, resources, formula, 0, (int)slot);
   struct agx_batch *before = agx_get_compute_batch(agx_context(ctx));
   if (!before)
      fail("obtain batch for underbind gate");
   uint32_t dispatches = before->apple9_dispatch_count;
   uint32_t launch = before->apple9_launch_next;
   uint32_t resource = before->apple9_resource_next;
   ctx->launch_grid(ctx, grid);
   struct agx_batch *after = agx_get_compute_batch(agx_context(ctx));
   if (after != before || after->apple9_dispatch_count != dispatches ||
       after->apple9_launch_next != launch ||
       after->apple9_resource_next != resource)
      fail("one-u32 SSBO underbind published work");
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
      fail("compile dependent-load NIR through Gallium");
   ctx->bind_compute_state(ctx, state);

   struct pipe_resource *resources[BUFFER_COUNT] = {0};
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      resources[b] = pipe_buffer_create(
         screen, PIPE_BIND_SHADER_BUFFER |
                    (b == BUFFER_DATA ? PIPE_BIND_CONSTANT_BUFFER : 0),
         b == BUFFER_OUTPUT ? PIPE_USAGE_DEFAULT : PIPE_USAGE_IMMUTABLE,
         BO_SIZE);
      if (!resources[b])
         fail("create 4-KiB guarded SSBO");
      pipe_buffer_write(ctx, resources[b], 0, BO_SIZE, oracle.seed[b]);
   }

   const struct pipe_grid_info grid = {
      .block = {LOCAL_SIZE, 1, 1},
      .grid = {GROUPS, 1, 1},
   };
   for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot)
      check_underbind(ctx, resources, &grid, formula, slot);

   struct agx_batch *batch = agx_get_compute_batch(agx_context(ctx));
   if (!batch)
      fail("obtain batch before valid dispatches");
   uint32_t dispatch_before = batch->apple9_dispatch_count;
   for (unsigned dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
      set_bindings(ctx, resources, formula, dispatch, -1);
      ctx->launch_grid(ctx, &grid);
   }
   struct agx_batch *queued = agx_get_compute_batch(agx_context(ctx));
   if (queued != batch ||
       queued->apple9_dispatch_count != dispatch_before + DISPATCHES)
      fail("two valid dispatches were not queued together");

   finish(screen, ctx);
   verify_resources(ctx, resources, &oracle);

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT, NULL, 0);
   ctx->set_constant_buffer(ctx, MESA_SHADER_COMPUTE, 0, NULL);
   ctx->bind_compute_state(ctx, NULL);
   ctx->delete_compute_state(ctx, state);
   for (unsigned b = 0; b < BUFFER_COUNT; ++b)
      pipe_resource_reference(&resources[b], NULL);
   printf("T8132_GALLIUM_COMPUTE_DEVICE_INDEX_CASE_OK formula=%s "
          "dispatches=2 underbind_rejections=3 exact_output_bytes=0x1000 "
          "inputs_immutable=yes offsets=0x140,end\n",
          formula_name(formula));
}

int
main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "--self-test"))
      return self_test();

   enum formula selected[FORMULA_COUNT] = {FORMULA_INDEX};
   unsigned selected_count = 1;
   if (argc == 2 && !strcmp(argv[1], "--reuse-alu")) {
      selected[0] = FORMULA_REUSE_ALU;
   } else if (argc == 2 && !strcmp(argv[1], "--add1")) {
      selected[0] = FORMULA_ADD1;
   } else if (argc == 2 && !strcmp(argv[1], "--affine")) {
      selected[0] = FORMULA_AFFINE;
   } else if (argc == 2 && !strcmp(argv[1], "--u8-load")) {
      selected[0] = FORMULA_U8_LOAD;
   } else if (argc == 2 && !strcmp(argv[1], "--i8-load")) {
      selected[0] = FORMULA_I8_LOAD;
   } else if (argc == 2 && !strcmp(argv[1], "--u16-load")) {
      selected[0] = FORMULA_U16_LOAD;
   } else if (argc == 2 && !strcmp(argv[1], "--i16-load")) {
      selected[0] = FORMULA_I16_LOAD;
   } else if (argc == 2 && !strcmp(argv[1], "--u8-store")) {
      selected[0] = FORMULA_U8_STORE;
   } else if (argc == 2 && !strcmp(argv[1], "--u16-store")) {
      selected[0] = FORMULA_U16_STORE;
   } else if (argc == 2 && !strcmp(argv[1], "--variable-shl")) {
      selected[0] = FORMULA_VARIABLE_SHL;
   } else if (argc == 2 && !strcmp(argv[1], "--variable-ashr")) {
      selected[0] = FORMULA_VARIABLE_ASHR;
   } else if (argc == 2 && !strcmp(argv[1], "--variable-ushr")) {
      selected[0] = FORMULA_VARIABLE_USHR;
   } else if (argc == 2 && !strcmp(argv[1], "--ubo")) {
      selected[0] = FORMULA_UBO;
   } else if (argc == 2 && !strcmp(argv[1], "--pressure40")) {
      selected[0] = FORMULA_PRESSURE40;
   } else if (argc == 2 && !strcmp(argv[1], "--suite")) {
      for (unsigned formula = 0; formula < FORMULA_COUNT; ++formula)
         selected[formula] = formula;
      selected_count = FORMULA_COUNT;
   } else if (argc != 1) {
      fprintf(stderr,
              "usage: %s [--self-test | --reuse-alu | --add1 | --affine | "
              "--u8-load | --i8-load | --u16-load | --i16-load | "
              "--u8-store | --u16-store | --variable-shl | "
              "--variable-ashr | --variable-ushr | --ubo | --pressure40 | "
              "--suite]\n",
              argv[0]);
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

   for (unsigned i = 0; i < selected_count; ++i)
      run_formula(screen, ctx, selected[i]);

   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   printf("T8132_GALLIUM_COMPUTE_DEVICE_INDEX_OK cases=%u "
          "dispatches=%u underbind_rejections=%u renderer=\"%s\"\n",
          selected_count, selected_count * DISPATCHES,
          selected_count * BUFFER_COUNT, renderer_copy);
   return 0;
}
