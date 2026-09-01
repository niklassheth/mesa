/* SPDX-License-Identifier: MIT */

/*
 * Direct Gallium exact-output test for the generic Apple9 three-SSBO compute
 * profile.  The first five programs are faithful NIR equivalents of the
 * own-source Metal kernels captured by EXP-M4-26:
 *
 *   iadd3(a, b) = a + b
 *   xor3(a, b) = a ^ b
 *   xoradd3(a, b) = (a ^ b) + a
 *   dag3(a, b) = ((a * 3 + b) ^ (a + b * 5)) + (a & 0x00ff00ff)
 *   reuse3(a, b):
 *      p = a + b
 *      q = (a ^ 0x55aa55aa) + (b & 0x00ff00ff)
 *      return (p * 7) ^ q ^ a
 *
 * Gallium's proven Apple9 package interface compacts both the shader bindings
 * and pipe slots as output=0, input-B=1, input-A=2.  The package profile maps
 * those slots back to the native main's input-A, input-B, output order.
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

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENTS 64u
#define LOCAL_SIZE 32u
#define GROUPS (ELEMENTS / LOCAL_SIZE)
#define PAYLOAD_BYTES (ELEMENTS * sizeof(uint32_t))
#define GUARD_BYTES 0x1000u
#define BUFFER_COUNT 3u
#define STATE_SELECTOR_OFFSET 0x20u

enum buffer_index {
   BUFFER_A = 0,
   BUFFER_B = 1,
   BUFFER_OUTPUT = 2,
};

enum formula {
   FORMULA_IADD3 = 0,
   FORMULA_XOR3,
   FORMULA_XORADD3,
   FORMULA_DAG3,
   FORMULA_REUSE3,
   FORMULA_STATE8,
   FORMULA_STATE9,
   FORMULA_IEQ3,
   FORMULA_INE3,
   FORMULA_ILT3,
   FORMULA_IGE3,
   FORMULA_ULT3,
   FORMULA_UGE3,
   FORMULA_COMPARE3,
   FORMULA_FCONST3,
   FORMULA_FLOAT_ADD_SUB3,
   FORMULA_FLOAT_ADD_SUB_MUL3,
   FORMULA_FLOAT_ADD_SUB_MUL_MIN3,
   FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3,
   FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3,
   FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3,
   FORMULA_FLOAT3,
   FORMULA_COUNT,
};

struct formula_desc {
   const char *name;
   enum formula formula;
};

static const struct formula_desc formulas[] = {
   {.name = "iadd3", .formula = FORMULA_IADD3},
   {.name = "xor3", .formula = FORMULA_XOR3},
   {.name = "xoradd3", .formula = FORMULA_XORADD3},
   {.name = "dag3", .formula = FORMULA_DAG3},
   {.name = "reuse3", .formula = FORMULA_REUSE3},
   {.name = "state8", .formula = FORMULA_STATE8},
   {.name = "state9", .formula = FORMULA_STATE9},
   {.name = "ieq3", .formula = FORMULA_IEQ3},
   {.name = "ine3", .formula = FORMULA_INE3},
   {.name = "ilt3", .formula = FORMULA_ILT3},
   {.name = "ige3", .formula = FORMULA_IGE3},
   {.name = "ult3", .formula = FORMULA_ULT3},
   {.name = "uge3", .formula = FORMULA_UGE3},
   {.name = "compare3", .formula = FORMULA_COMPARE3},
   {.name = "fconst3", .formula = FORMULA_FCONST3},
   {.name = "float-add-sub3", .formula = FORMULA_FLOAT_ADD_SUB3},
   {.name = "float-add-sub-mul3", .formula = FORMULA_FLOAT_ADD_SUB_MUL3},
   {.name = "float-add-sub-mul-min3",
    .formula = FORMULA_FLOAT_ADD_SUB_MUL_MIN3},
   {.name = "float-add-sub-mul-min-max3",
    .formula = FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3},
   {.name = "float-add-sub-mul-min-max-abs3",
    .formula = FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3},
   {.name = "float-add-sub-mul-min-max-abs-neg3",
    .formula = FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3},
   {.name = "float3", .formula = FORMULA_FLOAT3},
};

static const uint32_t state8_literals[] = {
   0x10203041u,
   0x00010080u,
   0x00000200u, /* The multiplier selected for ishl(..., 9). */
   UINT32_MAX,  /* The xor mask selected for inot. */
   0x18000000u,
   0x50000000u,
   0x20000000u,
   0x60000000u,
};

static const uint32_t state9_literals[] = {
   0x01020304u,
   0x11223344u,
   0x21324354u,
   0x31425364u,
   0x41526374u,
   0x51627384u,
   0x61728394u,
   0x718293a4u,
   0x8192a3b4u,
};

_Static_assert(ARRAY_SIZE(state8_literals) == 8,
               "state8 must exercise a long literal chain");
_Static_assert(ARRAY_SIZE(state9_literals) == 9,
               "state9 must extend that literal chain by one");

/* Deliberately distinct, nonzero offsets exercise all three range bases. */
static const size_t payload_offsets[BUFFER_COUNT] = {
   0x1000u,
   0x2000u,
   0x3000u,
};

struct oracle {
   uint8_t *seed[BUFFER_COUNT];
   uint8_t *expected[BUFFER_COUNT];
   size_t size[BUFFER_COUNT];
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GALLIUM_COMPUTE_DAG3_FAIL: %s\n", message);
   exit(1);
}

static uint8_t
poison_byte(enum formula formula, unsigned buffer, size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0xa5c39e17u + buffer * 0x31415927u;
   value ^= (uint32_t)formula * 0x6d2b79f5u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static bool
is_float_formula(enum formula formula)
{
   return formula >= FORMULA_FCONST3 && formula <= FORMULA_FLOAT3;
}

static uint32_t
input_word(enum formula formula, unsigned buffer, unsigned lane)
{
   static const uint32_t salt[2] = {
      0x10203040u,
      0x55667788u,
   };

   if (buffer >= 2 || lane >= ELEMENTS)
      fail("invalid input oracle coordinate");

   if (formula >= FORMULA_IEQ3 && formula <= FORMULA_COMPARE3) {
      static const uint32_t pairs[8][2] = {
         {0x12345678u, 0x12345678u},
         {0xffffffffu, 0x00000001u},
         {0x80000000u, 0x7fffffffu},
         {0x7fffffffu, 0x80000000u},
         {0x80000000u, 0x80000000u},
         {0x00000000u, 0xffffffffu},
         {0x12345678u, 0x87654321u},
         {0x87654321u, 0x12345678u},
      };
      return pairs[lane % 8][buffer];
   }

   if (is_float_formula(formula)) {
      /* Finite normal binary32 values with exact quarter-unit arithmetic. */
      static const uint32_t pairs[8][2] = {
         {0x3fc00000u, 0x40000000u}, /*  1.5,   2.0  */
         {0xc0200000u, 0x3f000000u}, /* -2.5,   0.5  */
         {0x40800000u, 0xbfc00000u}, /*  4.0,  -1.5  */
         {0xbf400000u, 0xc0000000u}, /* -0.75, -2.0  */
         {0x41000000u, 0x3e800000u}, /*  8.0,   0.25 */
         {0xc0800000u, 0xbf000000u}, /* -4.0,  -0.5  */
         {0x40400000u, 0x3f800000u}, /*  3.0,   1.0  */
         {0xbf800000u, 0x40000000u}, /* -1.0,   2.0  */
      };
      return pairs[lane % 8][buffer];
   }

   return salt[buffer] ^
          (lane * (0x01010101u + buffer * 0x00110011u));
}

static uint32_t
signed_key(uint32_t value)
{
   return value ^ 0x80000000u;
}

static uint32_t
signed_min(uint32_t a, uint32_t b)
{
   return signed_key(a) < signed_key(b) ? a : b;
}

static uint32_t
signed_max(uint32_t a, uint32_t b)
{
   return signed_key(a) > signed_key(b) ? a : b;
}

static float
float_from_bits(uint32_t bits)
{
   float value;
   memcpy(&value, &bits, sizeof(value));
   return value;
}

static uint32_t
float_to_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

/* Volatile stores make the CPU oracle's binary32 rounding points explicit. */
static float
oracle_fadd(float a, float b)
{
   volatile float value = a + b;
   return value;
}

static float
oracle_fsub(float a, float b)
{
   volatile float value = a - b;
   return value;
}

static float
oracle_fmul(float a, float b)
{
   volatile float value = a * b;
   return value;
}

static float
oracle_ffma(float a, float b, float c)
{
   volatile float value = fmaf(a, b, c);
   return value;
}

static uint32_t
state8_value(uint32_t a, uint32_t b,
             const uint32_t literals[static 8])
{
   uint32_t value = a - literals[0];
   value ^= b | literals[1];
   value += a * literals[2];
   value ^= b ^ literals[3];
   value += 0u - a;
   value ^= signed_min(a, literals[4]);
   value += signed_max(b, literals[5]);
   value ^= a < literals[6] ? a : literals[6];
   value += b > literals[7] ? b : literals[7];
   return value;
}

static uint32_t
state9_value(uint32_t a, uint32_t b,
             const uint32_t literals[static 9])
{
   uint32_t value = a;
   for (unsigned i = 0; i < 9; ++i)
      value = (i & 1) ? value ^ literals[i] : value + literals[i];
   return value ^ b;
}

static uint32_t
formula_value(enum formula formula, uint32_t a, uint32_t b)
{
   switch (formula) {
   case FORMULA_IADD3:
      return a + b;
   case FORMULA_XOR3:
      return a ^ b;
   case FORMULA_XORADD3:
      return (a ^ b) + a;
   case FORMULA_DAG3:
      return ((a * 3u + b) ^ (a + b * 5u)) +
             (a & 0x00ff00ffu);
   case FORMULA_REUSE3: {
      uint32_t p = a + b;
      uint32_t q = (a ^ 0x55aa55aau) + (b & 0x00ff00ffu);
      return (p * 7u) ^ q ^ a;
   }
   case FORMULA_STATE8:
      return state8_value(a, b, state8_literals);
   case FORMULA_STATE9:
      return state9_value(a, b, state9_literals);
   case FORMULA_IEQ3:
      return a == b;
   case FORMULA_INE3:
      return a != b;
   case FORMULA_ILT3:
      return signed_key(a) < signed_key(b);
   case FORMULA_IGE3:
      return signed_key(a) >= signed_key(b);
   case FORMULA_ULT3:
      return a < b;
   case FORMULA_UGE3:
      return a >= b;
   case FORMULA_COMPARE3:
      return ((a == b) ? 0x01u : 0u) |
             ((a != b) ? 0x02u : 0u) |
             ((signed_key(a) < signed_key(b)) ? 0x04u : 0u) |
             ((signed_key(a) >= signed_key(b)) ? 0x08u : 0u) |
             ((a < b) ? 0x10u : 0u) |
             ((a >= b) ? 0x20u : 0u);
   case FORMULA_FCONST3: {
      float fa = float_from_bits(a);
      float fb = float_from_bits(b);
      return float_to_bits(oracle_fadd(oracle_fadd(fa, fb), 2.0f));
   }
   case FORMULA_FLOAT_ADD_SUB3:
   case FORMULA_FLOAT_ADD_SUB_MUL3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3:
   case FORMULA_FLOAT3: {
      float fa = float_from_bits(a);
      float fb = float_from_bits(b);
      float add = oracle_fadd(fa, fb);
      float sub = oracle_fsub(fa, fb);
      float mul = oracle_fmul(fa, fb);
      float minimum = fminf(fa, fb);
      float maximum = fmaxf(fa, fb);
      float absolute = fabsf(sub);
      float negative = -minimum;
      float fused = oracle_ffma(fa, fb, add);
      float value = oracle_fadd(add, sub);
      if (formula == FORMULA_FLOAT_ADD_SUB3)
         return float_to_bits(value);
      value = oracle_fadd(value, mul);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL3)
         return float_to_bits(value);
      value = oracle_fadd(value, minimum);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN3)
         return float_to_bits(value);
      value = oracle_fadd(value, maximum);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3)
         return float_to_bits(value);
      value = oracle_fadd(value, absolute);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3)
         return float_to_bits(value);
      value = oracle_fadd(value, negative);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3)
         return float_to_bits(value);
      value = oracle_fadd(value, fused);
      return float_to_bits(value);
   }
   default:
      fail("invalid formula");
      return 0;
   }
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
oracle_finish(struct oracle *oracle)
{
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      free(oracle->seed[b]);
      free(oracle->expected[b]);
   }
   memset(oracle, 0, sizeof(*oracle));
}

static void
oracle_init(struct oracle *oracle, enum formula formula)
{
   memset(oracle, 0, sizeof(*oracle));

   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      oracle->size[b] = payload_offsets[b] + PAYLOAD_BYTES + GUARD_BYTES;
      oracle->seed[b] = malloc(oracle->size[b]);
      oracle->expected[b] = malloc(oracle->size[b]);
      if (!oracle->seed[b] || !oracle->expected[b])
         fail("allocate guarded CPU oracle");

      for (size_t i = 0; i < oracle->size[b]; ++i) {
         uint8_t poison = poison_byte(formula, b, i);
         oracle->seed[b][i] = poison;
         oracle->expected[b][i] = poison;
      }
   }

   for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
      uint32_t a = input_word(formula, BUFFER_A, lane);
      uint32_t b = input_word(formula, BUFFER_B, lane);
      size_t byte = lane * sizeof(uint32_t);
      write_word(oracle->seed[BUFFER_A],
                 payload_offsets[BUFFER_A] + byte, a);
      write_word(oracle->expected[BUFFER_A],
                 payload_offsets[BUFFER_A] + byte, a);
      write_word(oracle->seed[BUFFER_B],
                 payload_offsets[BUFFER_B] + byte, b);
      write_word(oracle->expected[BUFFER_B],
                 payload_offsets[BUFFER_B] + byte, b);
      write_word(oracle->expected[BUFFER_OUTPUT],
                 payload_offsets[BUFFER_OUTPUT] + byte,
                 formula_value(formula, a, b));
   }
}

static nir_shader *
build_shader(const nir_shader_compiler_options *options, enum formula formula)
{
   const char *name;
   switch (formula) {
   case FORMULA_IADD3: name = "t8132_iadd3"; break;
   case FORMULA_XOR3: name = "t8132_xor3"; break;
   case FORMULA_XORADD3: name = "t8132_xoradd3"; break;
   case FORMULA_DAG3: name = "t8132_dag3"; break;
   case FORMULA_REUSE3: name = "t8132_reuse3"; break;
   case FORMULA_STATE8: name = "t8132_state8"; break;
   case FORMULA_STATE9: name = "t8132_state9"; break;
   case FORMULA_IEQ3: name = "t8132_ieq3"; break;
   case FORMULA_INE3: name = "t8132_ine3"; break;
   case FORMULA_ILT3: name = "t8132_ilt3"; break;
   case FORMULA_IGE3: name = "t8132_ige3"; break;
   case FORMULA_ULT3: name = "t8132_ult3"; break;
   case FORMULA_UGE3: name = "t8132_uge3"; break;
   case FORMULA_COMPARE3: name = "t8132_compare3"; break;
   case FORMULA_FCONST3: name = "t8132_fconst3"; break;
   case FORMULA_FLOAT_ADD_SUB3: name = "t8132_float_add_sub3"; break;
   case FORMULA_FLOAT_ADD_SUB_MUL3:
      name = "t8132_float_add_sub_mul3";
      break;
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN3:
      name = "t8132_float_add_sub_mul_min3";
      break;
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3:
      name = "t8132_float_add_sub_mul_min_max3";
      break;
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3:
      name = "t8132_float_add_sub_mul_min_max_abs3";
      break;
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3:
      name = "t8132_float_add_sub_mul_min_max_abs_neg3";
      break;
   case FORMULA_FLOAT3: name = "t8132_float3"; break;
   default:
      fail("name unknown formula");
      return NULL;
   }
   nir_builder n = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "%s", name);
   n.shader->info.workgroup_size[0] = LOCAL_SIZE;
   n.shader->info.workgroup_size[1] = 1;
   n.shader->info.workgroup_size[2] = 1;
   n.shader->info.num_ssbos = 3;

   nir_def *gid = nir_channel(
      &n, nir_load_global_invocation_id(&n, 32), 0);
   nir_def *offset = nir_imul_imm(&n, gid, sizeof(uint32_t));

   /* See the compact binding contract in the file-level comment. */
   nir_def *a = nir_load_ssbo(
      &n, 1, 32, nir_imm_int(&n, 2), offset);
   nir_def *b = nir_load_ssbo(
      &n, 1, 32, nir_imm_int(&n, 1), offset);
   nir_def *value;

   switch (formula) {
   case FORMULA_IADD3:
      value = nir_iadd(&n, a, b);
      break;
   case FORMULA_XOR3:
      value = nir_ixor(&n, a, b);
      break;
   case FORMULA_XORADD3:
      value = nir_iadd(&n, nir_ixor(&n, a, b), a);
      break;
   case FORMULA_DAG3: {
      nir_def *left = nir_iadd(
         &n, nir_imul_imm(&n, a, 3), b);
      nir_def *right = nir_iadd(
         &n, a, nir_imul_imm(&n, b, 5));
      value = nir_iadd(
         &n, nir_ixor(&n, left, right),
         nir_iand(&n, a, nir_imm_int(&n, 0x00ff00ffu)));
      break;
   }
   case FORMULA_REUSE3: {
      nir_def *p = nir_iadd(&n, a, b);
      nir_def *q = nir_iadd(
         &n, nir_ixor(&n, a, nir_imm_int(&n, 0x55aa55aau)),
         nir_iand(&n, b, nir_imm_int(&n, 0x00ff00ffu)));
      value = nir_ixor(
         &n, nir_ixor(&n, nir_imul_imm(&n, p, 7), q), a);
      break;
   }
   case FORMULA_STATE8: {
      value = nir_isub(
         &n, a, nir_imm_int(&n, state8_literals[0]));
      value = nir_ixor(
         &n, value,
         nir_ior(&n, b, nir_imm_int(&n, state8_literals[1])));
      value = nir_iadd(&n, value, nir_ishl_imm(&n, a, 9));
      value = nir_ixor(&n, value, nir_inot(&n, b));
      value = nir_iadd(&n, value, nir_ineg(&n, a));
      value = nir_ixor(
         &n, value,
         nir_imin(&n, a, nir_imm_int(&n, state8_literals[4])));
      value = nir_iadd(
         &n, value,
         nir_imax(&n, b, nir_imm_int(&n, state8_literals[5])));
      value = nir_ixor(
         &n, value,
         nir_umin(&n, a, nir_imm_int(&n, state8_literals[6])));
      value = nir_iadd(
         &n, value,
         nir_umax(&n, b, nir_imm_int(&n, state8_literals[7])));
      break;
   }
   case FORMULA_STATE9:
      value = a;
      for (unsigned i = 0; i < ARRAY_SIZE(state9_literals); ++i) {
         nir_def *constant = nir_imm_int(&n, state9_literals[i]);
         value = (i & 1) ? nir_ixor(&n, value, constant)
                         : nir_iadd(&n, value, constant);
      }
      value = nir_ixor(&n, value, b);
      break;
   case FORMULA_IEQ3:
      value = nir_b2i32(&n, nir_ieq(&n, a, b));
      break;
   case FORMULA_INE3:
      value = nir_b2i32(&n, nir_ine(&n, a, b));
      break;
   case FORMULA_ILT3:
      value = nir_b2i32(&n, nir_ilt(&n, a, b));
      break;
   case FORMULA_IGE3:
      value = nir_b2i32(&n, nir_ige(&n, a, b));
      break;
   case FORMULA_ULT3:
      value = nir_b2i32(&n, nir_ult(&n, a, b));
      break;
   case FORMULA_UGE3:
      value = nir_b2i32(&n, nir_uge(&n, a, b));
      break;
   case FORMULA_COMPARE3: {
      nir_def *bits[] = {
         nir_bcsel(&n, nir_ieq(&n, a, b), nir_imm_int(&n, 0x01),
                   nir_imm_int(&n, 0)),
         nir_bcsel(&n, nir_ine(&n, a, b), nir_imm_int(&n, 0x02),
                   nir_imm_int(&n, 0)),
         nir_bcsel(&n, nir_ilt(&n, a, b), nir_imm_int(&n, 0x04),
                   nir_imm_int(&n, 0)),
         nir_bcsel(&n, nir_ige(&n, a, b), nir_imm_int(&n, 0x08),
                   nir_imm_int(&n, 0)),
         nir_bcsel(&n, nir_ult(&n, a, b), nir_imm_int(&n, 0x10),
                   nir_imm_int(&n, 0)),
         nir_bcsel(&n, nir_uge(&n, a, b), nir_imm_int(&n, 0x20),
                   nir_imm_int(&n, 0)),
      };
      value = bits[0];
      for (unsigned i = 1; i < ARRAY_SIZE(bits); ++i)
         value = nir_ior(&n, value, bits[i]);
      break;
   }
   case FORMULA_FCONST3:
      value = nir_fadd(
         &n, nir_fadd(&n, a, b), nir_imm_float(&n, 2.0f));
      break;
   case FORMULA_FLOAT_ADD_SUB3:
   case FORMULA_FLOAT_ADD_SUB_MUL3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3:
   case FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3:
   case FORMULA_FLOAT3: {
      nir_def *add = nir_fadd(&n, a, b);
      nir_def *sub = nir_fsub(&n, a, b);
      nir_def *mul = nir_fmul(&n, a, b);
      nir_def *minimum = nir_fmin(&n, a, b);
      nir_def *maximum = nir_fmax(&n, a, b);
      nir_def *absolute = nir_fabs(&n, sub);
      nir_def *negative = nir_fneg(&n, minimum);
      nir_def *fused = nir_ffma(&n, a, b, add);
      value = nir_fadd(&n, add, sub);
      if (formula == FORMULA_FLOAT_ADD_SUB3)
         break;
      value = nir_fadd(&n, value, mul);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL3)
         break;
      value = nir_fadd(&n, value, minimum);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN3)
         break;
      value = nir_fadd(&n, value, maximum);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX3)
         break;
      value = nir_fadd(&n, value, absolute);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS3)
         break;
      value = nir_fadd(&n, value, negative);
      if (formula == FORMULA_FLOAT_ADD_SUB_MUL_MIN_MAX_ABS_NEG3)
         break;
      value = nir_fadd(&n, value, fused);
      break;
   }
   default:
      fail("build unknown formula");
      return NULL;
   }

   nir_store_ssbo(&n, value, nir_imm_int(&n, 0), offset,
                  .write_mask = 1, .align_mul = sizeof(uint32_t));
   nir_shader_gather_info(n.shader, nir_shader_get_entrypoint(n.shader));
   n.shader->info.num_ssbos = 3;
   nir_validate_shader(n.shader, "T8132 three-SSBO compute fixture");
   return n.shader;
}

static void
check_oracle(const struct oracle *oracle, enum formula formula)
{
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      if (!payload_offsets[b] ||
          payload_offsets[b] + PAYLOAD_BYTES + GUARD_BYTES !=
             oracle->size[b])
         fail("guarded range layout invariant");
   }

   if (memcmp(oracle->seed[BUFFER_A], oracle->expected[BUFFER_A],
              oracle->size[BUFFER_A]) != 0 ||
       memcmp(oracle->seed[BUFFER_B], oracle->expected[BUFFER_B],
              oracle->size[BUFFER_B]) != 0)
      fail("input oracle is not immutable");

   const size_t out = payload_offsets[BUFFER_OUTPUT];
   if (memcmp(oracle->seed[BUFFER_OUTPUT], oracle->expected[BUFFER_OUTPUT],
              out) != 0 ||
       memcmp(oracle->seed[BUFFER_OUTPUT] + out + PAYLOAD_BYTES,
              oracle->expected[BUFFER_OUTPUT] + out + PAYLOAD_BYTES,
              GUARD_BYTES) != 0)
      fail("output guards changed in CPU oracle");

   for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
      uint32_t a = input_word(formula, BUFFER_A, lane);
      uint32_t b = input_word(formula, BUFFER_B, lane);
      uint32_t expected = formula_value(formula, a, b);
      uint32_t actual = read_word(
         oracle->expected[BUFFER_OUTPUT],
         out + lane * sizeof(uint32_t));
      if (actual != expected)
         fail("output formula oracle mismatch");
   }
}

static void
check_case_coverage(void)
{
   static const uint32_t compare_outputs[8] = {
      0x29u, 0x26u, 0x26u, 0x1au, 0x29u, 0x1au, 0x1au, 0x26u,
   };
   static const uint32_t float_outputs[8] = {
      0x41700000u, /*  15.0  */
      0xc0c00000u, /*  -6.0  */
      0x41000000u, /*   8.0  */
      0xbf400000u, /*  -0.75 */
      0x42300000u, /*  44.0  */
      0xc0b00000u, /*  -5.5  */
      0x41a80000u, /*  21.0  */
      0x00000000u, /*   0.0  */
   };

   for (unsigned lane = 0; lane < 8; ++lane) {
      uint32_t compare_a = input_word(FORMULA_COMPARE3, BUFFER_A, lane);
      uint32_t compare_b = input_word(FORMULA_COMPARE3, BUFFER_B, lane);
      if (formula_value(FORMULA_COMPARE3, compare_a, compare_b) !=
          compare_outputs[lane])
         fail("compare3 predicate packing coverage");

      uint32_t float_a = input_word(FORMULA_FLOAT3, BUFFER_A, lane);
      uint32_t float_b = input_word(FORMULA_FLOAT3, BUFFER_B, lane);
      uint32_t exponent_a = (float_a >> 23) & 0xff;
      uint32_t exponent_b = (float_b >> 23) & 0xff;
      if (exponent_a == 0 || exponent_a == 0xff ||
          exponent_b == 0 || exponent_b == 0xff ||
          formula_value(FORMULA_FLOAT3, float_a, float_b) !=
             float_outputs[lane])
         fail("float3 finite-normal bit-exact oracle coverage");
   }

   for (unsigned i = 0; i < ARRAY_SIZE(state8_literals); ++i) {
      if (state8_literals[i] <= 0xff)
         fail("state8 literal is not large");
      for (unsigned j = i + 1; j < ARRAY_SIZE(state8_literals); ++j) {
         if (state8_literals[i] == state8_literals[j])
            fail("state8 literals are not distinct");
      }

      uint32_t changed[ARRAY_SIZE(state8_literals)];
      memcpy(changed, state8_literals, sizeof(changed));
      changed[i] = 0;
      bool affects_output = false;
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t a = input_word(FORMULA_STATE8, BUFFER_A, lane);
         uint32_t b = input_word(FORMULA_STATE8, BUFFER_B, lane);
         affects_output |= state8_value(a, b, changed) !=
                           state8_value(a, b, state8_literals);
      }
      if (!affects_output)
         fail("state8 literal does not affect covered output");
   }

   for (unsigned i = 0; i < ARRAY_SIZE(state9_literals); ++i) {
      if (state9_literals[i] <= 0xff)
         fail("state9 literal is not large");
      for (unsigned j = i + 1; j < ARRAY_SIZE(state9_literals); ++j) {
         if (state9_literals[i] == state9_literals[j])
            fail("state9 literals are not distinct");
      }

      uint32_t changed[ARRAY_SIZE(state9_literals)];
      memcpy(changed, state9_literals, sizeof(changed));
      changed[i] = 0;
      bool affects_output = false;
      for (unsigned lane = 0; lane < ELEMENTS; ++lane) {
         uint32_t a = input_word(FORMULA_STATE9, BUFFER_A, lane);
         uint32_t b = input_word(FORMULA_STATE9, BUFFER_B, lane);
         affects_output |= state9_value(a, b, changed) !=
                           state9_value(a, b, state9_literals);
      }
      if (!affects_output)
         fail("state9 literal does not affect covered output");
   }
}

static int
self_test(void)
{
   struct agx_shader_part compiled[FORMULA_COUNT] = {0};

   check_case_coverage();

   for (unsigned i = 0; i < FORMULA_COUNT; ++i) {
      struct oracle oracle;
      oracle_init(&oracle, formulas[i].formula);
      check_oracle(&oracle, formulas[i].formula);
      oracle_finish(&oracle);

      nir_shader *nir = build_shader(&agx_nir_options,
                                     formulas[i].formula);
      struct agx_apple9_compute_profile profile = {0};
      const char *reason = NULL;
      if (!agx_compile_apple9_tiny(
             nir, &compiled[i], &profile, &reason)) {
         fprintf(stderr, "%s Apple9 compile failed: %s\n",
                 formulas[i].name, reason ? reason : "no diagnostic");
         fail("offline three-SSBO compute compile");
      }
      if (!compiled[i].binary || !compiled[i].info.binary_size ||
          profile.abi != AGX_APPLE9_COMPUTE_ABI_SSBO3_STATE_U6 ||
          profile.local_size[0] != LOCAL_SIZE ||
          profile.local_size[1] != 1 || profile.local_size[2] != 1 ||
          profile.index_rank != 1 || profile.index_stride[0] != 1 ||
          profile.index_stride[1] != 0 || profile.index_stride[2] != 0 ||
          agx_apple9_compute_resource_count(&profile) != 3 ||
          agx_apple9_compute_resource_binding(&profile, 0) != 2 ||
          agx_apple9_compute_resource_binding(&profile, 1) != 1 ||
          agx_apple9_compute_resource_binding(&profile, 2) != 0 ||
          agx_apple9_compute_read_mask(&profile) != 3 ||
          agx_apple9_compute_write_mask(&profile) != 4 ||
          profile.state_literal_count != 0)
         fail("offline three-SSBO package profile invariant");
      ralloc_free(nir);
   }

   for (unsigned a = 0; a < FORMULA_COUNT; ++a) {
      for (unsigned b = a + 1; b < FORMULA_COUNT; ++b) {
         if (compiled[a].info.binary_size == compiled[b].info.binary_size &&
             memcmp(compiled[a].binary, compiled[b].binary,
                    compiled[a].info.binary_size) == 0)
            fail("distinct formulas compiled to identical Apple9 mains");
      }
   }

   for (unsigned i = 0; i < FORMULA_COUNT; ++i)
      free(compiled[i].binary);

   printf("T8132_GALLIUM_COMPUTE_DAG3_SELF_TEST_OK "
          "cases=iadd3,xor3,xoradd3,dag3,reuse3,state8,state9,"
          "ieq3,ine3,ilt3,ige3,ult3,uge3,compare3,"
          "fconst3,"
          "float-add-sub3,float-add-sub-mul3,"
          "float-add-sub-mul-min3,float-add-sub-mul-min-max3,"
          "float-add-sub-mul-min-max-abs3,"
          "float-add-sub-mul-min-max-abs-neg3,float3 "
          "elements=64 local=32 offsets=0x1000,0x2000,0x3000\n");
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

static const char *
buffer_role(unsigned buffer)
{
   switch (buffer) {
   case BUFFER_A: return "input-a";
   case BUFFER_B: return "input-b";
   default: return "output";
   }
}

static bool
guards_are_exact(uint8_t *const actual[BUFFER_COUNT],
                 const struct oracle *oracle)
{
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      size_t payload_end = payload_offsets[b] + PAYLOAD_BYTES;
      if (memcmp(actual[b], oracle->expected[b], payload_offsets[b]) != 0 ||
          memcmp(actual[b] + payload_end,
                 oracle->expected[b] + payload_end,
                 oracle->size[b] - payload_end) != 0)
         return false;
   }
   return true;
}

static void
verify_results(const struct formula_desc *test,
               uint8_t *const actual[BUFFER_COUNT],
               const struct oracle *oracle)
{
   bool all_exact = true;
   for (unsigned b = 0; b < BUFFER_COUNT; ++b)
      all_exact &= memcmp(actual[b], oracle->expected[b],
                          oracle->size[b]) == 0;
   if (all_exact)
      return;

   bool inputs_exact =
      memcmp(actual[BUFFER_A] + payload_offsets[BUFFER_A],
             oracle->expected[BUFFER_A] + payload_offsets[BUFFER_A],
             PAYLOAD_BYTES) == 0 &&
      memcmp(actual[BUFFER_B] + payload_offsets[BUFFER_B],
             oracle->expected[BUFFER_B] + payload_offsets[BUFFER_B],
             PAYLOAD_BYTES) == 0;
   bool guards_exact = guards_are_exact(actual, oracle);

   fprintf(stderr,
           "T8132_GALLIUM_COMPUTE_DAG3_FAIL: %s exact-output mismatch "
           "inputs_exact=%s guards_exact=%s\n",
           test->name, inputs_exact ? "yes" : "no",
           guards_exact ? "yes" : "no");
   fprintf(stderr, "first 8 output words (actual/expected):\n");
   for (unsigned lane = 0; lane < 8; ++lane) {
      size_t offset = payload_offsets[BUFFER_OUTPUT] +
                      lane * sizeof(uint32_t);
      fprintf(stderr, "  [%u] %#010x / %#010x\n", lane,
              read_word(actual[BUFFER_OUTPUT], offset),
              read_word(oracle->expected[BUFFER_OUTPUT], offset));
   }

   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      for (size_t i = 0; i < oracle->size[b]; ++i) {
         if (actual[b][i] == oracle->expected[b][i])
            continue;

         const char *region = i < payload_offsets[b]
                              ? "leading-guard"
                           : i < payload_offsets[b] + PAYLOAD_BYTES
                              ? "payload"
                              : "trailing-guard";
         fprintf(stderr,
                 "first differing byte: %s %s offset=%#zx actual=%#x "
                 "expected=%#x\n",
                 buffer_role(b), region, i, actual[b][i],
                 oracle->expected[b][i]);
         exit(1);
      }
   }

   fail("mismatch diagnostic could not find differing byte");
}

static void *
create_formula_state(struct pipe_screen *screen, struct pipe_context *ctx,
                     const struct formula_desc *test)
{
   nir_shader *nir = build_shader(
      screen->nir_options[MESA_SHADER_COMPUTE], test->formula);
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
      fail("compile three-SSBO compute shader through Gallium");
   return compute;
}

static void
run_formula(struct pipe_screen *screen, struct pipe_context *ctx,
            const struct formula_desc *test, void *precompiled)
{
   struct oracle oracle;
   oracle_init(&oracle, test->formula);

   bool owns_compute = precompiled == NULL;
   void *compute = owns_compute
      ? create_formula_state(screen, ctx, test)
      : precompiled;
   ctx->bind_compute_state(ctx, compute);

   struct pipe_resource *resources[BUFFER_COUNT] = {0};
   struct pipe_shader_buffer bindings[BUFFER_COUNT] = {0};
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      enum pipe_resource_usage usage =
         b == BUFFER_OUTPUT ? PIPE_USAGE_DEFAULT : PIPE_USAGE_IMMUTABLE;
      resources[b] = pipe_buffer_create(
         screen, PIPE_BIND_SHADER_BUFFER, usage, oracle.size[b]);
      if (!resources[b])
         fail("create guarded shader buffer");
      pipe_buffer_write(ctx, resources[b], 0, oracle.size[b],
                        oracle.seed[b]);
   }

   static const unsigned compact_slot_to_buffer[BUFFER_COUNT] = {
      BUFFER_OUTPUT,
      BUFFER_B,
      BUFFER_A,
   };
   for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot) {
      unsigned b = compact_slot_to_buffer[slot];
      bindings[slot] = (struct pipe_shader_buffer){
         .buffer = resources[b],
         .buffer_offset = payload_offsets[b],
         .buffer_size = PAYLOAD_BYTES,
      };
   }

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT,
                           bindings, 1u << 0);
   const struct pipe_grid_info grid = {
      .block = {LOCAL_SIZE, 1, 1},
      .grid = {GROUPS, 1, 1},
   };
   ctx->launch_grid(ctx, &grid);

   struct pipe_fence_handle *fence = NULL;
   ctx->flush(ctx, &fence, PIPE_FLUSH_END_OF_FRAME);
   if (!fence ||
       !screen->fence_finish(screen, ctx, fence, OS_TIMEOUT_INFINITE))
      fail("wait for three-SSBO compute dispatch");
   screen->fence_reference(screen, &fence, NULL);

   uint8_t *actual[BUFFER_COUNT] = {0};
   for (unsigned b = 0; b < BUFFER_COUNT; ++b) {
      actual[b] = malloc(oracle.size[b]);
      if (!actual[b])
         fail("allocate full-BO verification image");
      struct pipe_transfer *transfer = NULL;
      const uint8_t *mapped = pipe_buffer_map_range(
         ctx, resources[b], 0, oracle.size[b], PIPE_MAP_READ, &transfer);
      if (!mapped)
         fail("map guarded shader buffer for verification");
      memcpy(actual[b], mapped, oracle.size[b]);
      pipe_buffer_unmap(ctx, transfer);
   }
   verify_results(test, actual, &oracle);
   for (unsigned b = 0; b < BUFFER_COUNT; ++b)
      free(actual[b]);

   ctx->set_shader_buffers(ctx, MESA_SHADER_COMPUTE, 0, BUFFER_COUNT,
                           NULL, 0);
   ctx->bind_compute_state(ctx, NULL);
   if (owns_compute)
      ctx->delete_compute_state(ctx, compute);
   for (unsigned b = 0; b < BUFFER_COUNT; ++b)
      pipe_resource_reference(&resources[b], NULL);
   oracle_finish(&oracle);
}

static const struct formula_desc *
find_formula(const char *name)
{
   for (unsigned i = 0; i < FORMULA_COUNT; ++i) {
      if (strcmp(name, formulas[i].name) == 0)
         return &formulas[i];
   }
   return NULL;
}

static struct agx_compiled_shader *
compiled_shader_from_state(void *state)
{
   struct agx_uncompiled_shader *shader = state;
   struct hash_entry *entry =
      _mesa_hash_table_next_entry(shader->variants, NULL);
   if (!entry)
      fail("compute state has no compiled variant");
   return entry->data;
}

static unsigned
dual_vm_rounds(void)
{
   const char *value = getenv("T8132_DUAL_VM_ROUNDS");
   if (!value)
      return 32;

   char *end = NULL;
   unsigned long parsed = strtoul(value, &end, 0);
   if (!value[0] || !end || end[0] || parsed == 0 || parsed > 512)
      fail("invalid T8132_DUAL_VM_ROUNDS");
   return (unsigned)parsed;
}

static int
run_dual_vm_state_alias(void)
{
   const struct formula_desc *tests[2] = {
      find_formula("state8"),
      find_formula("state9"),
   };
   struct pipe_loader_device *devices[2] = {0};
   struct pipe_screen *screens[2] = {0};
   struct pipe_context *contexts[2] = {0};
   void *states[2] = {0};
   struct agx_compiled_shader *compiled[2] = {0};
   char renderer[128] = {0};

   for (unsigned i = 0; i < 2; ++i) {
      /* Probe independently so the DRM shim creates two DRM files, clients,
       * VMs, and userspace VA allocators.  Their first per-pipeline Dynamic
       * Caching records should therefore deliberately alias in GPU VA while
       * remaining backed by different pages and roots. */
      devices[i] = find_asahi_device();
      if (!devices[i])
         fail("Asahi Gallium device not found for dual-VM test");
      screens[i] = pipe_loader_create_screen(devices[i], false);
      if (!screens[i])
         fail("create Asahi Gallium screen for dual-VM test");
      const char *name = screens[i]->get_name(screens[i]);
      if (!name || strstr(name, "Apple M4") == NULL)
         fail("unexpected dual-VM Gallium renderer");
      if (i == 0)
         snprintf(renderer, sizeof(renderer), "%s", name);
      contexts[i] = screens[i]->context_create(screens[i], NULL, 0);
      if (!contexts[i])
         fail("create Gallium context for dual-VM test");
      states[i] = create_formula_state(
         screens[i], contexts[i], tests[i]);
      compiled[i] = compiled_shader_from_state(states[i]);
      if (!compiled[i]->apple9_tiny || !compiled[i]->apple9_state_bo ||
          !compiled[i]->apple9_state_address)
         fail("dual-VM shader lacks persistent Apple9 state");
   }

   if (compiled[0]->apple9_state_address !=
       compiled[1]->apple9_state_address)
      fail("dual-VM test did not create an aliased state selector");
   if (memcmp(agx_bo_map(compiled[0]->apple9_state_bo),
              agx_bo_map(compiled[1]->apple9_state_bo),
              AGX_APPLE9_COMPUTE_STATE_STRIDE) == 0)
      fail("dual-VM aliased state records are not distinguishable");

   unsigned rounds = dual_vm_rounds();
   for (unsigned round = 0; round < rounds; ++round) {
      run_formula(screens[0], contexts[0], tests[0], states[0]);
      run_formula(screens[1], contexts[1], tests[1], states[1]);
   }

   uint64_t state_address = compiled[0]->apple9_state_address;
   for (unsigned i = 0; i < 2; ++i) {
      contexts[i]->delete_compute_state(contexts[i], states[i]);
      contexts[i]->destroy(contexts[i]);
      screens[i]->destroy(screens[i]);
      pipe_loader_release(&devices[i], 1);
   }

   printf("T8132_GALLIUM_COMPUTE_DUAL_VM_STATE_ALIAS_OK "
          "clients=2 vms=2 rounds=%u publications=%u state=%#llx "
          "state_bytes=different renderer=\"%s\"\n",
          rounds, rounds * 2,
          (unsigned long long)state_address, renderer);
   return 0;
}

static int
run_state_append_after_use(void)
{
   const struct formula_desc *state8 = find_formula("state8");
   const struct formula_desc *state9 = find_formula("state9");
   struct pipe_loader_device *device = find_asahi_device();
   if (!device)
      fail("Asahi Gallium device not found for state append test");

   struct pipe_screen *screen = pipe_loader_create_screen(device, false);
   if (!screen)
      fail("create Asahi Gallium screen for state append test");
   const char *name = screen->get_name(screen);
   if (!name || strstr(name, "Apple M4") == NULL)
      fail("unexpected state append Gallium renderer");
   char renderer[128];
   snprintf(renderer, sizeof(renderer), "%s", name);

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   if (!ctx)
      fail("create Gallium context for state append test");

   /* Publish the first selector before allocating the second record.  This
    * is the important lifetime boundary: appending CPU-authored bytes to the
    * same mapped slab page must not invalidate a record the GPU already used.
    */
   void *states[2] = {create_formula_state(screen, ctx, state8), NULL};
   struct agx_compiled_shader *compiled[2] = {
      compiled_shader_from_state(states[0]), NULL,
   };
   run_formula(screen, ctx, state8, states[0]);

   states[1] = create_formula_state(screen, ctx, state9);
   compiled[1] = compiled_shader_from_state(states[1]);
   if (!compiled[0]->apple9_state_bo || !compiled[1]->apple9_state_bo ||
       compiled[0]->apple9_state_bo != compiled[1]->apple9_state_bo)
      fail("successive state records did not share one slab");
   if (compiled[1]->apple9_state_address !=
       compiled[0]->apple9_state_address + AGX_APPLE9_COMPUTE_STATE_STRIDE)
      fail("successive state selectors are not one record apart");

   uint64_t slab_base = compiled[0]->apple9_state_bo->va->addr;
   uint64_t first_offset = compiled[0]->apple9_state_address - slab_base -
                           STATE_SELECTOR_OFFSET;
   uint64_t second_offset = compiled[1]->apple9_state_address - slab_base -
                            STATE_SELECTOR_OFFSET;
   if (second_offset + AGX_APPLE9_COMPUTE_STATE_STRIDE >
          compiled[0]->apple9_state_bo->size ||
       memcmp((uint8_t *)agx_bo_map(compiled[0]->apple9_state_bo) +
                 first_offset,
              (uint8_t *)agx_bo_map(compiled[1]->apple9_state_bo) +
                 second_offset,
              AGX_APPLE9_COMPUTE_STATE_STRIDE) == 0)
      fail("successive state records are not distinguishable");

   run_formula(screen, ctx, state9, states[1]);
   run_formula(screen, ctx, state8, states[0]);

   uint64_t first_selector = compiled[0]->apple9_state_address;
   uint64_t second_selector = compiled[1]->apple9_state_address;
   for (unsigned i = 0; i < 2; ++i)
      ctx->delete_compute_state(ctx, states[i]);
   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   printf("T8132_GALLIUM_COMPUTE_STATE_APPEND_OK publications=3 "
          "same_slab=yes stride=%#x selectors=%#llx,%#llx "
          "renderer=\"%s\"\n",
          AGX_APPLE9_COMPUTE_STATE_STRIDE,
          (unsigned long long)first_selector,
          (unsigned long long)second_selector, renderer);
   return 0;
}

static int
run_state_slab_boundary(void)
{
   const struct formula_desc *state8 = find_formula("state8");
   const struct formula_desc *state9 = find_formula("state9");
   struct pipe_loader_device *device = find_asahi_device();
   if (!device)
      fail("Asahi Gallium device not found for state slab boundary test");

   struct pipe_screen *screen = pipe_loader_create_screen(device, false);
   if (!screen)
      fail("create Asahi Gallium screen for state slab boundary test");
   const char *name = screen->get_name(screen);
   if (!name || strstr(name, "Apple M4") == NULL)
      fail("unexpected state slab boundary Gallium renderer");
   char renderer[128];
   snprintf(renderer, sizeof(renderer), "%s", name);

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   if (!ctx)
      fail("create Gallium context for state slab boundary test");

   void *states[2] = {create_formula_state(screen, ctx, state8), NULL};
   struct agx_compiled_shader *compiled[2] = {
      compiled_shader_from_state(states[0]), NULL,
   };
   run_formula(screen, ctx, state8, states[0]);

   /* Slot zero belongs to state8.  Author every remaining record only after
    * that selector has executed, then require state9 to roll to a fresh slab.
    * The dummy records deliberately model compile tombstones: they consume
    * addresses but are never selected by a command.
    */
   struct agx_device *dev = agx_device(screen);
   uint64_t last_tombstone = 0;
   for (unsigned slot = 1; slot < 256; ++slot) {
      struct agx_bo *bo = NULL;
      void *record = NULL;
      uint64_t selector = 0;
      if (!agx_apple9_alloc_compute_state(
             dev, &bo, &record, &selector))
         fail("allocate state slab tombstone");
      if (bo != compiled[0]->apple9_state_bo ||
          selector != compiled[0]->apple9_state_address +
                         slot * AGX_APPLE9_COMPUTE_STATE_STRIDE)
         fail("state slab did not fill monotonically through slot 255");
      memset(record, (int)(slot ^ 0xa5u),
             AGX_APPLE9_COMPUTE_STATE_STRIDE);
      last_tombstone = selector;
      agx_bo_unreference(dev, bo);
   }

   states[1] = create_formula_state(screen, ctx, state9);
   compiled[1] = compiled_shader_from_state(states[1]);
   if (!compiled[1]->apple9_state_bo ||
       compiled[1]->apple9_state_bo == compiled[0]->apple9_state_bo)
      fail("state record 256 did not roll to a fresh slab");
   if (compiled[1]->apple9_state_address !=
       compiled[1]->apple9_state_bo->va->addr + STATE_SELECTOR_OFFSET)
      fail("fresh state slab did not restart at slot zero");

   run_formula(screen, ctx, state9, states[1]);
   run_formula(screen, ctx, state8, states[0]);

   uint64_t first_selector = compiled[0]->apple9_state_address;
   uint64_t rollover_selector = compiled[1]->apple9_state_address;
   for (unsigned i = 0; i < 2; ++i)
      ctx->delete_compute_state(ctx, states[i]);
   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   printf("T8132_GALLIUM_COMPUTE_STATE_SLAB_BOUNDARY_OK "
          "publications=3 records=257 first=%#llx last=%#llx "
          "rollover=%#llx renderer=\"%s\"\n",
          (unsigned long long)first_selector,
          (unsigned long long)last_tombstone,
          (unsigned long long)rollover_selector, renderer);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
      return self_test();
   if (argc == 2 && strcmp(argv[1], "--dual-vm-state-alias") == 0)
      return run_dual_vm_state_alias();
   if (argc == 2 && strcmp(argv[1], "--state-append-after-use") == 0)
      return run_state_append_after_use();
   if (argc == 2 && strcmp(argv[1], "--state-slab-boundary") == 0)
      return run_state_slab_boundary();

   const struct formula_desc *selected[FORMULA_COUNT];
   unsigned selected_count = 0;
   bool precompile_all = false;
   if (argc == 3 && strcmp(argv[1], "--case") == 0) {
      selected[0] = find_formula(argv[2]);
      if (!selected[0]) {
         fprintf(stderr, "unknown formula: %s\n", argv[2]);
         return 2;
      }
      selected_count = 1;
   } else if (argc >= 3 && strcmp(argv[1], "--sequence") == 0) {
      if ((unsigned)(argc - 2) > FORMULA_COUNT) {
         fprintf(stderr, "formula sequence is too long\n");
         return 2;
      }
      for (int i = 2; i < argc; ++i) {
         selected[selected_count] = find_formula(argv[i]);
         if (!selected[selected_count]) {
            fprintf(stderr, "unknown formula: %s\n", argv[i]);
            return 2;
         }
         selected_count++;
      }
   } else if (argc == 2 && strcmp(argv[1], "--precompile-all") == 0) {
      precompile_all = true;
   } else if (argc != 1) {
      fprintf(stderr,
              "usage: %s [--self-test | --case NAME | "
              "--sequence NAME... | --precompile-all | "
              "--dual-vm-state-alias | --state-append-after-use | "
              "--state-slab-boundary]\n",
              argv[0]);
      return 2;
   }

   if (!selected_count) {
      for (unsigned i = 0; i < FORMULA_COUNT; ++i)
         selected[selected_count++] = &formulas[i];
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

   void *precompiled[FORMULA_COUNT] = {0};
   if (precompile_all) {
      for (unsigned i = 0; i < selected_count; ++i)
         precompiled[i] = create_formula_state(screen, ctx, selected[i]);
   }

   for (unsigned i = 0; i < selected_count; ++i)
      run_formula(screen, ctx, selected[i], precompiled[i]);

   if (precompile_all) {
      for (unsigned i = 0; i < selected_count; ++i)
         ctx->delete_compute_state(ctx, precompiled[i]);
   }

   if (selected_count == 1) {
      printf("T8132_GALLIUM_COMPUTE_DAG3_OK "
             "case=%s elements=64 local=32 "
             "offsets=0x1000,0x2000,0x3000 renderer=\"%s\"\n",
             selected[0]->name, renderer);
   } else {
      printf("T8132_GALLIUM_COMPUTE_DAG3_OK "
             "cases=%u precompiled=%s elements=64 local=32 "
             "offsets=0x1000,0x2000,0x3000 renderer=\"%s\"\n",
             selected_count, precompile_all ? "yes" : "no", renderer);
   }

   ctx->destroy(ctx);
   screen->destroy(screen);
   pipe_loader_release(&device, 1);

   return 0;
}
