/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_compile_apple9.h"

#include "compiler/nir/nir_builder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum tool_program {
   TOOL_PROGRAM_CONSTANT,
   TOOL_PROGRAM_GLOBAL_ID,
   TOOL_PROGRAM_GLOBAL_ID_MUL_ADD,
   TOOL_PROGRAM_DAG,
   TOOL_PROGRAM_SELECT_DAG,
   TOOL_PROGRAM_CARRIER,
};

static nir_shader *
store_shader(enum tool_program program, unsigned first, unsigned second)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "apple9_tiny_tool");

   b.shader->info.workgroup_size[0] = 256;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = program == TOOL_PROGRAM_CARRIER ? first : 1;

   nir_def *global_id = nir_load_global_invocation_id(&b, 32);
   nir_def *gid = nir_channel(&b, global_id, 0);
   nir_def *value;
   switch (program) {
   case TOOL_PROGRAM_CONSTANT:
      value = nir_imm_int(&b, first);
      break;
   case TOOL_PROGRAM_GLOBAL_ID:
      value = gid;
      break;
   case TOOL_PROGRAM_GLOBAL_ID_MUL_ADD:
      value = nir_iadd(&b, nir_imul_imm(&b, gid, first),
                       nir_imm_int(&b, second));
      break;
   case TOOL_PROGRAM_DAG:
      value = nir_iadd_imm(
         &b, nir_ixor(&b, nir_imul_imm(&b, gid, 3),
                      nir_iadd_imm(&b, gid, 7)),
         11);
      break;
   case TOOL_PROGRAM_SELECT_DAG:
      value = nir_bcsel(
         &b,
         nir_ult(&b, nir_iadd_imm(&b, gid, 3), nir_imul_imm(&b, gid, 2)),
         nir_ixor(&b, gid, nir_imm_int(&b, 0x55)),
         nir_iadd_imm(&b, gid, 100));
      break;
   case TOOL_PROGRAM_CARRIER: {
      nir_def *offset = nir_imul_imm(&b, gid, 4);
      value = nir_imm_int(&b, 0x6d2b79f5);
      for (unsigned binding = 0; binding + 1 < first; ++binding) {
         nir_def *loaded = nir_load_ssbo(
            &b, 1, 32, nir_imm_int(&b, binding), offset, .align_mul = 4);
         value = nir_iadd(&b, nir_imul_imm(&b, value, 33), loaded);
      }
      nir_store_ssbo(&b, value, nir_imm_int(&b, first - 1), offset,
                     .align_mul = 4);
      return b.shader;
   }
   }

   nir_store_ssbo(&b, value, nir_imm_int(&b, 0),
                  nir_imul_imm(&b, gid, 4));
   return b.shader;
}

static nir_shader *
render_shader(bool fragment, bool constant, bool perspective, bool small,
              bool alternate)
{
   nir_builder b = nir_builder_init_simple_shader(
      fragment ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX, &agx_nir_options,
      "apple9_render_tool");
   nir_def *color;
   if (fragment) {
      nir_def *bary =
         nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_SMOOTH);
      color = nir_load_interpolated_input(
         &b, 3, 32, bary, nir_imm_int(&b, 0),
         .io_semantics = {.location = VARYING_SLOT_VAR0, .num_slots = 1},
         .dest_type = nir_type_float32);
      if (constant)
         color = nir_vec3(&b, nir_imm_float(&b, 0.75), nir_imm_float(&b, 0.5),
                          nir_imm_float(&b, 0.25));
      nir_def *r = nir_channel(&b, color, 0);
      nir_def *g = nir_channel(&b, color, 1);
      nir_def *blue = nir_channel(&b, color, 2);
      color = alternate
                 ? nir_vec4(&b, nir_fsub(&b, nir_imm_float(&b, 1), r),
                            nir_fmul(&b, g, g),
                            nir_fadd_imm(&b, nir_fmul_imm(&b, blue, 0.5), 0.25),
                            nir_imm_float(&b, 1))
                 : nir_vec4(&b, nir_fmul(&b, r, r), nir_fadd_imm(&b, g, 0.125),
                            nir_fmul_imm(&b, blue, 0.5), nir_imm_float(&b, 1));
      nir_store_output(
         &b, color, nir_imm_int(&b, 0), .write_mask = 15,
         .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1},
         .src_type = nir_type_float32);
   } else {
      nir_def *id = nir_load_vertex_id_zero_base(&b);
      nir_def *red = nir_ieq_imm(&b, id, 0);
      nir_def *green = nir_ieq_imm(&b, id, 1);
      nir_def *blue = nir_ieq_imm(&b, id, 2);
      nir_def *x = nir_bcsel(
         &b, red, nir_imm_float(&b, -0.75),
         nir_bcsel(&b, green, nir_imm_float(&b, 0.75), nir_imm_float(&b, 0)));
      nir_def *y =
         nir_bcsel(&b, blue, nir_imm_float(&b, 0.75), nir_imm_float(&b, -0.75));
      if (small) {
         x = nir_fadd_imm(&b, nir_fmul_imm(&b, x, 0.5), 0.125);
         y = nir_fmul_imm(&b, y, 0.5);
      }
      nir_def *w = perspective
                      ? nir_bcsel(&b, red, nir_imm_float(&b, 1),
                                  nir_bcsel(&b, green, nir_imm_float(&b, 2),
                                            nir_imm_float(&b, 4)))
                      : nir_imm_float(&b, 1);
      if (perspective) {
         x = nir_fmul(&b, x, w);
         y = nir_fmul(&b, y, w);
      }
      nir_store_output(
         &b, nir_vec4(&b, x, y, nir_imm_float(&b, 0), w), nir_imm_int(&b, 0),
         .write_mask = 15,
         .io_semantics = {.location = VARYING_SLOT_POS, .num_slots = 1},
         .src_type = nir_type_float32);
      color = nir_vec3(
         &b, nir_bcsel(&b, red, nir_imm_float(&b, 1), nir_imm_float(&b, 0)),
         nir_bcsel(&b, green, nir_imm_float(&b, 1), nir_imm_float(&b, 0)),
         nir_bcsel(&b, blue, nir_imm_float(&b, 1), nir_imm_float(&b, 0)));
      nir_store_output(
         &b, color, nir_imm_int(&b, 0), .write_mask = 7,
         .io_semantics = {.location = VARYING_SLOT_VAR0, .num_slots = 1},
         .src_type = nir_type_float32);
   }
   b.shader->info.io_lowered = true;
   return b.shader;
}

static bool
parse_u8(const char *text, unsigned *value)
{
   char *end = NULL;
   errno = 0;
   unsigned long parsed = strtoul(text, &end, 0);
   if (errno || *text == '\0' || *end != '\0' || parsed > 0xff)
      return false;

   *value = parsed;
   return true;
}

int
main(int argc, char **argv)
{
   if (argc < 2 || argc > 5) {
usage:
      fprintf(stderr, "usage: %s OUTPUT [VALUE|gid]\n", argv[0]);
      fprintf(stderr, "       %s OUTPUT mad MULTIPLIER ADDEND\n", argv[0]);
      fprintf(stderr, "       %s OUTPUT vertex|vertex-w|vertex-small\n",
              argv[0]);
      fprintf(stderr,
              "       %s OUTPUT fragment|fragment-alt|fragment-constant\n",
              argv[0]);
      fprintf(stderr, "       %s OUTPUT dag\n", argv[0]);
      fprintf(stderr, "       %s OUTPUT select-dag\n", argv[0]);
      fprintf(stderr, "       %s OUTPUT carrier RESOURCE_COUNT\n", argv[0]);
      return EXIT_FAILURE;
   }

   enum tool_program program = TOOL_PROGRAM_CONSTANT;
   unsigned first = 42;
   unsigned second = 0;
   bool perspective = argc == 3 && !strcmp(argv[2], "vertex-w");
   bool small = argc == 3 && !strcmp(argv[2], "vertex-small");
   bool alternate = argc == 3 && !strcmp(argv[2], "fragment-alt");
   bool vertex =
      perspective || small || (argc == 3 && !strcmp(argv[2], "vertex"));
   bool constant = argc == 3 && !strcmp(argv[2], "fragment-constant");
   bool fragment =
      alternate || constant || (argc == 3 && !strcmp(argv[2], "fragment"));
   if (vertex || fragment) {
   } else if (argc == 3 && strcmp(argv[2], "gid") == 0) {
      program = TOOL_PROGRAM_GLOBAL_ID;
   } else if (argc == 3 && strcmp(argv[2], "dag") == 0) {
      program = TOOL_PROGRAM_DAG;
   } else if (argc == 3 && strcmp(argv[2], "select-dag") == 0) {
      program = TOOL_PROGRAM_SELECT_DAG;
   } else if (argc == 4 && strcmp(argv[2], "carrier") == 0) {
      char *end = NULL;
      unsigned long count = strtoul(argv[3], &end, 0);
      if (*argv[3] == '\0' || *end != '\0' || count < 1 || count > 8)
         goto usage;
      program = TOOL_PROGRAM_CARRIER;
      first = count;
   } else if (argc == 3) {
      if (!parse_u8(argv[2], &first))
         goto usage;
   } else if (argc == 5 && strcmp(argv[2], "mad") == 0) {
      program = TOOL_PROGRAM_GLOBAL_ID_MUL_ADD;
      if (!parse_u8(argv[3], &first) || !parse_u8(argv[4], &second))
         goto usage;
   } else if (argc != 2) {
      goto usage;
   }

   nir_shader *nir =
      vertex || fragment
         ? render_shader(fragment, constant, perspective, small, alternate)
         : store_shader(program, first, second);
   struct agx_shader_part compiled = {0};
   struct agx_apple9_compute_profile profile = {0};
   const char *reason = NULL;
   bool compiled_ok =
      vertex     ? agx_compile_apple9_vertex(nir, &compiled, &reason)
      : fragment ? agx_compile_apple9_fragment(nir, &compiled, &reason)
                 : agx_compile_apple9_tiny(nir, &compiled, &profile, &reason);
   if (!compiled_ok) {
      fprintf(stderr, "Apple9 compilation failed: %s\n", reason);
      nir_print_shader(nir, stderr);
      ralloc_free(nir);
      return EXIT_FAILURE;
   }

   FILE *output = fopen(argv[1], "wb");
   if (output == NULL) {
      perror(argv[1]);
      free(compiled.binary);
      ralloc_free(nir);
      return EXIT_FAILURE;
   }
   bool ok = fwrite(compiled.binary, compiled.info.binary_size, 1, output) == 1;
   if (fclose(output) != 0)
      ok = false;

   if (!ok) {
      perror(argv[1]);
      free(compiled.binary);
      ralloc_free(nir);
      return EXIT_FAILURE;
   }

   fprintf(stderr, "Apple9 compiler wrote %u bytes to %s (ABI %u; ",
           compiled.info.binary_size, argv[1], profile.abi);
   for (unsigned i = 0; i < profile.resource_binding_count; ++i)
      fprintf(stderr, "%sarg%u=b%u", i ? "," : "", i,
              profile.resource_binding[i]);
   fprintf(stderr, ")\n");
   free(compiled.binary);
   ralloc_free(nir);
   return EXIT_SUCCESS;
}
