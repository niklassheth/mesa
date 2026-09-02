/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_compile_apple9.h"
#include "agx_apple9_ir.h"

#include "compiler/nir/nir.h"
#include "util/u_dynarray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Apple9 compute compilation uses one NIR-to-VIR-to-machine pipeline. */

struct apple9_emitter {
   struct util_dynarray bytes;
   unsigned instructions;
};

static bool
apple9_emit_packed(struct apple9_emitter *emitter,
                   const struct agx_apple9_packed_instruction *packed);

static void
apple9_emit(struct apple9_emitter *emitter, unsigned length,
            const uint8_t *encoded)
{
   memcpy(util_dynarray_grow_bytes(&emitter->bytes, 1, length), encoded,
          length);
   emitter->instructions++;
}

static void
apple9_emit_stop(struct apple9_emitter *emitter)
{
   const uint8_t encoded[4] = {0x0e, 0, 0, 0};
   apple9_emit(emitter, sizeof(encoded), encoded);
}

/*
 * Gallium's GLSL path keeps SSBO offsets as a low/high pair until late NIR.
 * Peel only representation-preserving wrappers plus additions by zero.
 */
static nir_scalar
apple9_chase_trivial(nir_scalar scalar)
{
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      scalar = nir_scalar_chase_movs(scalar);
      if (nir_def_instr_type(scalar.def) != nir_instr_type_alu)
         return scalar;

      nir_op op = nir_scalar_alu_op(scalar);
      if (op == nir_op_vec2 || op == nir_op_vec3 || op == nir_op_vec4) {
         scalar = nir_scalar_chase_alu_src(scalar, scalar.comp);
         continue;
      }

      if (op == nir_op_iadd) {
         nir_scalar left =
            apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 0));
         nir_scalar right =
            apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
         if (nir_scalar_is_const(left) && nir_scalar_as_uint(left) == 0) {
            scalar = right;
            continue;
         }
         if (nir_scalar_is_const(right) && nir_scalar_as_uint(right) == 0) {
            scalar = left;
            continue;
         }
      }

      return scalar;
   }

   return scalar;
}

static bool
apple9_global_id_component(nir_scalar scalar, unsigned *component)
{
   scalar = apple9_chase_trivial(scalar);

   if (scalar.def->bit_size != 32 || scalar.comp >= 3 ||
       nir_def_instr_type(scalar.def) != nir_instr_type_intrinsic ||
       nir_def_as_intrinsic(scalar.def)->intrinsic !=
          nir_intrinsic_load_global_invocation_id)
      return false;

   if (component != NULL)
      *component = scalar.comp;
   return true;
}

struct apple9_system_source {
   uint8_t selector;
   bool zext16;
   bool global_id;
};

/*
 * EXP-M4-29 establishes two regular compute system-register forms.  Global
 * and workgroup positions are direct 32-bit values.  Local position/index,
 * SIMD indices, and workgroup-size components use the same narrow GET_SR plus
 * zero-extension pair and differ only in the selector.  Native asymmetric-3D
 * executions establish selectors 0x98..0x9a as the local-size XYZ tuple.
 *
 * Selectors 0xa8..0xaa are a different dispatch-time tuple used by Metal's
 * load_num_workgroups lowering.  Their bare values track the CDM local tuple,
 * while the public group count is derived from caller-owned global-thread
 * metadata.  Do not alias that packaging contract to load_workgroup_size.
 */
static bool
apple9_system_source(nir_scalar scalar, struct apple9_system_source *source)
{
   scalar = apple9_chase_trivial(scalar);
   if (scalar.def->bit_size != 32 ||
       nir_def_instr_type(scalar.def) != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_op op = nir_def_as_intrinsic(scalar.def)->intrinsic;
   unsigned components = scalar.def->num_components;
   uint8_t base;
   bool zext16 = false;
   bool global_id = false;

   switch (op) {
   case nir_intrinsic_load_global_invocation_id:
      base = 0xa0;
      global_id = true;
      break;
   case nir_intrinsic_load_workgroup_id:
      base = 0x9c;
      break;
   case nir_intrinsic_load_local_invocation_id:
      base = 0xa4;
      zext16 = true;
      break;
   case nir_intrinsic_load_workgroup_size:
      base = 0x98;
      zext16 = true;
      break;
   case nir_intrinsic_load_local_invocation_index:
      base = 0xa7;
      components = 1;
      zext16 = true;
      break;
   case nir_intrinsic_load_subgroup_invocation:
      base = 0x82;
      components = 1;
      zext16 = true;
      break;
   case nir_intrinsic_load_subgroup_id:
      base = 0x85;
      components = 1;
      zext16 = true;
      break;
   default:
      return false;
   }

   if (scalar.comp >= components || scalar.comp >= 3)
      return false;
   *source = (struct apple9_system_source){
      .selector = base + scalar.comp,
      .zext16 = zext16,
      .global_id = global_id,
   };
   return true;
}

static bool
apple9_const_u32(nir_scalar scalar, uint32_t *value)
{
   scalar = apple9_chase_trivial(scalar);
   if (!nir_scalar_is_const(scalar) || scalar.def->bit_size != 32)
      return false;

   *value = nir_scalar_as_uint(scalar);
   return true;
}

static bool
apple9_element_index(nir_def *offset, nir_scalar *index, unsigned *index_scale,
                     unsigned *index_add, unsigned element_size)
{
   if (element_size != 1 && element_size != 2 && element_size != 4)
      return false;
   nir_scalar scalar = apple9_chase_trivial(nir_get_scalar(offset, 0));
   uint32_t byte_add = 0;

   /* Peel a constant structure-field displacement.  The remaining expression
    * still names the semantic array index; scale/add are carried explicitly
    * into instruction selection and runtime bounds metadata. */
   if (nir_def_instr_type(scalar.def) == nir_instr_type_alu &&
       nir_scalar_alu_op(scalar) == nir_op_iadd) {
      nir_scalar left =
         apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 0));
      nir_scalar right =
         apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
      if (apple9_const_u32(right, &byte_add))
         scalar = left;
      else if (apple9_const_u32(left, &byte_add))
         scalar = right;
   }

   uint32_t byte_stride = 1;
   nir_scalar candidate = scalar;
   if (nir_def_instr_type(scalar.def) == nir_instr_type_alu) {
      nir_op op = nir_scalar_alu_op(scalar);
      if (op == nir_op_imul || op == nir_op_amul || op == nir_op_ishl) {
         nir_scalar left =
            apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 0));
         nir_scalar right =
            apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
         if (op == nir_op_ishl) {
            uint32_t shift;
            if (!apple9_const_u32(right, &shift) || shift >= 32)
               return false;
            byte_stride = 1u << shift;
            candidate = left;
         } else if (apple9_const_u32(right, &byte_stride)) {
            candidate = left;
         } else if (apple9_const_u32(left, &byte_stride)) {
            candidate = right;
         } else {
            return false;
         }
      }
   }

   if (byte_stride == 1 && element_size != 1) {
      /* A raw scalar offset is already an element index only for byte data. */
      return false;
   }

   if (byte_stride == 0 || byte_stride % element_size != 0 ||
       byte_add % element_size != 0) {
      return false;
   } else {
      candidate = apple9_chase_trivial(candidate);
   }
   if (candidate.def->bit_size != 32)
      return false;

   if (index != NULL)
      *index = candidate;
   if (index_scale != NULL)
      *index_scale = byte_stride / element_size;
   if (index_add != NULL)
      *index_add = byte_add / element_size;
   return true;
}

static bool
apple9_dword_index(nir_def *offset, nir_scalar *index)
{
   unsigned index_scale, index_add;
   return apple9_element_index(offset, index, &index_scale, &index_add,
                               sizeof(uint32_t)) &&
          index_scale == 1 && index_add == 0;
}

struct apple9_affine_index {
   uint64_t base;
   uint64_t stride[3];
};

struct apple9_scalar_load {
   nir_intrinsic_instr *intr;
   nir_scalar index;
   struct apple9_affine_index affine;
   bool bounded_index;
   uint32_t bounded_max;
   unsigned argument;
   unsigned component;
   unsigned index_scale;
   unsigned index_add;
   unsigned bit_size;
};

static bool
apple9_affine_scale(struct apple9_affine_index *value, uint64_t factor)
{
   if (value->base && factor > UINT32_MAX / value->base)
      return false;
   value->base *= factor;
   for (unsigned d = 0; d < 3; ++d) {
      if (value->stride[d] && factor > UINT32_MAX / value->stride[d])
         return false;
      value->stride[d] *= factor;
   }
   return true;
}

static bool
apple9_affine_index_inner(nir_scalar scalar, struct apple9_affine_index *value,
                          unsigned depth)
{
   if (depth >= 32)
      return false;
   scalar = apple9_chase_trivial(scalar);
   memset(value, 0, sizeof(*value));

   uint32_t constant;
   if (apple9_const_u32(scalar, &constant)) {
      value->base = constant;
      return true;
   }

   unsigned component;
   if (apple9_global_id_component(scalar, &component)) {
      value->stride[component] = 1;
      return true;
   }

   if (nir_def_instr_type(scalar.def) != nir_instr_type_alu)
      return false;
   nir_op op = nir_scalar_alu_op(scalar);
   nir_scalar left = apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 0));
   nir_scalar right = apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));

   if (op == nir_op_iadd) {
      struct apple9_affine_index a, b;
      if (!apple9_affine_index_inner(left, &a, depth + 1) ||
          !apple9_affine_index_inner(right, &b, depth + 1) ||
          a.base > UINT32_MAX - b.base)
         return false;
      value->base = a.base + b.base;
      for (unsigned d = 0; d < 3; ++d) {
         if (a.stride[d] > UINT32_MAX - b.stride[d])
            return false;
         value->stride[d] = a.stride[d] + b.stride[d];
      }
      return true;
   }

   uint32_t factor;
   nir_scalar affine;
   if (op == nir_op_ishl) {
      if (!apple9_const_u32(right, &factor) || factor >= 32)
         return false;
      factor = 1u << factor;
      affine = left;
   } else if (op == nir_op_imul || op == nir_op_amul) {
      if (apple9_const_u32(right, &factor))
         affine = left;
      else if (apple9_const_u32(left, &factor))
         affine = right;
      else
         return false;
   } else {
      return false;
   }

   return apple9_affine_index_inner(affine, value, depth + 1) &&
          apple9_affine_scale(value, factor);
}

static bool
apple9_affine_index(nir_scalar scalar, struct apple9_affine_index *value,
                    unsigned *rank)
{
   if (!apple9_affine_index_inner(scalar, value, 0) || value->base != 0 ||
       value->stride[0] != 1 || (value->stride[2] && !value->stride[1]))
      return false;

   *rank = value->stride[2] ? 3 : value->stride[1] ? 2 : 1;
   return true;
}

static bool
apple9_index_upper_bound_inner(nir_shader *nir, nir_scalar scalar,
                               uint32_t *maximum, unsigned depth)
{
   if (depth >= 32)
      return false;
   scalar = apple9_chase_trivial(scalar);
   if (apple9_const_u32(scalar, maximum))
      return true;

   if (nir_def_instr_type(scalar.def) == nir_instr_type_intrinsic) {
      nir_intrinsic_op op = nir_def_as_intrinsic(scalar.def)->intrinsic;
      uint64_t local_size = 1;
      for (unsigned d = 0; d < 3; ++d) {
         if (nir->info.workgroup_size[d] == 0)
            return false;
         local_size *= nir->info.workgroup_size[d];
      }

      switch (op) {
      case nir_intrinsic_load_local_invocation_id:
         if (scalar.comp >= 3)
            return false;
         *maximum = nir->info.workgroup_size[scalar.comp] - 1;
         return true;
      case nir_intrinsic_load_local_invocation_index:
         if (local_size > UINT32_MAX)
            return false;
         *maximum = local_size - 1;
         return true;
      case nir_intrinsic_load_subgroup_invocation:
         *maximum = 31;
         return true;
      case nir_intrinsic_load_subgroup_id:
         if (local_size > UINT32_MAX)
            return false;
         *maximum = DIV_ROUND_UP(local_size, 32) - 1;
         return true;
      case nir_intrinsic_load_subgroup_size:
         *maximum = 32;
         return true;
      case nir_intrinsic_load_workgroup_size:
         if (scalar.comp >= 3)
            return false;
         *maximum = nir->info.workgroup_size[scalar.comp];
         return true;
      default:
         return false;
      }
   }

   if (nir_def_instr_type(scalar.def) != nir_instr_type_alu)
      return false;

   nir_op op = nir_scalar_alu_op(scalar);
   nir_scalar left = apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 0));
   nir_scalar right = apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
   uint32_t a, b;

   switch (op) {
   case nir_op_iand:
      /* For an unsigned scalar, AND cannot set a bit absent from a constant
       * mask, regardless of the other source. */
      return apple9_const_u32(left, maximum) ||
             apple9_const_u32(right, maximum);
   case nir_op_umin: {
      bool have_a = apple9_index_upper_bound_inner(nir, left, &a, depth + 1);
      bool have_b = apple9_index_upper_bound_inner(nir, right, &b, depth + 1);
      if (!have_a && !have_b)
         return false;
      *maximum = have_a && have_b ? MIN2(a, b) : have_a ? a : b;
      return true;
   }
   case nir_op_iadd:
      if (!apple9_index_upper_bound_inner(nir, left, &a, depth + 1) ||
          !apple9_index_upper_bound_inner(nir, right, &b, depth + 1) ||
          a > UINT32_MAX - b)
         return false;
      *maximum = a + b;
      return true;
   case nir_op_imul:
   case nir_op_amul:
      if (!apple9_index_upper_bound_inner(nir, left, &a, depth + 1) ||
          !apple9_index_upper_bound_inner(nir, right, &b, depth + 1) ||
          (a != 0 && b > UINT32_MAX / a))
         return false;
      *maximum = a * b;
      return true;
   case nir_op_ishl:
      if (!apple9_index_upper_bound_inner(nir, left, &a, depth + 1) ||
          !apple9_const_u32(right, &b) || b >= 32 ||
          a > (UINT32_MAX >> b))
         return false;
      *maximum = a << b;
      return true;
   case nir_op_ushr:
      if (!apple9_index_upper_bound_inner(nir, left, &a, depth + 1) ||
          !apple9_const_u32(right, &b) || b >= 32)
         return false;
      *maximum = a >> b;
      return true;
   case nir_op_bcsel: {
      nir_scalar if_false =
         apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 2));
      if (!apple9_index_upper_bound_inner(nir, right, &a, depth + 1) ||
          !apple9_index_upper_bound_inner(nir, if_false, &b, depth + 1))
         return false;
      *maximum = MAX2(a, b);
      return true;
   }
   default:
      return false;
   }
}

static bool
apple9_index_upper_bound(nir_shader *nir, nir_scalar scalar, uint32_t *maximum)
{
   return apple9_index_upper_bound_inner(nir, scalar, maximum, 0);
}

static bool
apple9_instruction_is_in_subset(nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
      return true;
   case nir_instr_type_alu: {
      nir_op op = nir_instr_as_alu(instr)->op;
      switch (op) {
      case nir_op_mov:
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
      case nir_op_b2i32:
      case nir_op_bcsel:
      case nir_op_inot:
      case nir_op_ineg:
      case nir_op_fabs:
      case nir_op_fneg:
      case nir_op_u2f32:
      case nir_op_i2f32:
      case nir_op_f2i32:
      case nir_op_f2u32:
      case nir_op_i2i8:
      case nir_op_i2i16:
      case nir_op_i2i32:
      case nir_op_u2u8:
      case nir_op_u2u16:
      case nir_op_u2u32:
      case nir_op_iadd:
      case nir_op_isub:
      case nir_op_imul:
      case nir_op_amul:
      case nir_op_iand:
      case nir_op_ior:
      case nir_op_ixor:
      case nir_op_ishl:
      case nir_op_ishr:
      case nir_op_ushr:
      case nir_op_imin:
      case nir_op_imax:
      case nir_op_umin:
      case nir_op_umax:
      case nir_op_fadd:
      case nir_op_fsub:
      case nir_op_fmul:
      case nir_op_fmin:
      case nir_op_fmax:
      case nir_op_ffma:
      case nir_op_ffma_weak:
      case nir_op_ieq:
      case nir_op_ine:
      case nir_op_ilt:
      case nir_op_ige:
      case nir_op_ult:
      case nir_op_uge:
      case nir_op_feq:
      case nir_op_fneu:
      case nir_op_flt:
      case nir_op_fge:
         return true;
      default:
         return false;
      }
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
      return op == nir_intrinsic_load_global_invocation_id ||
             op == nir_intrinsic_load_workgroup_id ||
             op == nir_intrinsic_load_local_invocation_id ||
             op == nir_intrinsic_load_local_invocation_index ||
             op == nir_intrinsic_load_workgroup_size ||
             op == nir_intrinsic_load_subgroup_invocation ||
             op == nir_intrinsic_load_subgroup_id ||
             op == nir_intrinsic_load_subgroup_size ||
             op == nir_intrinsic_load_ssbo || op == nir_intrinsic_load_ubo ||
             op == nir_intrinsic_store_ssbo;
   }
   default:
      return false;
   }
}

struct apple9_dag_lower {
   struct agx_apple9_vir_program program;
   uint32_t *ssa_to_vreg;
   unsigned ssa_map_count;
   uint32_t system_vreg[256];
   uint32_t zero_vreg;
   struct apple9_scalar_load *loads;
   unsigned load_count;
   unsigned load_instruction_count;
   unsigned emitted_load_count;
   const char *reason;
};

static uint32_t
apple9_dag_emit(struct apple9_dag_lower *lower, enum agx_apple9_vir_opcode op,
                enum agx_apple9_encoding encoding, const uint32_t *src,
                unsigned nr_srcs, uint32_t immediate)
{
   uint32_t value = agx_apple9_vir_emit(&lower->program, op, encoding, src,
                                        nr_srcs, immediate);
   if (value == AGX_APPLE9_VREG_INVALID)
      lower->reason = "out of memory building Apple9 virtual IR";
   return value;
}

/* Extended integer OR is the general Apple9 bit-copy form available to this
 * compiler: x | x preserves every bit, accepts any r0-r63 source, and can
 * place the result outside compact destination banks. */
static uint32_t
apple9_dag_general_copy(struct apple9_dag_lower *lower, uint32_t source)
{
   if (source == AGX_APPLE9_VREG_INVALID)
      return source;
   const uint32_t sources[] = {source, source};
   return apple9_dag_emit(lower, AGX_APPLE9_VIR_IOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, sources,
                          ARRAY_SIZE(sources), 0);
}

static uint32_t
apple9_dag_emit_constrained(struct apple9_dag_lower *lower,
                            enum agx_apple9_vir_opcode op,
                            enum agx_apple9_encoding encoding,
                            const uint32_t *sources, unsigned nr_sources,
                            uint32_t immediate)
{
   return apple9_dag_general_copy(
      lower,
      apple9_dag_emit(lower, op, encoding, sources, nr_sources, immediate));
}

static uint32_t
apple9_dag_imm(struct apple9_dag_lower *lower, uint32_t value)
{
   return apple9_dag_emit_constrained(lower, AGX_APPLE9_VIR_IMM,
                                      AGX_APPLE9_ENC_MOV_IMM_COMPACT, NULL, 0,
                                      value);
}

static uint32_t
apple9_dag_zero(struct apple9_dag_lower *lower)
{
   if (lower->zero_vreg == AGX_APPLE9_VREG_INVALID)
      lower->zero_vreg = apple9_dag_imm(lower, 0);
   return lower->zero_vreg;
}

static enum agx_apple9_vir_opcode
apple9_dag_binary_opcode(nir_op op)
{
   switch (op) {
   case nir_op_iadd:
      return AGX_APPLE9_VIR_IADD;
   case nir_op_isub:
      return AGX_APPLE9_VIR_ISUB;
   case nir_op_iand:
      return AGX_APPLE9_VIR_IAND;
   case nir_op_ior:
      return AGX_APPLE9_VIR_IOR;
   case nir_op_ixor:
      return AGX_APPLE9_VIR_IXOR;
   case nir_op_imin:
      return AGX_APPLE9_VIR_IMIN;
   case nir_op_imax:
      return AGX_APPLE9_VIR_IMAX;
   case nir_op_umin:
      return AGX_APPLE9_VIR_UMIN;
   case nir_op_umax:
      return AGX_APPLE9_VIR_UMAX;
   case nir_op_fadd:
      return AGX_APPLE9_VIR_FADD;
   case nir_op_fsub:
      return AGX_APPLE9_VIR_FSUB;
   case nir_op_fmul:
      return AGX_APPLE9_VIR_FMUL;
   case nir_op_fmin:
      return AGX_APPLE9_VIR_FMIN;
   case nir_op_fmax:
      return AGX_APPLE9_VIR_FMAX;
   default:
      return (enum agx_apple9_vir_opcode) - 1;
   }
}

static enum agx_apple9_encoding
apple9_dag_binary_encoding(nir_op op)
{
   switch (op) {
   case nir_op_iadd:
   case nir_op_isub:
      return AGX_APPLE9_ENC_INT_ADD_EXTENDED;
   case nir_op_iand:
   case nir_op_ior:
   case nir_op_ixor:
      return AGX_APPLE9_ENC_LOGIC_EXTENDED;
   case nir_op_imin:
   case nir_op_imax:
   case nir_op_umin:
   case nir_op_umax:
   case nir_op_fmin:
   case nir_op_fmax:
      return AGX_APPLE9_ENC_MINMAX_COMPACT;
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
      return AGX_APPLE9_ENC_FLOAT2_COMPACT;
   default:
      return AGX_APPLE9_ENC_COUNT;
   }
}

static uint32_t apple9_lower_dag_scalar(struct apple9_dag_lower *lower,
                                        nir_scalar scalar);

static uint32_t
apple9_lower_dag_source(struct apple9_dag_lower *lower, nir_scalar parent,
                        unsigned source)
{
   return apple9_lower_dag_scalar(
      lower, apple9_chase_trivial(nir_scalar_chase_alu_src(parent, source)));
}

static bool
apple9_select_condition(nir_op op, uint32_t *immediate)
{
   switch (op) {
   case nir_op_feq:
      *immediate = AGX_APPLE9_SELECT_FEQ | AGX_APPLE9_SELECT_EQUALITY;
      return true;
   case nir_op_fneu:
      *immediate = AGX_APPLE9_SELECT_FEQ | AGX_APPLE9_SELECT_EQUALITY;
      return true;
   case nir_op_flt:
      *immediate = AGX_APPLE9_SELECT_FLT;
      return true;
   case nir_op_fge:
      *immediate = AGX_APPLE9_SELECT_FGT | AGX_APPLE9_SELECT_EQUALITY;
      return true;
   case nir_op_ult:
      *immediate = AGX_APPLE9_SELECT_ULT;
      return true;
   case nir_op_ilt:
      *immediate = AGX_APPLE9_SELECT_ILT;
      return true;
   default:
      return false;
   }
}

static uint32_t
apple9_emit_dag_select_raw(struct apple9_dag_lower *lower, uint32_t cmp_a,
                           uint32_t cmp_b, uint32_t if_true, uint32_t if_false,
                           uint32_t immediate)
{
   uint32_t sources[4] = {cmp_a, cmp_b, if_true, if_false};
   for (unsigned i = 0; i < ARRAY_SIZE(sources); ++i) {
      if (sources[i] == AGX_APPLE9_VREG_INVALID)
         return AGX_APPLE9_VREG_INVALID;
   }

   return apple9_dag_emit_constrained(lower, AGX_APPLE9_VIR_SELECT,
                                      AGX_APPLE9_ENC_SELECT_GPR_WIDE, sources,
                                      ARRAY_SIZE(sources), immediate);
}

static uint32_t
apple9_emit_dag_select(struct apple9_dag_lower *lower, nir_scalar predicate,
                       uint32_t if_true, uint32_t if_false)
{
   predicate = apple9_chase_trivial(predicate);
   nir_op op = nir_def_instr_type(predicate.def) == nir_instr_type_alu
                  ? nir_scalar_alu_op(predicate)
                  : nir_op_mov;
   if (nir_def_instr_type(predicate.def) != nir_instr_type_alu) {
      lower->reason = "Apple9 DAG select requires a supported comparison";
      return AGX_APPLE9_VREG_INVALID;
   }

   uint32_t cmp_a = apple9_lower_dag_source(lower, predicate, 0);
   uint32_t cmp_b = apple9_lower_dag_source(lower, predicate, 1);
   if (cmp_a == AGX_APPLE9_VREG_INVALID || cmp_b == AGX_APPLE9_VREG_INVALID)
      return AGX_APPLE9_VREG_INVALID;

   /* Flipping the sign bit maps two's-complement order to unsigned order. */
   if (op == nir_op_ilt || op == nir_op_ige) {
      uint32_t sign_a = apple9_dag_imm(lower, 0x80000000u);
      uint32_t sign_b = apple9_dag_imm(lower, 0x80000000u);
      uint32_t biased_a_sources[2] = {cmp_a, sign_a};
      uint32_t biased_b_sources[2] = {cmp_b, sign_b};
      uint32_t biased_a = apple9_dag_emit(
         lower, AGX_APPLE9_VIR_IXOR, AGX_APPLE9_ENC_LOGIC_EXTENDED,
         biased_a_sources, ARRAY_SIZE(biased_a_sources), 0);
      uint32_t biased_b = apple9_dag_emit(
         lower, AGX_APPLE9_VIR_IXOR, AGX_APPLE9_ENC_LOGIC_EXTENDED,
         biased_b_sources, ARRAY_SIZE(biased_b_sources), 0);
      return apple9_emit_dag_select_raw(
         lower, biased_a, biased_b, op == nir_op_ige ? if_false : if_true,
         op == nir_op_ige ? if_true : if_false, AGX_APPLE9_SELECT_ULT);
   }

   if (op == nir_op_uge) {
      return apple9_emit_dag_select_raw(lower, cmp_a, cmp_b, if_false, if_true,
                                        AGX_APPLE9_SELECT_ULT);
   }

   if (op == nir_op_ieq || op == nir_op_ine) {
      bool not_equal = op == nir_op_ine;
      uint32_t xor_sources[2] = {cmp_a, cmp_b};
      uint32_t difference = apple9_dag_emit(
         lower, AGX_APPLE9_VIR_IXOR, AGX_APPLE9_ENC_LOGIC_EXTENDED, xor_sources,
         ARRAY_SIZE(xor_sources), 0);
      uint32_t one = apple9_dag_imm(lower, 1);
      return apple9_emit_dag_select_raw(
         lower, difference, one, not_equal ? if_false : if_true,
         not_equal ? if_true : if_false, AGX_APPLE9_SELECT_ULT);
   }

   uint32_t immediate;
   if (!apple9_select_condition(op, &immediate)) {
      lower->reason = "Apple9 DAG select requires a supported comparison";
      return AGX_APPLE9_VREG_INVALID;
   }

   if (op == nir_op_fneu) {
      uint32_t temporary = if_true;
      if_true = if_false;
      if_false = temporary;
   }

   return apple9_emit_dag_select_raw(lower, cmp_a, cmp_b, if_true, if_false,
                                     immediate);
}

static uint32_t
apple9_dag_shift_imm(struct apple9_dag_lower *lower, nir_op op,
                     uint32_t source, unsigned amount)
{
   if (source == AGX_APPLE9_VREG_INVALID || amount >= 32)
      return AGX_APPLE9_VREG_INVALID;
   if (amount == 0)
      return source;

   if (op == nir_op_ishl) {
      uint32_t scale = apple9_dag_imm(lower, 1u << amount);
      uint32_t zero = apple9_dag_zero(lower);
      uint32_t sources[3] = {source, scale, zero};
      if (scale == AGX_APPLE9_VREG_INVALID ||
          zero == AGX_APPLE9_VREG_INVALID)
         return AGX_APPLE9_VREG_INVALID;
      return apple9_dag_emit(lower, AGX_APPLE9_VIR_IMAD,
                             AGX_APPLE9_ENC_INT_MAD_EXTENDED, sources,
                             ARRAY_SIZE(sources), 0);
   }

   /* The immediate ASHR fields are executed only in the proven compact bank.
    * A longer variable-shift graph exposed that the earlier byte-diff-derived
    * r16-r63 interpretation reads zero on T8132.  The shift's machine
    * constraints and source-class propagation place both values in an
    * arbitrary r0-r15 pair; copy the result back to the general bank. */
   const uint32_t copy_sources[2] = {source, source};
   uint32_t compact_source =
      apple9_dag_emit(lower, AGX_APPLE9_VIR_IOR,
                      AGX_APPLE9_ENC_LOGIC_EXTENDED, copy_sources,
                      ARRAY_SIZE(copy_sources), 0);
   if (compact_source == AGX_APPLE9_VREG_INVALID)
      return AGX_APPLE9_VREG_INVALID;
   uint32_t shifted = apple9_dag_emit(lower, AGX_APPLE9_VIR_ISHR,
                                      AGX_APPLE9_ENC_SHIFT_EXTENDED,
                                      &compact_source, 1, amount);
   if (shifted == AGX_APPLE9_VREG_INVALID)
      return AGX_APPLE9_VREG_INVALID;
   shifted = apple9_dag_general_copy(lower, shifted);
   if (op != nir_op_ushr || shifted == AGX_APPLE9_VREG_INVALID)
      return shifted;

   uint32_t mask = apple9_dag_imm(lower, UINT32_MAX >> amount);
   uint32_t sources[2] = {shifted, mask};
   return mask == AGX_APPLE9_VREG_INVALID
             ? AGX_APPLE9_VREG_INVALID
             : apple9_dag_emit(lower, AGX_APPLE9_VIR_IAND,
                               AGX_APPLE9_ENC_LOGIC_EXTENDED, sources,
                               ARRAY_SIZE(sources), 0);
}

static uint32_t
apple9_dag_shift_variable(struct apple9_dag_lower *lower, nir_op op,
                          uint32_t source, uint32_t amount)
{
   if (source == AGX_APPLE9_VREG_INVALID ||
       amount == AGX_APPLE9_VREG_INVALID)
      return AGX_APPLE9_VREG_INVALID;

   uint32_t result = source;
   uint32_t zero = apple9_dag_zero(lower);
   if (zero == AGX_APPLE9_VREG_INVALID)
      return zero;

   /* NIR shifts consume the low five amount bits.  A five-stage conditional
    * barrel shift is a correctness lowering built entirely from validated
    * Apple9 operations; the incompletely decoded native register-shift form
    * can replace it later as an instruction-selection optimization. */
   for (unsigned bit = 0; bit < 5; ++bit) {
      uint32_t mask = apple9_dag_imm(lower, BITFIELD_BIT(bit));
      uint32_t condition_sources[2] = {amount, mask};
      uint32_t condition =
         mask == AGX_APPLE9_VREG_INVALID
            ? AGX_APPLE9_VREG_INVALID
            : apple9_dag_emit(lower, AGX_APPLE9_VIR_IAND,
                              AGX_APPLE9_ENC_LOGIC_EXTENDED,
                              condition_sources,
                              ARRAY_SIZE(condition_sources), 0);
      uint32_t shifted =
         apple9_dag_shift_imm(lower, op, result, BITFIELD_BIT(bit));
      if (condition == AGX_APPLE9_VREG_INVALID ||
          shifted == AGX_APPLE9_VREG_INVALID)
         return AGX_APPLE9_VREG_INVALID;
      result = apple9_emit_dag_select_raw(
         lower, zero, condition, shifted, result, AGX_APPLE9_SELECT_ULT);
      if (result == AGX_APPLE9_VREG_INVALID)
         return result;
   }

   return result;
}

static uint32_t
apple9_lower_dag_scalar(struct apple9_dag_lower *lower, nir_scalar scalar)
{
   scalar = apple9_chase_trivial(scalar);
   if ((scalar.def->bit_size != 8 && scalar.def->bit_size != 16 &&
        scalar.def->bit_size != 32) ||
       scalar.comp >= 4) {
      lower->reason = "Apple9 DAG compiler supports 8-, 16- and 32-bit scalar components";
      return AGX_APPLE9_VREG_INVALID;
   }

   unsigned key = scalar.def->index * 4 + scalar.comp;
   if (key >= lower->ssa_map_count) {
      lower->reason = "Apple9 DAG compiler encountered an invalid SSA index";
      return AGX_APPLE9_VREG_INVALID;
   }
   if (lower->ssa_to_vreg[key] != AGX_APPLE9_VREG_INVALID)
      return lower->ssa_to_vreg[key];

   uint32_t value = AGX_APPLE9_VREG_INVALID;
   uint32_t constant;
   if (nir_scalar_is_const(scalar)) {
      constant = nir_scalar_as_uint(scalar);
      if (scalar.def->bit_size < 32)
         constant &= BITFIELD_MASK(scalar.def->bit_size);
      value = apple9_dag_imm(lower, constant);
   } else {
      struct apple9_system_source system;
      const bool subgroup_size =
         nir_def_instr_type(scalar.def) == nir_instr_type_intrinsic &&
         nir_def_as_intrinsic(scalar.def)->intrinsic ==
            nir_intrinsic_load_subgroup_size;
      if (subgroup_size) {
         /* Native Metal materializes the architectural SIMD width. */
         value = apple9_dag_imm(lower, 32);
      } else if (apple9_system_source(scalar, &system)) {
         if (lower->system_vreg[system.selector] == AGX_APPLE9_VREG_INVALID) {
            enum agx_apple9_vir_opcode op = system.global_id
                                               ? AGX_APPLE9_VIR_GET_GLOBAL_ID
                                               : AGX_APPLE9_VIR_GET_SR;
            enum agx_apple9_encoding encoding =
               system.zext16 ? AGX_APPLE9_ENC_GET_SR_ZEXT16
                             : AGX_APPLE9_ENC_GET_SR;
            uint32_t immediate =
               system.global_id
                  ? system.selector - 0xa0
                  : system.selector | (system.zext16 ? 0 : (0x10u << 8));
            /* Both GET_SR families have a proven low destination contract.
             * Immediately move the value to the general bank so the rest of
             * instruction selection does not inherit that constraint. */
            lower->system_vreg[system.selector] = apple9_dag_emit_constrained(
               lower, op, encoding, NULL, 0, immediate);
         }
         value = lower->system_vreg[system.selector];
      } else if (nir_def_instr_type(scalar.def) == nir_instr_type_intrinsic &&
                 (nir_def_as_intrinsic(scalar.def)->intrinsic ==
                     nir_intrinsic_load_ssbo ||
                  nir_def_as_intrinsic(scalar.def)->intrinsic ==
                     nir_intrinsic_load_ubo)) {
         struct apple9_scalar_load *load = NULL;
         for (unsigned i = 0; i < lower->load_count; ++i) {
            if (&lower->loads[i].intr->def == scalar.def &&
                lower->loads[i].component == scalar.comp) {
               load = &lower->loads[i];
               break;
            }
         }
         if (load == NULL) {
            lower->reason =
               "Apple9 DAG load is absent from the resource ledger";
            return AGX_APPLE9_VREG_INVALID;
         }

         uint32_t index = apple9_lower_dag_scalar(lower, load->index);
         if (index == AGX_APPLE9_VREG_INVALID)
            return index;

         unsigned read_count = 0;
         for (unsigned i = 0; i < lower->load_count; ++i)
            read_count += lower->loads[i].intr == load->intr;

         /* Preserve a native NIR vector access whenever at least two lanes
          * survive.  The memory format supplies the std430 element stride
          * (2 dwords for vec2, 4 for vec3/vec4), while one scoreboard slot
          * covers the complete adjacent destination tuple.  Loading an
          * unused lane is preferable to splitting one semantic vector access
          * into independently scheduled scalar producers. */
         if (read_count >= 2) {
            const unsigned components = load->intr->def.num_components;
            const unsigned expected_stride = components == 2 ? 2 : 4;
            if (components < 2 || components > 4 ||
                load->index_scale != expected_stride ||
                load->index_add != 0) {
               lower->reason =
                  "Apple9 native vector load requires a std430 vector stride";
               return AGX_APPLE9_VREG_INVALID;
            }

            uint8_t flags = lower->load_instruction_count == 1
                               ? AGX_APPLE9_DEVICE_LOAD_FIRST
                               : (lower->emitted_load_count + 1 <
                                        lower->load_instruction_count
                                     ? AGX_APPLE9_DEVICE_LOAD_HAS_NEXT
                                     : 0);
            const struct agx_apple9_device_load_contract contract = {
               .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
               .group_flags = flags,
               .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
            };
            const uint32_t base = agx_apple9_vir_emit_device_load_vector(
               &lower->program, load->argument, index, components, &contract);
            if (base == AGX_APPLE9_VREG_INVALID ||
                !agx_apple9_vir_set_device_load_contract(
                   &lower->program, base, flags,
                   AGX_APPLE9_SCOREBOARD_SLOT_AUTO)) {
               lower->reason =
                  "could not describe an Apple9 native vector load";
               return AGX_APPLE9_VREG_INVALID;
            }

            for (unsigned c = 0; c < components; ++c) {
               const unsigned lane_key = scalar.def->index * 4 + c;
               lower->ssa_to_vreg[lane_key] = base + c;
            }
            ++lower->emitted_load_count;
            return lower->ssa_to_vreg[key];
         }

         if (load->index_scale > 1) {
            uint32_t scale = apple9_dag_imm(lower, load->index_scale);
            uint32_t zero = apple9_dag_zero(lower);
            uint32_t sources[3] = {index, scale, zero};
            if (scale == AGX_APPLE9_VREG_INVALID ||
                zero == AGX_APPLE9_VREG_INVALID)
               return AGX_APPLE9_VREG_INVALID;
            index = apple9_dag_emit(lower, AGX_APPLE9_VIR_IMAD,
                                    AGX_APPLE9_ENC_INT_MAD_EXTENDED, sources,
                                    ARRAY_SIZE(sources), 0);
            if (index == AGX_APPLE9_VREG_INVALID)
               return index;
         }

         const unsigned element_add = load->index_add + load->component;
         if (element_add != 0) {
            uint32_t component = apple9_dag_imm(lower, element_add);
            uint32_t sources[2] = {index, component};
            if (component == AGX_APPLE9_VREG_INVALID)
               return component;
            index = apple9_dag_emit(lower, AGX_APPLE9_VIR_IADD,
                                    AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources,
                                    ARRAY_SIZE(sources), 0);
            if (index == AGX_APPLE9_VREG_INVALID)
               return index;
         }

         const uint32_t source[] = {index};
         value = apple9_dag_emit(lower, AGX_APPLE9_VIR_DEVICE_LOAD,
                                 AGX_APPLE9_ENC_DEVICE_LOAD, source, 1,
                                 load->argument);
         if (value != AGX_APPLE9_VREG_INVALID)
            lower->program.instructions[lower->program.instruction_count - 1]
               .memory_bits = load->bit_size;
         uint8_t flags =
            lower->load_instruction_count == 1
               ? AGX_APPLE9_DEVICE_LOAD_FIRST
               : (lower->emitted_load_count + 1 < lower->load_instruction_count
                     ? AGX_APPLE9_DEVICE_LOAD_HAS_NEXT
                     : 0);
         if (value == AGX_APPLE9_VREG_INVALID ||
             !agx_apple9_vir_set_device_load_contract(
                &lower->program, value, flags,
                AGX_APPLE9_SCOREBOARD_SLOT_AUTO)) {
            lower->reason = lower->reason != NULL
                               ? lower->reason
                               : "could not describe an Apple9 device load";
            return AGX_APPLE9_VREG_INVALID;
         }
         ++lower->emitted_load_count;
      } else if (nir_def_instr_type(scalar.def) == nir_instr_type_alu) {
         nir_op op = nir_scalar_alu_op(scalar);

         if (op == nir_op_i2i8 || op == nir_op_i2i16 || op == nir_op_i2i32 ||
             op == nir_op_u2u8 || op == nir_op_u2u16 || op == nir_op_u2u32) {
            nir_scalar source_scalar = apple9_chase_trivial(
               nir_scalar_chase_alu_src(scalar, 0));
            const unsigned source_bits = source_scalar.def->bit_size;
            const unsigned destination_bits = scalar.def->bit_size;
            uint32_t source = apple9_lower_dag_scalar(lower, source_scalar);
            if (source == AGX_APPLE9_VREG_INVALID)
               return source;

            if (destination_bits <= source_bits) {
               /* Narrowing is a semantic truncation.  Keep the low bits in
                * the same SSA value; a narrow store consumes exactly those
                * bits, while any later widening emits its own extension. */
               value = source;
            } else if (op == nir_op_u2u8 || op == nir_op_u2u16 ||
                       op == nir_op_u2u32) {
               uint32_t mask = apple9_dag_imm(
                  lower, source_bits == 32 ? UINT32_MAX
                                           : BITFIELD_MASK(source_bits));
               uint32_t sources[2] = {source, mask};
               if (mask != AGX_APPLE9_VREG_INVALID)
                  value = apple9_dag_emit(
                     lower, AGX_APPLE9_VIR_IAND,
                     AGX_APPLE9_ENC_LOGIC_EXTENDED, sources,
                     ARRAY_SIZE(sources), 0);
            } else {
               const unsigned shift = 32 - source_bits;
               uint32_t scale = apple9_dag_imm(lower, 1u << shift);
               uint32_t zero = apple9_dag_zero(lower);
               uint32_t sources[3] = {source, scale, zero};
               uint32_t shifted =
                  scale == AGX_APPLE9_VREG_INVALID ||
                        zero == AGX_APPLE9_VREG_INVALID
                     ? AGX_APPLE9_VREG_INVALID
                     : apple9_dag_emit(
                          lower, AGX_APPLE9_VIR_IMAD,
                          AGX_APPLE9_ENC_INT_MAD_EXTENDED, sources,
                          ARRAY_SIZE(sources), 0);
               if (shifted != AGX_APPLE9_VREG_INVALID)
                  value = apple9_dag_shift_imm(lower, nir_op_ishr, shifted,
                                               shift);
            }
         } else if (op == nir_op_bcsel) {
            uint32_t if_true = apple9_lower_dag_source(lower, scalar, 1);
            uint32_t if_false = apple9_lower_dag_source(lower, scalar, 2);
            if (if_true != AGX_APPLE9_VREG_INVALID &&
                if_false != AGX_APPLE9_VREG_INVALID) {
               value = apple9_emit_dag_select(
                  lower, nir_scalar_chase_alu_src(scalar, 0), if_true,
                  if_false);
            }
         } else if (op == nir_op_b2i32) {
            uint32_t if_true = apple9_dag_imm(lower, 1);
            uint32_t if_false = apple9_dag_zero(lower);
            if (if_true != AGX_APPLE9_VREG_INVALID &&
                if_false != AGX_APPLE9_VREG_INVALID) {
               value = apple9_emit_dag_select(
                  lower, nir_scalar_chase_alu_src(scalar, 0), if_true,
                  if_false);
            }
         } else if (op == nir_op_u2f32 || op == nir_op_i2f32 ||
                    op == nir_op_f2i32 || op == nir_op_f2u32) {
            uint32_t source = apple9_lower_dag_source(lower, scalar, 0);
            if (source != AGX_APPLE9_VREG_INVALID) {
               enum agx_apple9_vir_opcode vir_op =
                  op == nir_op_u2f32   ? AGX_APPLE9_VIR_U2F32
                  : op == nir_op_i2f32 ? AGX_APPLE9_VIR_I2F32
                  : op == nir_op_f2i32 ? AGX_APPLE9_VIR_F2I32
                                       : AGX_APPLE9_VIR_F2U32;
               enum agx_apple9_encoding encoding =
                  op == nir_op_u2f32   ? AGX_APPLE9_ENC_UINT_TO_FLOAT
                  : op == nir_op_i2f32 ? AGX_APPLE9_ENC_SINT_TO_FLOAT
                  : op == nir_op_f2i32 ? AGX_APPLE9_ENC_FLOAT_TO_SINT
                                       : AGX_APPLE9_ENC_FLOAT_TO_UINT;
               value = apple9_dag_emit(lower, vir_op, encoding, &source, 1, 0);
            }
         } else if (op == nir_op_inot || op == nir_op_fneg ||
                    op == nir_op_fabs || op == nir_op_ineg) {
            uint32_t source = apple9_lower_dag_source(lower, scalar, 0);
            if (source == AGX_APPLE9_VREG_INVALID)
               return source;
            uint32_t immediate = op == nir_op_fneg   ? 0x80000000u
                                 : op == nir_op_fabs ? 0x7fffffffu
                                 : op == nir_op_inot ? UINT32_MAX
                                                     : 0;
            uint32_t other = apple9_dag_imm(lower, immediate);
            if (other == AGX_APPLE9_VREG_INVALID)
               return other;
            uint32_t sources[2];
            enum agx_apple9_vir_opcode vir_op;
            enum agx_apple9_encoding encoding;
            if (op == nir_op_ineg) {
               sources[0] = other;
               sources[1] = source;
               vir_op = AGX_APPLE9_VIR_ISUB;
               encoding = AGX_APPLE9_ENC_INT_ADD_EXTENDED;
            } else {
               sources[0] = source;
               sources[1] = other;
               vir_op =
                  op == nir_op_fabs ? AGX_APPLE9_VIR_IAND : AGX_APPLE9_VIR_IXOR;
               encoding = AGX_APPLE9_ENC_LOGIC_EXTENDED;
            }
            value = apple9_dag_emit(lower, vir_op, encoding, sources, 2, 0);
         } else if (op == nir_op_imul || op == nir_op_amul) {
            uint32_t sources[3] = {
               apple9_lower_dag_source(lower, scalar, 0),
               apple9_lower_dag_source(lower, scalar, 1),
               apple9_dag_zero(lower),
            };
            if (sources[0] != AGX_APPLE9_VREG_INVALID &&
                sources[1] != AGX_APPLE9_VREG_INVALID &&
                sources[2] != AGX_APPLE9_VREG_INVALID)
               value = apple9_dag_emit(lower, AGX_APPLE9_VIR_IMAD,
                                       AGX_APPLE9_ENC_INT_MAD_EXTENDED, sources,
                                       3, 0);
         } else if (op == nir_op_ishl) {
            nir_scalar shift =
               apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
            uint32_t amount;
            uint32_t source = apple9_lower_dag_source(lower, scalar, 0);
            value = apple9_const_u32(shift, &amount) && amount < 32
                       ? apple9_dag_shift_imm(lower, op, source, amount)
                       : apple9_dag_shift_variable(
                            lower, op, source,
                            apple9_lower_dag_scalar(lower, shift));
         } else if (op == nir_op_ishr || op == nir_op_ushr) {
            nir_scalar shift =
               apple9_chase_trivial(nir_scalar_chase_alu_src(scalar, 1));
            uint32_t amount;
            uint32_t source = apple9_lower_dag_source(lower, scalar, 0);
            value = apple9_const_u32(shift, &amount) && amount < 32
                       ? apple9_dag_shift_imm(lower, op, source, amount)
                       : apple9_dag_shift_variable(
                            lower, op, source,
                            apple9_lower_dag_scalar(lower, shift));
         } else if (op == nir_op_ffma || op == nir_op_ffma_weak) {
            uint32_t sources[3] = {
               apple9_lower_dag_source(lower, scalar, 0),
               apple9_lower_dag_source(lower, scalar, 1),
               apple9_lower_dag_source(lower, scalar, 2),
            };
            if (sources[0] != AGX_APPLE9_VREG_INVALID &&
                sources[1] != AGX_APPLE9_VREG_INVALID &&
                sources[2] != AGX_APPLE9_VREG_INVALID)
               value = apple9_dag_emit(lower, AGX_APPLE9_VIR_FMA,
                                       AGX_APPLE9_ENC_FLOAT3_EXTENDED, sources,
                                       3, 0);
         } else {
            enum agx_apple9_vir_opcode vir_op = apple9_dag_binary_opcode(op);
            enum agx_apple9_encoding encoding = apple9_dag_binary_encoding(op);
            if (encoding != AGX_APPLE9_ENC_COUNT) {
               uint32_t sources[2] = {
                  apple9_lower_dag_source(lower, scalar, 0),
                  apple9_lower_dag_source(lower, scalar, 1),
               };
               if (sources[0] != AGX_APPLE9_VREG_INVALID &&
                   sources[1] != AGX_APPLE9_VREG_INVALID) {
                  const struct agx_apple9_operand_constraint *destination =
                     agx_apple9_find_operand(encoding, AGX_APPLE9_OPERAND_DEST);
                  value = destination != NULL && (destination->flags &
                                                  AGX_APPLE9_OPERAND_HARD_LOW)
                             ? apple9_dag_emit_constrained(
                                  lower, vir_op, encoding, sources, 2, 0)
                             : apple9_dag_emit(lower, vir_op, encoding, sources,
                                               2, 0);
               }
            }
         }
      }
   }

   if (value == AGX_APPLE9_VREG_INVALID && lower->reason == NULL)
      lower->reason =
         "Apple9 DAG compiler encountered an unsupported scalar operation";
   if (value != AGX_APPLE9_VREG_INVALID)
      lower->ssa_to_vreg[key] = value;
   return value;
}

static bool
apple9_find_dag_store(nir_shader *nir, nir_intrinsic_instr **store_out,
                      nir_scalar *index_out, uint8_t *binding_out,
                      unsigned *index_scale_out, unsigned *index_add_out,
                      const char **reason)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_intrinsic_instr *store = NULL;
   unsigned block_count = 0;

   nir_foreach_block(block, impl) {
      if (!exec_list_is_empty(&block->instr_list))
         ++block_count;
      nir_foreach_instr(instr, block) {
         if (!apple9_instruction_is_in_subset(instr)) {
            *reason = "Apple9 DAG compiler encountered unsupported NIR";
            return false;
         }
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_store_ssbo) {
            if (store != NULL) {
               *reason = "Apple9 DAG compiler accepts exactly one store";
               return false;
            }
            store = nir_instr_as_intrinsic(instr);
         }
      }
   }

   if (block_count != 1) {
      *reason = "Apple9 DAG compiler accepts one straight-line block";
      return false;
   }
   if (store == NULL) {
      *reason = "Apple9 DAG compiler requires one SSBO store";
      return false;
   }
   const unsigned components = store->src[0].ssa->num_components;
   const unsigned bit_size = store->src[0].ssa->bit_size;
   if (components < 1 || components > 4 ||
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(components) ||
       (bit_size != 8 && bit_size != 16 && bit_size != 32) ||
       (bit_size != 32 && components != 1)) {
      *reason = "Apple9 DAG compiler requires one complete scalar or u32 tuple store";
      return false;
   }
   nir_scalar binding =
      apple9_chase_trivial(nir_get_scalar(store->src[1].ssa, 0));
   if (!nir_scalar_is_const(binding) || nir_scalar_as_uint(binding) > UINT8_MAX) {
      *reason = "Apple9 DAG compiler requires a constant SSBO binding";
      return false;
   }
   unsigned index_scale, index_add;
   const unsigned expected_stride = components == 1   ? 1
                                    : components == 2 ? 2
                                                      : 4;
   if (!apple9_element_index(store->src[2].ssa, index_out, &index_scale,
                             &index_add,
                             store->src[0].ssa->bit_size / 8) ||
       (components > 1 &&
        (index_scale != expected_stride || index_add != 0))) {
      *reason = "Apple9 DAG compiler requires a natural 32-bit store stride";
      return false;
   }

   *store_out = store;
   *binding_out = nir_scalar_as_uint(binding);
   *index_scale_out = index_scale;
   *index_add_out = index_add;
   return true;
}

static bool
apple9_shader_has_buffer_load(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
            (nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_load_ssbo ||
             nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_load_ubo))
            return true;
      }
   }

   return false;
}

static bool
apple9_emit_packed(struct apple9_emitter *emitter,
                   const struct agx_apple9_packed_instruction *packed)
{
   if (packed->length == 0 || packed->length > sizeof(packed->bytes))
      return false;
   apple9_emit(emitter, packed->length, packed->bytes);
   return true;
}

struct apple9_buffer_resource {
   enum agx_apple9_compute_resource_kind kind;
   uint8_t binding;
};

struct apple9_buffer_map {
   unsigned input_count;
   struct apple9_buffer_resource resource[4];
};

static int
apple9_compare_buffer_resource(const void *a_, const void *b_)
{
   const struct apple9_buffer_resource *a = a_;
   const struct apple9_buffer_resource *b = b_;
   if (a->kind != b->kind)
      return (int)a->kind - (int)b->kind;
   return (int)b->binding - (int)a->binding;
}

/* Collect the semantic API bindings before selecting a package.  Native
 * package arguments remain compact: inputs occupy arguments 0..N-1 in
 * SSBO-before-UBO, descending-binding order and the sole SSBO output occupies
 * argument N.  The kind tie-break leaves every established all-SSBO argument
 * order byte-identical. */
static bool
apple9_collect_buffer_map(nir_shader *nir, struct apple9_buffer_map *map,
                          const char **reason)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   uint32_t output_binding = UINT32_MAX;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_ssbo &&
             intr->intrinsic != nir_intrinsic_load_ubo &&
             intr->intrinsic != nir_intrinsic_store_ssbo)
            continue;

         const bool store = intr->intrinsic == nir_intrinsic_store_ssbo;
         nir_def *binding_def = store ? intr->src[1].ssa : intr->src[0].ssa;
         uint32_t binding;
         if (!apple9_const_u32(nir_get_scalar(binding_def, 0), &binding) ||
             binding > UINT8_MAX) {
            *reason = "Apple9 buffer compiler requires constant bindings";
            return false;
         }

         if (store) {
            if (output_binding != UINT32_MAX && output_binding != binding) {
               *reason = "Apple9 SSBO compiler accepts one output binding";
               return false;
            }
            output_binding = binding;
            continue;
         }

         enum agx_apple9_compute_resource_kind kind =
            intr->intrinsic == nir_intrinsic_load_ubo
               ? AGX_APPLE9_COMPUTE_RESOURCE_UBO
               : AGX_APPLE9_COMPUTE_RESOURCE_SSBO;
         bool known = false;
         for (unsigned i = 0; i < map->input_count; ++i)
            known |= map->resource[i].kind == kind &&
                     map->resource[i].binding == binding;
         if (!known) {
            if (map->input_count == 3) {
               *reason = "Apple9 compiler supports one to three buffer inputs";
               return false;
            }
            map->resource[map->input_count++] =
               (struct apple9_buffer_resource){kind, binding};
         }
      }
   }

   if (output_binding == UINT32_MAX) {
      *reason = "Apple9 SSBO compiler requires one output binding";
      return false;
   }
   for (unsigned i = 0; i < map->input_count; ++i) {
      if (map->resource[i].kind == AGX_APPLE9_COMPUTE_RESOURCE_SSBO &&
          map->resource[i].binding == output_binding) {
         *reason = "Apple9 generic compiler requires distinct input and output bindings";
         return false;
      }
   }

   qsort(map->resource, map->input_count, sizeof(map->resource[0]),
         apple9_compare_buffer_resource);
   map->resource[map->input_count] = (struct apple9_buffer_resource){
      AGX_APPLE9_COMPUTE_RESOURCE_SSBO, output_binding};
   return true;
}

static unsigned
apple9_buffer_argument(const struct apple9_buffer_map *map,
                       enum agx_apple9_compute_resource_kind kind,
                       uint32_t binding)
{
   for (unsigned i = 0; i < map->input_count; ++i) {
      if (map->resource[i].kind == kind &&
          map->resource[i].binding == binding)
         return i;
   }
   return UINT_MAX;
}

/* Resource count selects package layout, never shader semantics. */
static bool
apple9_find_buffer_dag(nir_shader *nir, const struct apple9_buffer_map *map,
                       struct util_dynarray *loads,
                       nir_intrinsic_instr **store_out, nir_scalar *index_out,
                       struct apple9_affine_index *store_affine_out,
                       unsigned *store_rank_out, unsigned *store_scale_out,
                       unsigned *store_add_out, const char **reason)
{
   if (map->input_count < 1 || map->input_count > 3) {
      *reason = "Apple9 buffer compiler supports one to three inputs";
      return false;
   }

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_intrinsic_instr *store = NULL;
   unsigned block_count = 0;
   bool have_store_index = false;

   nir_foreach_block(block, impl) {
      if (!exec_list_is_empty(&block->instr_list))
         ++block_count;
      nir_foreach_instr(instr, block) {
         if (!apple9_instruction_is_in_subset(instr)) {
            *reason = "Apple9 buffer compiler encountered unsupported NIR";
            return false;
         }
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_ssbo &&
             intr->intrinsic != nir_intrinsic_load_ubo &&
             intr->intrinsic != nir_intrinsic_store_ssbo)
            continue;
         if (nir_intrinsic_access(intr) & (ACCESS_COHERENT | ACCESS_VOLATILE)) {
            *reason = "Apple9 SSBO compiler rejects volatile access";
            return false;
         }

         const bool load = intr->intrinsic != nir_intrinsic_store_ssbo;
         nir_def *offset = load ? intr->src[1].ssa : intr->src[2].ssa;
         nir_scalar index;
         unsigned index_scale, index_add;
         const unsigned bit_size =
            load ? intr->def.bit_size : intr->src[0].ssa->bit_size;
         struct apple9_affine_index affine = {0};
         uint32_t bounded_max = 0;
         if (!apple9_element_index(offset, &index, &index_scale, &index_add,
                                   bit_size / 8)) {
            *reason =
               "Apple9 buffer compiler requires naturally indexed scalar elements";
            return false;
         }
         bool affine_index = apple9_affine_index_inner(index, &affine, 0);
         bool bounded_index =
            !affine_index && apple9_index_upper_bound(nir, index, &bounded_max);
         if (!affine_index && !bounded_index) {
            *reason = "Apple9 buffer compiler cannot prove the load index bound";
            return false;
         }

         nir_def *binding_def = load ? intr->src[0].ssa : intr->src[1].ssa;
         uint32_t binding;
         if (!apple9_const_u32(nir_get_scalar(binding_def, 0), &binding)) {
            *reason = "Apple9 buffer compiler requires constant bindings";
            return false;
         }

         if (load) {
            if (intr->def.num_components == 0 || intr->def.num_components > 4 ||
                (intr->def.bit_size != 8 && intr->def.bit_size != 16 &&
                 intr->def.bit_size != 32) ||
                (intr->def.bit_size != 32 && intr->def.num_components != 1)) {
               *reason =
                  "Apple9 inputs must be scalar 8/16-bit or one-to-four-component 32-bit buffer loads";
               return false;
            }
            enum agx_apple9_compute_resource_kind kind =
               intr->intrinsic == nir_intrinsic_load_ubo
                  ? AGX_APPLE9_COMPUTE_RESOURCE_UBO
                  : AGX_APPLE9_COMPUTE_RESOURCE_SSBO;
            unsigned argument = apple9_buffer_argument(map, kind, binding);
            if (argument == UINT_MAX) {
               *reason = "Apple9 input binding is absent from its resource map";
               return false;
            }
            const nir_component_mask_t read_mask =
               nir_def_components_read(&intr->def);
            for (unsigned component = 0; component < intr->def.num_components;
                 ++component) {
               if (!(read_mask & BITFIELD_BIT(component)))
                  continue;

               struct apple9_affine_index component_affine = affine;
               if (index_add > UINT32_MAX - component) {
                  *reason = "Apple9 vector-load field offset exceeds 32 bits";
                  return false;
               }
               const uint32_t component_add = index_add + component;
               if (!apple9_affine_scale(&component_affine, index_scale) ||
                   component_affine.base > UINT32_MAX - component_add) {
                  *reason = "Apple9 vector-load index exceeds 32 bits";
                  return false;
               }
               component_affine.base += component_add;

               if (bounded_index &&
                   (bounded_max >
                    (UINT32_MAX - component_add) / index_scale)) {
                  *reason = "Apple9 bounded vector-load index exceeds 32 bits";
                  return false;
               }

               struct apple9_scalar_load load = {
                  .intr = intr,
                  .index = index,
                  .affine = component_affine,
                  .bounded_index = bounded_index,
                  .bounded_max =
                     bounded_index
                        ? bounded_max * index_scale + index_add + component
                        : 0,
                  .argument = argument,
                  .component = component,
                  .index_scale = index_scale,
                  .index_add = index_add,
                  .bit_size = intr->def.bit_size,
               };
               util_dynarray_append(loads, load);
            }
         } else {
            const unsigned components = intr->src[0].ssa->num_components;
            const unsigned expected_stride = components == 1   ? 1
                                             : components == 2 ? 2
                                                               : 4;
            if (components < 1 || components > 4 ||
                (components > 1 &&
                 (index_scale != expected_stride || index_add != 0))) {
               *reason =
                  "Apple9 store index must use its natural vector stride";
               return false;
            }
            if (!affine_index) {
               *reason = "Apple9 store index must be affine";
               return false;
            }
            if (store != NULL ||
                binding != map->resource[map->input_count].binding ||
                nir_intrinsic_write_mask(intr) != BITFIELD_MASK(components) ||
                (bit_size != 8 && bit_size != 16 && bit_size != 32) ||
                (bit_size != 32 && components != 1)) {
               *reason = "Apple9 requires one complete scalar or u32 tuple store";
               return false;
            }
            store = intr;
            *index_out = index;
            *store_affine_out = affine;
            *store_scale_out = index_scale;
            *store_add_out = index_add;
            have_store_index = true;
         }
      }
   }

   bool complete =
      block_count == 1 && have_store_index && store != NULL && loads->size != 0;
   if (!complete) {
      *reason =
         "Apple9 requires one block, at least one input load, and one output";
      return false;
   }

   if (!apple9_affine_index(*index_out, store_affine_out, store_rank_out)) {
      *reason = "Apple9 DAG compiler requires a dense affine store index";
      return false;
   }

   util_dynarray_foreach(loads, struct apple9_scalar_load, load) {
      if (load->bounded_index)
         continue;
      bool same_dense_shape =
         memcmp(load->affine.stride, store_affine_out->stride,
                sizeof(load->affine.stride)) == 0;
      bool one_dimensional = load->affine.stride[1] == 0 &&
                             load->affine.stride[2] == 0 &&
                             load->affine.stride[0] != 0;
      if (!same_dense_shape && !one_dimensional) {
         *reason =
            "Apple9 load bounds require a 1D or dense-dispatch affine index";
         return false;
      }
   }

   *store_out = store;
   return true;
}

static bool
apple9_emit_dag_constant(struct apple9_emitter *emitter, unsigned dst,
                         unsigned accumulator, uint32_t value,
                         const char **reason)
{
   struct agx_apple9_packed_instruction packed;
   if (value <= 0x7f) {
      if (!agx_apple9_pack_mov_imm(dst, value, &packed))
         goto pack_error;
      return apple9_emit_packed(emitter, &packed);
   }

   unsigned highest = 7;
   while (highest > 0 && ((value >> (highest * 4)) & 0xf) == 0)
      --highest;
   if (!agx_apple9_pack_mov_imm(accumulator, (value >> (highest * 4)) & 0xf,
                                &packed) ||
       !apple9_emit_packed(emitter, &packed) ||
       !agx_apple9_pack_mov_imm(15, 16, &packed) ||
       !apple9_emit_packed(emitter, &packed) ||
       !agx_apple9_pack_mov_imm(14, 0, &packed) ||
       !apple9_emit_packed(emitter, &packed))
      goto pack_error;

   bool addend_is_zero = true;
   while (highest-- > 0) {
      unsigned digit = (value >> (highest * 4)) & 0xf;

      /*
       * r15 is one long-lived radix value.  r14 remains a long-lived zero
       * across consecutive zero digits; a nonzero digit semantically
       * overwrites it, so only then is zero assigned again for the next
       * multiply.  This exercises the encoded last-use model instead of
       * reloading consumed operands before every digit.
       */
      if (!addend_is_zero) {
         if (!agx_apple9_pack_mov_imm(14, 0, &packed) ||
             !apple9_emit_packed(emitter, &packed))
            goto pack_error;
         addend_is_zero = true;
      }

      struct agx_apple9_vir_instr multiply = {
         .op = AGX_APPLE9_VIR_IMAD,
         .encoding = AGX_APPLE9_ENC_INT_MAD_EXTENDED,
         .dest = 0,
         .src = {0, 1, 2},
         .nr_srcs = 3,
         .live_after_mask = (highest > 0 ? (1u << 1) : 0) |
                            ((digit == 0 && highest > 0) ? (1u << 2) : 0),
      };
      const uint8_t multiply_phys[] = {accumulator, 15, 14};
      if (!agx_apple9_pack_vir_instruction(&multiply, multiply_phys, &packed,
                                           reason) ||
          !apple9_emit_packed(emitter, &packed))
         goto pack_error;

      if (digit != 0) {
         if (!agx_apple9_pack_mov_imm(14, digit, &packed) ||
             !apple9_emit_packed(emitter, &packed))
            goto pack_error;
         struct agx_apple9_vir_instr add = {
            .op = AGX_APPLE9_VIR_IADD,
            .encoding = AGX_APPLE9_ENC_INT_ADD_EXTENDED,
            .dest = 0,
            .src = {0, 1},
            .nr_srcs = 2,
         };
         const uint8_t add_phys[] = {accumulator, 14};
         if (!agx_apple9_pack_vir_instruction(&add, add_phys, &packed,
                                              reason) ||
             !apple9_emit_packed(emitter, &packed))
            goto pack_error;
         addend_is_zero = false;
      }
   }

   if (!agx_apple9_pack_mov(dst, accumulator, &packed) ||
       !apple9_emit_packed(emitter, &packed))
      goto pack_error;
   return true;

pack_error:
   if (reason != NULL && *reason == NULL)
      *reason = "Apple9 DAG constant pack failed";
   return false;
}

static bool
apple9_emit_collect_vir(struct apple9_emitter *emitter,
                        const struct agx_apple9_vir_instr *instruction,
                        const uint8_t *phys, unsigned *emission_max_gpr,
                        const char **reason)
{
   const unsigned components = instruction->dest_components;
   if (instruction->op != AGX_APPLE9_VIR_COLLECT || components < 2 ||
       components > 4 || instruction->nr_srcs != components)
      goto invalid;

   for (unsigned c = 0; c < components; ++c) {
      const unsigned dst = phys[instruction->dest + c];
      const unsigned src = phys[instruction->src[c]];
      *emission_max_gpr = MAX2(*emission_max_gpr, MAX2(dst, src));
      if (dst == src)
         continue;

      /* Extended IOR(x, x) is the validated general-register bit copy used by
       * scoreboard materialization. COLLECT is a parallel-copy pseudo, but
       * the allocator permits only complete coalescing or a disjoint tuple,
       * so these lane copies cannot clobber a later source. */
      struct agx_apple9_vir_instr copy = {
         .op = AGX_APPLE9_VIR_IOR,
         .encoding = AGX_APPLE9_ENC_LOGIC_EXTENDED,
         .dest = 0,
         .dest_components = 1,
         .src = {1, 1},
         .nr_srcs = 2,
         .live_after_mask =
            (instruction->live_after_mask & (1u << c)) ? 0x3 : 0,
      };
      const uint8_t copy_phys[] = {dst, src};
      struct agx_apple9_packed_instruction packed;
      if (!agx_apple9_pack_vir_instruction(&copy, copy_phys, &packed, reason) ||
          !apple9_emit_packed(emitter, &packed))
         goto invalid;
   }

   return true;

invalid:
   if (reason != NULL && *reason == NULL)
      *reason = "Apple9 COLLECT lowering failed";
   return false;
}

static bool
apple9_emit_device_store_vir(
   struct apple9_emitter *emitter,
   const struct agx_apple9_vir_instr *instruction, const uint8_t *phys,
   const char **reason)
{
   const unsigned components = instruction->memory_components;
   if (instruction->op != AGX_APPLE9_VIR_DEVICE_STORE || components < 1 ||
       components > 4 || instruction->nr_srcs != components + 1)
      goto invalid;

   struct agx_apple9_packed_instruction packed;
   unsigned data[4];
   for (unsigned c = 0; c < components; ++c)
      data[c] = phys[instruction->src[c]];
   const unsigned index = phys[instruction->src[components]];

   bool adjacent = true;
   for (unsigned c = 1; c < components; ++c)
      adjacent &= data[c] == data[0] + c;

   if (!adjacent)
      goto invalid;

   bool packed_ok =
      components == 1
         ? agx_apple9_pack_device_store_scalar(
              data[0], index, instruction->immediate, instruction->memory_bits,
              instruction->device_store_form, &packed)
         : agx_apple9_pack_device_store_vector_u32(
              data[0], index, instruction->immediate, components,
              instruction->device_store_form, &packed);
   if (!packed_ok || !apple9_emit_packed(emitter, &packed))
      goto invalid;
   return true;

invalid:
   if (reason != NULL && *reason == NULL)
      *reason = "Apple9 VIR device-store legalization failed";
   return false;
}

static void
apple9_infer_device_load_index_contracts(struct agx_apple9_vir_program *program)
{
   for (unsigned i = 0; i < program->instruction_count; ++i) {
      struct agx_apple9_vir_instr *load = &program->instructions[i];
      if (load->op != AGX_APPLE9_VIR_DEVICE_LOAD || load->nr_srcs != 1)
         continue;

      uint32_t index = load->src[0];
      bool retained = false;
      for (unsigned live = 0; live < program->live_out_count; ++live)
         retained |= program->live_out[live] == index;
      for (unsigned j = i + 1; j < program->instruction_count; ++j) {
         for (unsigned source = 0; source < program->instructions[j].nr_srcs;
              ++source)
            retained |= program->instructions[j].src[source] == index;
      }
      load->device_load_index_kind =
         retained ? AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR
                  : AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR;

      const struct agx_apple9_vir_instr *producer = NULL;
      bool earlier_load_consumer = false;
      for (unsigned j = 0; j < i; ++j) {
         const struct agx_apple9_vir_instr *candidate =
            &program->instructions[j];
         const unsigned components =
            candidate->dest_components ? candidate->dest_components : 1;
         if (index >= candidate->dest && index - candidate->dest < components)
            producer = candidate;
         if (candidate->op == AGX_APPLE9_VIR_DEVICE_LOAD &&
             candidate->nr_srcs == 1 && candidate->src[0] == index)
            earlier_load_consumer = true;
      }
      load->device_load_index_first_consumer =
         producer != NULL && producer->op == AGX_APPLE9_VIR_DEVICE_LOAD &&
         !earlier_load_consumer;
   }
}

static bool
apple9_compile_dag(nir_shader *nir, struct agx_shader_part *out,
                   struct agx_apple9_compute_profile *profile,
                   const char **reason)
{
   nir_intrinsic_instr *store = NULL;
   struct util_dynarray loads = UTIL_DYNARRAY_INIT;
   nir_scalar store_index;
   struct apple9_affine_index affine = {0};
   unsigned index_rank = 0;
   unsigned store_index_scale = 1;
   unsigned store_index_add = 0;
   struct apple9_buffer_map resource_map = {0};
   uint8_t store_binding = 0;

   if (!apple9_collect_buffer_map(nir, &resource_map, reason))
      return false;
   const unsigned input_count = resource_map.input_count;

   if (apple9_shader_has_buffer_load(nir)) {
      if (!apple9_find_buffer_dag(nir, &resource_map, &loads, &store,
                                  &store_index, &affine, &index_rank,
                                  &store_index_scale, &store_index_add,
                                  reason)) {
         util_dynarray_fini(&loads);
         return false;
      }
   } else if (!apple9_find_dag_store(nir, &store, &store_index, &store_binding,
                                     &store_index_scale, &store_index_add,
                                     reason)) {
      util_dynarray_fini(&loads);
      return false;
   }

   if (input_count == 0 &&
       !apple9_affine_index(store_index, &affine, &index_rank)) {
      *reason = "Apple9 DAG compiler requires a dense affine store index";
      util_dynarray_fini(&loads);
      return false;
   }

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(impl);
   struct apple9_dag_lower lower = {
      .zero_vreg = AGX_APPLE9_VREG_INVALID,
      .loads = loads.data,
      .load_count =
         util_dynarray_num_elements(&loads, struct apple9_scalar_load),
   };
   for (unsigned i = 0; i < ARRAY_SIZE(lower.system_vreg); ++i)
      lower.system_vreg[i] = AGX_APPLE9_VREG_INVALID;
   for (unsigned i = 0; i < lower.load_count; ++i) {
      bool first = true;
      for (unsigned earlier = 0; earlier < i; ++earlier)
         first &= lower.loads[earlier].intr != lower.loads[i].intr;
      lower.load_instruction_count += first;
   }
   agx_apple9_vir_init(&lower.program);
   lower.ssa_map_count = impl->ssa_alloc * 4;
   lower.ssa_to_vreg = malloc(lower.ssa_map_count * sizeof(uint32_t));
   if (lower.ssa_to_vreg == NULL) {
      *reason = "out of memory indexing Apple9 NIR";
      goto fail;
   }
   for (unsigned i = 0; i < lower.ssa_map_count; ++i)
      lower.ssa_to_vreg[i] = AGX_APPLE9_VREG_INVALID;

   uint32_t index = AGX_APPLE9_VREG_INVALID;
   const unsigned output_components = store->src[0].ssa->num_components;
   const unsigned output_bits = store->src[0].ssa->bit_size;

   /* Preserve NIR's topological load schedule instead of discovering loads
    * recursively from the final store expression.  This makes independent
    * outstanding results genuinely simultaneous, exposes their real
    * scoreboard/register pressure, and still recursively lowers any address
    * calculation that a dependent load needs. */
   for (unsigned i = 0; i < lower.load_count; ++i) {
      const struct apple9_scalar_load *load = &lower.loads[i];
      const unsigned load_key = load->intr->def.index * 4 + load->component;
      if (load_key < lower.ssa_map_count &&
          lower.ssa_to_vreg[load_key] != AGX_APPLE9_VREG_INVALID)
         continue;
      if (apple9_lower_dag_scalar(
             &lower, nir_get_scalar(&load->intr->def, load->component)) ==
          AGX_APPLE9_VREG_INVALID) {
         *reason = lower.reason;
         goto fail;
      }
   }

   uint32_t output[4] = {AGX_APPLE9_VREG_INVALID, AGX_APPLE9_VREG_INVALID,
                         AGX_APPLE9_VREG_INVALID, AGX_APPLE9_VREG_INVALID};
   for (unsigned c = 0; c < output_components; ++c) {
      output[c] = apple9_lower_dag_scalar(
         &lower, apple9_chase_trivial(nir_get_scalar(store->src[0].ssa, c)));
      if (output[c] == AGX_APPLE9_VREG_INVALID) {
         *reason = lower.reason;
         goto fail;
      }
   }

   if (lower.emitted_load_count != lower.load_instruction_count) {
      *reason = "Apple9 DAG contains an input load outside the output graph";
      goto fail;
   }

   index = apple9_lower_dag_scalar(&lower, store_index);
   if (index == AGX_APPLE9_VREG_INVALID) {
      *reason = lower.reason;
      goto fail;
   }
   /* Native vector stores scale their tuple index in the memory format.
    * Scalar stores instead consume a u32 element index, so only they need
    * an explicit affine address calculation here. */
   if (output_components == 1 && store_index_scale > 1) {
      uint32_t scale = apple9_dag_imm(&lower, store_index_scale);
      uint32_t zero = apple9_dag_zero(&lower);
      uint32_t sources[3] = {index, scale, zero};
      if (scale == AGX_APPLE9_VREG_INVALID ||
          zero == AGX_APPLE9_VREG_INVALID) {
         *reason = lower.reason;
         goto fail;
      }
      index = apple9_dag_emit(&lower, AGX_APPLE9_VIR_IMAD,
                              AGX_APPLE9_ENC_INT_MAD_EXTENDED, sources,
                              ARRAY_SIZE(sources), 0);
   }
   if (output_components == 1 && store_index_add != 0 &&
       index != AGX_APPLE9_VREG_INVALID) {
      uint32_t add = apple9_dag_imm(&lower, store_index_add);
      uint32_t sources[2] = {index, add};
      index = add == AGX_APPLE9_VREG_INVALID
                 ? AGX_APPLE9_VREG_INVALID
                 : apple9_dag_emit(&lower, AGX_APPLE9_VIR_IADD,
                                   AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources,
                                   ARRAY_SIZE(sources), 0);
   }
   if (index == AGX_APPLE9_VREG_INVALID) {
      *reason = lower.reason;
      goto fail;
   }

   const unsigned output_binding = input_count ? input_count : store_binding;
   if (!agx_apple9_vir_emit_device_store(
          &lower.program, output_binding, index, output, output_components,
          output_bits)) {
      *reason = "could not emit the Apple9 VIR device store";
      goto fail;
   }
   apple9_infer_device_load_index_contracts(&lower.program);

   if (!agx_apple9_assign_vir_scoreboard_slots(&lower.program, reason))
      goto fail;

   /* The byte/subword source selector is not generalized yet.  Scoreboard
    * materialization may have replaced the original load SSA, so constrain
    * the store's final source rather than the pre-materialization value. */
   if (output_bits != 32) {
      struct agx_apple9_vir_instr *final_store =
         &lower.program.instructions[lower.program.instruction_count - 1];
      if (final_store->op != AGX_APPLE9_VIR_DEVICE_STORE ||
          !agx_apple9_vir_set_fixed_phys(&lower.program, final_store->src[0],
                                         0)) {
         *reason = "could not reserve the proven narrow-store source";
         goto fail;
      }
   }

   if (!agx_apple9_allocate_vir(&lower.program, reason))
      goto fail;

   if (getenv("AGX_APPLE9_TRACE") != NULL) {
      for (unsigned i = 0; i < lower.program.instruction_count; ++i) {
         const struct agx_apple9_vir_instr *instruction =
            &lower.program.instructions[i];
         fprintf(stderr, "APPLE9_ALLOC i=%u op=%u enc=%u dst=", i,
                 instruction->op, instruction->encoding);
         if (instruction->dest == AGX_APPLE9_VREG_INVALID)
            fputs("-", stderr);
         else
            fprintf(stderr, "r%u", lower.program.phys[instruction->dest]);
         fputs(" src=", stderr);
         for (unsigned s = 0; s < instruction->nr_srcs; ++s)
            fprintf(stderr, "%sr%u", s ? "," : "",
                    lower.program.phys[instruction->src[s]]);
         fprintf(stderr, " imm=%#x live=%#x slot=%u\n", instruction->immediate,
                 instruction->live_after_mask, instruction->scoreboard_slot);
      }
   }

   struct apple9_emitter emitter = {.bytes = UTIL_DYNARRAY_INIT};
   bool used_large_constant = false;
   unsigned emission_max_gpr = lower.program.max_phys_gpr;
   struct agx_apple9_packed_instruction packed;

   for (unsigned i = 0; i < lower.program.instruction_count; ++i) {
      const struct agx_apple9_vir_instr *instruction =
         &lower.program.instructions[i];
      if (instruction->op == AGX_APPLE9_VIR_COLLECT) {
         if (!apple9_emit_collect_vir(&emitter, instruction,
                                      lower.program.phys, &emission_max_gpr,
                                      reason)) {
            util_dynarray_fini(&emitter.bytes);
            goto fail;
         }
         continue;
      }
      if (instruction->op == AGX_APPLE9_VIR_DEVICE_STORE) {
         if (!apple9_emit_device_store_vir(
                &emitter, instruction, lower.program.phys, reason)) {
            util_dynarray_fini(&emitter.bytes);
            goto fail;
         }
         continue;
      }
      if (instruction->op == AGX_APPLE9_VIR_IMM &&
          instruction->immediate > 0x7f) {
         used_large_constant = true;
         if (!apple9_emit_dag_constant(&emitter,
                                       lower.program.phys[instruction->dest], 0,
                                       instruction->immediate, reason)) {
            util_dynarray_fini(&emitter.bytes);
            goto fail;
         }
         continue;
      }
      if (!agx_apple9_pack_vir_instruction(instruction, lower.program.phys,
                                           &packed, reason) ||
          !apple9_emit_packed(&emitter, &packed)) {
         if (getenv("AGX_APPLE9_TRACE") != NULL) {
            fprintf(stderr,
                    "APPLE9_PACK_FAIL i=%u op=%u enc=%u dst=r%u src=",
                    i, instruction->op, instruction->encoding,
                    lower.program.phys[instruction->dest]);
            for (unsigned s = 0; s < instruction->nr_srcs; ++s)
               fprintf(stderr, "%sr%u", s ? "," : "",
                       lower.program.phys[instruction->src[s]]);
            fputc('\n', stderr);
         }
         if (*reason == NULL)
            *reason = "Apple9 DAG instruction pack failed";
         util_dynarray_fini(&emitter.bytes);
         goto fail;
      }
      if (getenv("AGX_APPLE9_TRACE") != NULL) {
         fprintf(stderr,
                 "APPLE9_VIR i=%u op=%u enc=%u dst=r%u src=",
                 i, instruction->op, instruction->encoding,
                 lower.program.phys[instruction->dest]);
         for (unsigned s = 0; s < instruction->nr_srcs; ++s)
            fprintf(stderr, "%sr%u", s ? "," : "",
                    lower.program.phys[instruction->src[s]]);
         fprintf(stderr, " live=%#x slot=%u\n", instruction->live_after_mask,
                 instruction->scoreboard_slot);
      }
   }
   apple9_emit_stop(&emitter);

   out->binary = emitter.bytes.data;
   out->info.stage = MESA_SHADER_COMPUTE;
   out->info.main_size = emitter.bytes.size;
   out->info.binary_size = emitter.bytes.size;
   /* nr_gprs is the architectural GPR high-water mark for Apple9.  Keep the
    * established minimum tier, but do not collapse every program using r16+
    * back to the old 16-register capture tier.  Large-literal materialization
    * uses fixed r14/r15 in addition to the allocator-authored graph. */
   unsigned max_gpr = emission_max_gpr;
   if (used_large_constant)
      max_gpr = MAX2(max_gpr, 15);
   out->info.nr_gprs = MAX2(max_gpr + 1, 8);
   out->info.stats.instrs = emitter.instructions;
   for (unsigned d = 0; d < 3; ++d)
      out->info.workgroup_size[d] = nir->info.workgroup_size[d];

   if (profile != NULL) {
      if (input_count == 0)
         *profile = AGX_APPLE9_TINY_COMPUTE_PROFILE;
      else if (input_count == 1)
         *profile = AGX_APPLE9_SSBO2_COMPUTE_PROFILE;
      else if (input_count == 2)
         *profile = AGX_APPLE9_SSBO3_STATE_U6_COMPUTE_PROFILE;
      else
         *profile = AGX_APPLE9_SSBO4_COMPUTE_PROFILE;
      profile->resource_binding_count = input_count + 1;
      for (unsigned i = 0; i < profile->resource_binding_count; ++i) {
         profile->resource_binding[i] =
            input_count == 0 ? store_binding
                             : resource_map.resource[i].binding;
         profile->resource_kind[i] =
            input_count == 0 ? AGX_APPLE9_COMPUTE_RESOURCE_SSBO
                             : resource_map.resource[i].kind;
      }
      for (unsigned d = 0; d < 3; ++d) {
         profile->local_size[d] = nir->info.workgroup_size[d];
         profile->index_stride[d] = affine.stride[d];
      }
      profile->index_rank = index_rank;
      profile->resource_access_element_size[input_count] = output_bits / 8;
      if (output_components == 1 &&
          (store_index_scale != 1 || store_index_add != 0)) {
         profile->resource_access_scale[input_count] = store_index_scale;
         profile->resource_access_add[input_count] = store_index_add;
      }

      bool bounded_argument[4] = {false};
      bool affine_argument[4] = {false};
      util_dynarray_foreach(&loads, struct apple9_scalar_load, load) {
         const uint8_t element_size = load->bit_size / 8;
         uint8_t *profile_size =
            &profile->resource_access_element_size[load->argument];
         if (*profile_size != 0 && *profile_size != element_size) {
            *reason = "Apple9 one resource uses incompatible scalar element sizes";
            util_dynarray_fini(&emitter.bytes);
            goto fail;
         }
         *profile_size = element_size;
         if (load->bounded_index) {
            bounded_argument[load->argument] = true;
            profile->resource_access_add[load->argument] =
               MAX2(profile->resource_access_add[load->argument],
                    load->bounded_max);
            continue;
         }

         affine_argument[load->argument] = true;
         uint32_t scale = load->bounded_index
                             ? 1
                             : (memcmp(load->affine.stride, affine.stride,
                                       sizeof(load->affine.stride)) == 0
                                   ? 1
                                   : (uint32_t)load->affine.stride[0]);
         uint32_t add = load->bounded_index ? load->bounded_max
                                            : (uint32_t)load->affine.base;
         if (scale == 1 && add == 0)
            continue;
         profile->resource_access_scale[load->argument] =
            MAX2(profile->resource_access_scale[load->argument], scale);
         profile->resource_access_add[load->argument] =
            MAX2(profile->resource_access_add[load->argument], add);
      }
      for (unsigned i = 0; i < input_count; ++i) {
         if (bounded_argument[i] && !affine_argument[i])
            profile->resource_access_mode[i] =
               AGX_APPLE9_COMPUTE_ACCESS_BOUNDED_INDEX;
      }
   }

   free(lower.ssa_to_vreg);
   agx_apple9_vir_finish(&lower.program);
   util_dynarray_fini(&loads);
   return true;

fail:
   free(lower.ssa_to_vreg);
   agx_apple9_vir_finish(&lower.program);
   util_dynarray_fini(&loads);
   return false;
}


bool
agx_compile_apple9_tiny(nir_shader *nir, struct agx_shader_part *out,
                        struct agx_apple9_compute_profile *profile,
                        const char **reason_out)
{
   if (getenv("AGX_APPLE9_TRACE_NIR") != NULL)
      nir_print_shader(nir, stderr);

   const char *reason = NULL;
   memset(out, 0, sizeof(*out));
   if (profile != NULL)
      memset(profile, 0, sizeof(*profile));
   if (reason_out != NULL)
      *reason_out = NULL;

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      reason = "Apple9 bounded compiler supports only compute shaders";
      goto unsupported;
   }

   uint64_t local_threads = 1;
   for (unsigned d = 0; d < 3; ++d) {
      if (!nir->info.workgroup_size[d] ||
          nir->info.workgroup_size[d] > 1024 / local_threads) {
         reason = "Apple9 compute requires a legal fixed local size";
         goto unsupported;
      }
      local_threads *= nir->info.workgroup_size[d];
   }

   if (apple9_compile_dag(nir, out, profile, &reason))
      return true;

unsupported:
   if (reason_out != NULL)
      *reason_out = reason;
   return false;
}

static bool
apple9_graphics_unsupported(struct agx_shader_part *out,
                            const char **reason_out)
{
   memset(out, 0, sizeof(*out));
   if (reason_out != NULL)
      *reason_out =
         "Apple9 graphics compilation is not implemented";
   return false;
}

bool
agx_compile_apple9_fragment(nir_shader *nir, struct agx_shader_part *out,
                            const char **reason_out)
{
   (void)nir;
   return apple9_graphics_unsupported(out, reason_out);
}

bool
agx_compile_apple9_vertex(nir_shader *nir, struct agx_shader_part *out,
                          const char **reason_out)
{
   (void)nir;
   return apple9_graphics_unsupported(out, reason_out);
}

bool
agx_compile_apple9_vertex_prolog(nir_shader *nir, struct agx_shader_part *out,
                                 const char **reason_out)
{
   (void)nir;
   return apple9_graphics_unsupported(out, reason_out);
}
