/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_apple9_ir.h"
#include "agx_apple9_machine.h"
#include "agx_compile_apple9.h"

#include "compiler/nir/nir_builder.h"
#include <gtest/gtest.h>

TEST(Apple9Machine, PhysicalModel)
{
   EXPECT_EQ(agx_apple9_machine.gpr_count, 96u);
   EXPECT_EQ(agx_apple9_machine.half_register_count, 192u);
   EXPECT_TRUE(agx_apple9_machine.hardware_register_interlocks);
   EXPECT_FALSE(agx_apple9_machine.software_waits);
   EXPECT_FALSE(agx_apple9_machine.spilling_supported);
   EXPECT_EQ(agx_apple9_machine.occupancy_model,
             AGX_APPLE9_OCCUPANCY_PRESSURE_TIER);
}

TEST(Apple9Machine, EveryEncodingIsDescribed)
{
   for (unsigned i = 0; i < AGX_APPLE9_ENC_COUNT; ++i) {
      const auto *info =
         agx_apple9_encoding_info(static_cast<agx_apple9_encoding>(i));
      ASSERT_NE(info, nullptr);
      EXPECT_NE(info->name, nullptr);
      EXPECT_GT(info->length, 0u) << info->name;
      EXPECT_GT(info->operand_count, 0u) << info->name;
      EXPECT_LE(info->operand_count, AGX_APPLE9_MAX_ENCODING_OPERANDS)
         << info->name;
   }
}

TEST(Apple9Machine, RawLiteralUsesSixBitScatteredDestination)
{
   for (unsigned gpr : {0u, 15u, 16u, 31u, 32u, 47u, 48u, 63u})
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
         AGX_APPLE9_ENC_MOV_IMM32, AGX_APPLE9_OPERAND_DEST, gpr, 32));

   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_MOV_IMM32, AGX_APPLE9_OPERAND_DEST, 64, 32));
   const auto *dst = agx_apple9_find_operand(AGX_APPLE9_ENC_MOV_IMM32,
                                             AGX_APPLE9_OPERAND_DEST);
   ASSERT_NE(dst, nullptr);
   EXPECT_TRUE(dst->flags & AGX_APPLE9_OPERAND_SCATTERED);
   EXPECT_FALSE(dst->flags & AGX_APPLE9_OPERAND_HARD_LOW);
}

TEST(Apple9Machine, CompactBinaryAluUsesScatteredFullRegisterMap)
{
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT2_COMPACT, AGX_APPLE9_OPERAND_DEST, 95, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT2_COMPACT, AGX_APPLE9_OPERAND_SRC0, 64, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT2_COMPACT, AGX_APPLE9_OPERAND_SRC1, 95, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT2_COMPACT, AGX_APPLE9_OPERAND_DEST, 96, 32));

   const auto *extended =
      agx_apple9_encoding_info(AGX_APPLE9_ENC_FLOAT2_MODIFIER_EXTENDED);
   EXPECT_FALSE(extended->allocator_safe);
   const auto *dst = agx_apple9_find_operand(
      AGX_APPLE9_ENC_FLOAT2_MODIFIER_EXTENDED, AGX_APPLE9_OPERAND_DEST);
   ASSERT_NE(dst, nullptr);
   EXPECT_EQ(dst->max_index, 95u);
   EXPECT_TRUE(dst->flags & AGX_APPLE9_OPERAND_SCATTERED);

   for (auto role : {AGX_APPLE9_OPERAND_DEST, AGX_APPLE9_OPERAND_SRC0,
                     AGX_APPLE9_OPERAND_SRC1}) {
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(AGX_APPLE9_ENC_MINMAX_COMPACT,
                                                  role, 95, 32));
      EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
         AGX_APPLE9_ENC_MINMAX_COMPACT, role, 96, 32));
   }

   const auto *fma_dst = agx_apple9_find_operand(AGX_APPLE9_ENC_FLOAT3_EXTENDED,
                                                 AGX_APPLE9_OPERAND_DEST);
   ASSERT_NE(fma_dst, nullptr);
   EXPECT_EQ(fma_dst->max_index, 95u);
   EXPECT_TRUE(fma_dst->flags & AGX_APPLE9_OPERAND_SCATTERED);
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT3_EXTENDED, AGX_APPLE9_OPERAND_DEST, 64, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT3_EXTENDED, AGX_APPLE9_OPERAND_SRC0, 95, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT3_EXTENDED, AGX_APPLE9_OPERAND_SRC1, 64, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT3_EXTENDED, AGX_APPLE9_OPERAND_SRC2, 95, 32));
}

TEST(Apple9Machine, FullFileBoundaries)
{
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_GET_SR, AGX_APPLE9_OPERAND_DEST, 15, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_GET_SR, AGX_APPLE9_OPERAND_DEST, 16, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_DEVICE_LOAD, AGX_APPLE9_OPERAND_DEST, 63, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_DEVICE_LOAD, AGX_APPLE9_OPERAND_DEST, 64, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_DEVICE_LOAD, AGX_APPLE9_OPERAND_DEST, 96, 32));
   EXPECT_EQ(
      agx_apple9_encoding_info(AGX_APPLE9_ENC_DEVICE_LOAD)->operand_count, 2u);
   EXPECT_NE(agx_apple9_find_operand(AGX_APPLE9_ENC_DEVICE_LOAD,
                                     AGX_APPLE9_OPERAND_INDEX),
             nullptr);
}

TEST(Apple9Machine, ReciprocalHasAsymmetricRegisterFiles)
{
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT_RECIPROCAL, AGX_APPLE9_OPERAND_DEST, 95, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT_RECIPROCAL, AGX_APPLE9_OPERAND_DEST, 96, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT_RECIPROCAL, AGX_APPLE9_OPERAND_SRC0, 63, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_FLOAT_RECIPROCAL, AGX_APPLE9_OPERAND_SRC0, 64, 32));
}

TEST(Apple9Packer, ReciprocalPacksHandoffLifetimeAndNativeResultHint)
{
   const uint8_t phys[] = {18, 7};
   agx_apple9_vir_instr reciprocal = {
      .op = AGX_APPLE9_VIR_FRCP,
      .encoding = AGX_APPLE9_ENC_FLOAT_RECIPROCAL,
      .dest = 0,
      .dest_components = 1,
      .src = {1},
      .immediate = 0x02,
      .nr_srcs = 1,
      .scoreboard_slot = AGX_APPLE9_SCOREBOARD_SLOT_6,
   };
   agx_apple9_packed_instruction packed = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&reciprocal, phys, &packed,
                                               &reason))
      << (reason ? reason : "no diagnostic");
   static const uint8_t pending_release[] = {
      0xaf, 0x00, 0x56, 0x24, 0x02,
      0x1c, 0x10, 0x48, 0x20, 0x00,
   };
   ASSERT_EQ(packed.length, sizeof(pending_release));
   EXPECT_EQ(memcmp(packed.bytes, pending_release, sizeof(pending_release)), 0);

   reciprocal.immediate = 0x03;
   reciprocal.live_after_mask = 1;
   reciprocal.scoreboard_slot = AGX_APPLE9_SCOREBOARD_SLOT_NONE;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&reciprocal, phys, &packed,
                                               &reason))
      << (reason ? reason : "no diagnostic");
   static const uint8_t ordinary_retain[] = {
      0xaf, 0x00, 0x54, 0x24, 0x03,
      0x1c, 0x00, 0x48, 0x20, 0x00,
   };
   ASSERT_EQ(packed.length, sizeof(ordinary_retain));
   EXPECT_EQ(memcmp(packed.bytes, ordinary_retain, sizeof(ordinary_retain)), 0);

   for (auto slot : {AGX_APPLE9_SCOREBOARD_SLOT_4,
                     AGX_APPLE9_SCOREBOARD_SLOT_5}) {
      reciprocal.scoreboard_slot = slot;
      ASSERT_TRUE(agx_apple9_pack_vir_instruction(&reciprocal, phys, &packed,
                                                  &reason))
         << reason;
      const unsigned encoded_mask =
         (packed.bytes[1] >> 4) | ((packed.bytes[2] & 0x3) << 4);
      EXPECT_EQ(encoded_mask, 1u << (slot - 1));
   }
}

TEST(Apple9Packer, RawLoadTokensPackIndependentlyOfIndexAndFraming)
{
   static const struct {
      uint16_t token;
      uint8_t byte8;
      uint8_t byte9;
   } cases[] = {
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_5101, 0x51, 0x01},
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_1100, 0x11, 0x00},
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_5100, 0x51, 0x00},
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_9100, 0x91, 0x00},
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_D100, 0xd1, 0x00},
      {AGX_APPLE9_DEVICE_LOAD_TOKEN_1101, 0x11, 0x01},
   };

   for (const auto &test : cases) {
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_device_load_u32_raw(
         2, 1, 3, AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR,
         AGX_APPLE9_DEVICE_LOAD_HAS_NEXT, false, test.token, &packed));
      static const uint8_t fixed[] = {
         0x67, 0x00, 0x54, 0x04, 0x03, 0x01, 0x20,
         0x00, 0x00, 0x00, 0x00, 0x40, 0x46, 0x00,
      };
      ASSERT_EQ(packed.length, sizeof(fixed));
      EXPECT_EQ(memcmp(packed.bytes, fixed, 8), 0);
      EXPECT_EQ(packed.bytes[8], test.byte8);
      EXPECT_EQ(packed.bytes[9], test.byte9);
      EXPECT_EQ(memcmp(packed.bytes + 10, fixed + 10, 4), 0);
   }

   agx_apple9_packed_instruction packed = {};
   EXPECT_FALSE(agx_apple9_pack_device_load_u32_raw(
      2, AGX_APPLE9_GPR_COUNT, 0, AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR, 0,
      false, AGX_APPLE9_DEVICE_LOAD_TOKEN_5101, &packed));
   EXPECT_FALSE(agx_apple9_pack_device_load_u32_raw(
      2, 1, 0, AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR, 0, false, 0x7100,
      &packed));
}

TEST(Apple9Vir, DeviceLoadDirectIndexIsAnSsaSource)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   const agx_apple9_device_load_contract contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR,
      .group_flags = AGX_APPLE9_DEVICE_LOAD_FIRST,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };
   uint32_t load =
      agx_apple9_vir_emit_device_load(&program, 0, index, &contract);
   ASSERT_NE(load, AGX_APPLE9_VREG_INVALID);
   ASSERT_EQ(program.instruction_count, 1u);
   EXPECT_EQ(program.instructions[0].encoding, AGX_APPLE9_ENC_DEVICE_LOAD);
   ASSERT_EQ(program.instructions[0].nr_srcs, 1u);
   EXPECT_EQ(program.instructions[0].src[0], index);
   EXPECT_EQ(program.instructions[0].device_load_raw_token,
             AGX_APPLE9_DEVICE_LOAD_TOKEN_5101);

   ASSERT_TRUE(agx_apple9_vir_set_fixed_phys(&program, index, 1));
   ASSERT_TRUE(agx_apple9_vir_set_fixed_phys(&program, load, 0));
   ASSERT_TRUE(agx_apple9_vir_add_live_out(&program, index));
   program.output = load;
   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&program.instructions[0],
                                               program.phys, &packed, &reason))
      << (reason ? reason : "");
   static const uint8_t expected[] = {
      0x67, 0x10, 0x44, 0x00, 0x00, 0x01, 0x20,
      0x00, 0x51, 0x01, 0x00, 0x40, 0x46, 0x00,
   };
   ASSERT_EQ(packed.length, sizeof(expected));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);

   EXPECT_EQ(agx_apple9_vir_emit_device_load(
                &program, 0, AGX_APPLE9_VREG_INVALID, &contract),
             AGX_APPLE9_VREG_INVALID);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, DeviceLoadComputedIndexIsIndependentOfDestination)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 2);
   const agx_apple9_device_load_contract contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_COMPUTED_GPR,
      .group_flags = 0,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };
   uint32_t load =
      agx_apple9_vir_emit_device_load(&program, 0, index, &contract);
   ASSERT_NE(load, AGX_APPLE9_VREG_INVALID);

   uint8_t phys[] = {2, 4};
   agx_apple9_packed_instruction packed = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&program.instructions[0], phys,
                                               &packed, &reason))
      << (reason ? reason : "");
   static const uint8_t expected[] = {
      0x67, 0x00, 0x44, 0x08, 0x00, 0x82, 0x20,
      0x00, 0x51, 0x01, 0x00, 0x40, 0x46, 0x00,
   };
   ASSERT_EQ(packed.length, sizeof(expected));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);
   EXPECT_EQ(agx_apple9_vir_emit_device_load(
                &program, 0, AGX_APPLE9_VREG_INVALID, &contract),
             AGX_APPLE9_VREG_INVALID);
   EXPECT_FALSE(agx_apple9_vir_set_device_load_index_kind(
      &program, load, static_cast<agx_apple9_device_load_index_kind>(3)));
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, DeviceLoadIndexLifetimeAssertionsFollowLiveness)
{
   for (bool asserted_last_use : {false, true}) {
      SCOPED_TRACE(asserted_last_use);
      agx_apple9_vir_program program;
      agx_apple9_vir_init(&program);
      uint32_t index = agx_apple9_vir_input(&program, 1);
      const agx_apple9_device_load_contract contract = {
         .index_kind = asserted_last_use
                          ? AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR
                          : AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
         .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
      };
      uint32_t load =
         agx_apple9_vir_emit_device_load(&program, 0, index, &contract);
      ASSERT_NE(load, AGX_APPLE9_VREG_INVALID);
      ASSERT_TRUE(agx_apple9_vir_set_fixed_phys(&program, index, 1));
      ASSERT_TRUE(agx_apple9_vir_set_fixed_phys(&program, load, 0));

      /* Make the actual lifetime the opposite of the authored assertion. */
      if (asserted_last_use) {
         ASSERT_TRUE(agx_apple9_vir_add_live_out(&program, index));
      }
      program.output = load;
      const char *reason = nullptr;
      EXPECT_FALSE(agx_apple9_allocate_vir(&program, &reason));
      ASSERT_NE(reason, nullptr);
      EXPECT_NE(strstr(reason, "lifetime"), nullptr) << reason;
      agx_apple9_vir_finish(&program);
   }
}

TEST(Apple9Packer, DeviceLoadSeparatesGroupFramingFromScoreboardSlot)
{
   static const struct {
      uint8_t flags;
      enum agx_apple9_scoreboard_slot slot;
      uint8_t bytes[14];
   } cases[] = {
      {0,
       AGX_APPLE9_SCOREBOARD_SLOT_1,
       {0x67, 0x00, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x11, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {0,
       AGX_APPLE9_SCOREBOARD_SLOT_2,
       {0x67, 0x00, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {0,
       AGX_APPLE9_SCOREBOARD_SLOT_6,
       {0x67, 0x00, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x01, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST,
       AGX_APPLE9_SCOREBOARD_SLOT_1,
       {0x67, 0x10, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x11, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST,
       AGX_APPLE9_SCOREBOARD_SLOT_2,
       {0x67, 0x10, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST,
       AGX_APPLE9_SCOREBOARD_SLOT_6,
       {0x67, 0x10, 0x44, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x01, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_1,
       {0x67, 0x00, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x11, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_2,
       {0x67, 0x00, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_6,
       {0x67, 0x00, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x01, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST | AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_1,
       {0x67, 0x10, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x11, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST | AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_2,
       {0x67, 0x10, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x00, 0x00, 0x40,
        0x46, 0x00}},
      {AGX_APPLE9_DEVICE_LOAD_FIRST | AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       AGX_APPLE9_SCOREBOARD_SLOT_6,
       {0x67, 0x10, 0x54, 0x04, 0x03, 0x01, 0x20, 0x00, 0x51, 0x01, 0x00, 0x40,
        0x46, 0x00}},
   };

   agx_apple9_packed_instruction packed = {};
   for (const auto &test : cases) {
      ASSERT_TRUE(agx_apple9_pack_device_load_u32(2, 1, 3, test.flags,
                                                  test.slot, &packed));
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0)
         << "flags=" << unsigned(test.flags) << " slot=" << test.slot;
   }

   EXPECT_FALSE(agx_apple9_pack_device_load_u32(
      2, 1, 3, 0x80, AGX_APPLE9_SCOREBOARD_SLOT_6, &packed));
   EXPECT_FALSE(agx_apple9_pack_device_load_u32(
      2, 1, 3, 0, AGX_APPLE9_SCOREBOARD_SLOT_NONE, &packed));
   EXPECT_FALSE(agx_apple9_pack_device_load_u32(
      2, 1, 3, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO, &packed));
}

TEST(Apple9Packer, NativeVectorMemoryWidthsMatchValidatedEncodings)
{
   struct vector_case {
      unsigned components;
      unsigned dst;
      unsigned index;
      uint8_t flags;
      uint8_t load[14];
      uint8_t store_load[14];
      uint8_t store_alu[14];
   };
   static const vector_case cases[] = {
      {2,
       0,
       2,
       AGX_APPLE9_DEVICE_LOAD_FIRST,
       {0x67, 0x10, 0x44, 0x00, 0x00, 0x02, 0x20, 0x00, 0x59, 0x01, 0x00, 0x40,
        0x48, 0x00},
       {0xe7, 0x00, 0x56, 0x00, 0x01, 0x02, 0x21, 0x00, 0x19, 0x00, 0x00, 0x10,
        0x12, 0x00},
       {0xe7, 0x00, 0x54, 0x00, 0x01, 0x02, 0x21, 0x00, 0x19, 0x00, 0x00, 0x10,
        0x12, 0x00}},
      {3,
       4,
       7,
       AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
       {0x67, 0x00, 0x54, 0x08, 0x00, 0x07, 0x20, 0x00, 0x5d, 0x01, 0x00, 0x40,
        0x40, 0x00},
       {0xe7, 0x00, 0x56, 0x00, 0x01, 0x07, 0x21, 0x00, 0x1d, 0x00, 0x00, 0x10,
        0x10, 0x00},
       {0xe7, 0x00, 0x54, 0x00, 0x01, 0x07, 0x21, 0x00, 0x1d, 0x00, 0x00, 0x10,
        0x10, 0x00}},
      {4,
       0,
       4,
       AGX_APPLE9_DEVICE_LOAD_FIRST,
       {0x67, 0x10, 0x44, 0x00, 0x00, 0x04, 0x20, 0x00, 0x57, 0x01, 0x00, 0x40,
        0x40, 0x00},
       {0xe7, 0x00, 0x56, 0x00, 0x01, 0x04, 0x21, 0x00, 0x17, 0x00, 0x00, 0x10,
        0x10, 0x00},
       {0xe7, 0x00, 0x54, 0x00, 0x01, 0x04, 0x21, 0x00, 0x17, 0x00, 0x00, 0x10,
        0x10, 0x00}},
   };

   agx_apple9_packed_instruction packed = {};
   for (const auto &test : cases) {
      SCOPED_TRACE(test.components);
      ASSERT_TRUE(agx_apple9_pack_device_load_vector_u32_raw(
         test.dst, test.index, 0, test.components,
         AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR, test.flags, false,
         AGX_APPLE9_DEVICE_LOAD_TOKEN_5101, &packed));
      EXPECT_EQ(memcmp(packed.bytes, test.load, sizeof(test.load)), 0);

      ASSERT_TRUE(agx_apple9_pack_device_store_vector_u32(
         0, test.index, 1, test.components,
         AGX_APPLE9_SCOREBOARD_SLOT_6, true, &packed));
      EXPECT_EQ(memcmp(packed.bytes, test.store_load, sizeof(test.store_load)),
                0);

      ASSERT_TRUE(agx_apple9_pack_device_store_vector_u32(
         0, test.index, 1, test.components,
         AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
      EXPECT_EQ(memcmp(packed.bytes, test.store_alu, sizeof(test.store_alu)),
                0);
   }
}

TEST(Apple9Packer, NarrowScalarMemoryMatchesOwnMslCorpus)
{
   agx_apple9_packed_instruction packed = {};

   ASSERT_TRUE(agx_apple9_pack_device_load_scalar_raw(
      1, 0, 1, 8, AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
      AGX_APPLE9_DEVICE_LOAD_FIRST, false, AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
      &packed));
   static const uint8_t load_u8[] = {
      0x67, 0x10, 0x44, 0x02, 0x01, 0x00, 0x20,
      0x00, 0x61, 0x01, 0x00, 0x40, 0x42, 0x00,
   };
   EXPECT_EQ(packed.length, sizeof(load_u8));
   EXPECT_EQ(memcmp(packed.bytes, load_u8, sizeof(load_u8)), 0);

   ASSERT_TRUE(agx_apple9_pack_device_load_scalar_raw(
      1, 0, 2, 16, AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
      AGX_APPLE9_DEVICE_LOAD_FIRST | AGX_APPLE9_DEVICE_LOAD_HAS_NEXT, false,
      AGX_APPLE9_DEVICE_LOAD_TOKEN_5101, &packed));
   static const uint8_t load_u16[] = {
      0x67, 0x10, 0x54, 0x02, 0x02, 0x00, 0x20,
      0x00, 0x41, 0x01, 0x00, 0x40, 0x44, 0x00,
   };
   EXPECT_EQ(memcmp(packed.bytes, load_u16, sizeof(load_u16)), 0);

   ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
      0, 0, 0, 8, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
   static const uint8_t store_u8[] = {
      0xe7, 0x00, 0x54, 0x00, 0x00, 0x00, 0x21,
      0x00, 0x21, 0x00, 0x00, 0x90, 0x10, 0x00,
   };
   EXPECT_EQ(memcmp(packed.bytes, store_u8, sizeof(store_u8)), 0);

   ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
      0, 0, 0, 16, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
   static const uint8_t store_u16[] = {
      0xe7, 0x00, 0x54, 0x00, 0x00, 0x00, 0x21,
      0x00, 0x01, 0x00, 0x00, 0x10, 0x11, 0x00,
   };
   EXPECT_EQ(memcmp(packed.bytes, store_u16, sizeof(store_u16)), 0);
}

TEST(Apple9Packer, DeviceStoreEncodesAllocatedDataRegister)
{
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
      17, 9, 3, 32, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
   EXPECT_EQ(packed.bytes[2], 0x54);
   EXPECT_EQ(packed.bytes[3], 34);
   EXPECT_EQ(packed.bytes[4], 3);
   EXPECT_EQ(packed.bytes[5], 9);

   ASSERT_TRUE(agx_apple9_pack_device_store_vector_u32(
      40, 7, 2, 4, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
   EXPECT_EQ(packed.bytes[3], 80);
   EXPECT_FALSE(agx_apple9_pack_device_store_scalar(
      1, 0, 0, 16, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
}

TEST(Apple9Packer, DeviceStoreIndexLifetimeMatchesNativeAccessDescriptor)
{
   agx_apple9_packed_instruction packed = {};

   ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
      17, 9, 3, 32, AGX_APPLE9_SCOREBOARD_SLOT_NONE, false, &packed));
   EXPECT_EQ(packed.bytes[6], 0x20);

   ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
      17, 9, 3, 32, AGX_APPLE9_SCOREBOARD_SLOT_NONE, true, &packed));
   EXPECT_EQ(packed.bytes[6], 0x21);

   ASSERT_TRUE(agx_apple9_pack_device_store_vector_u32(
      40, 7, 2, 4, AGX_APPLE9_SCOREBOARD_SLOT_NONE, false, &packed));
   EXPECT_EQ(packed.bytes[6], 0x20);
}

TEST(Apple9Packer, GlobalInvocationIdAxesMatchNativeSelectors)
{
   static const uint8_t expected[3][4] = {
      {0x0c, 0xa0, 0x10, 0x06},
      {0x0c, 0xa1, 0x10, 0x06},
      {0x0c, 0xa2, 0x10, 0x06},
   };
   agx_apple9_packed_instruction packed;
   for (unsigned component = 0; component < 3; ++component) {
      ASSERT_TRUE(agx_apple9_pack_get_global_id(0, component, &packed));
      ASSERT_EQ(packed.length, sizeof(expected[component]));
      EXPECT_EQ(memcmp(packed.bytes, expected[component], packed.length), 0);
   }
   EXPECT_FALSE(agx_apple9_pack_get_global_id(0, 3, &packed));
}

TEST(Apple9Packer, NarrowSystemValuesUseNativeZeroExtendPair)
{
   static const uint8_t expected[] = {
      0x14, 0xa4, 0x10, 0x06, 0x13, 0x00, 0x00, 0x01,
   };
   agx_apple9_packed_instruction packed;
   ASSERT_TRUE(agx_apple9_pack_get_sr_zext16(1, 0xa4, &packed));
   ASSERT_EQ(packed.length, sizeof(expected));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);
   EXPECT_FALSE(agx_apple9_pack_get_sr_zext16(16, 0xa4, &packed));

   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_GET_SR_ZEXT16, AGX_APPLE9_OPERAND_DEST, 15, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_GET_SR_ZEXT16, AGX_APPLE9_OPERAND_DEST, 16, 32));
}

TEST(Apple9Packer, ScalarConversionsAndArithmeticShiftMatchT8132Forms)
{
   uint8_t phys[2] = {16, 17};
   agx_apple9_vir_instr instruction = {};
   instruction.dest = 0;
   instruction.src[0] = 1;
   instruction.nr_srcs = 1;
   agx_apple9_packed_instruction packed;
   const char *reason = nullptr;

   instruction.op = AGX_APPLE9_VIR_I2F32;
   instruction.encoding = AGX_APPLE9_ENC_SINT_TO_FLOAT;
   static const uint8_t i2f[] = {0xa7, 0x07, 0x54, 0x20,
                                 0x03, 0x44, 0xac, 0x60};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, i2f, sizeof(i2f)), 0);

   instruction.op = AGX_APPLE9_VIR_U2F32;
   instruction.encoding = AGX_APPLE9_ENC_UINT_TO_FLOAT;
   static const uint8_t u2f[] = {0xa7, 0x07, 0x54, 0x20,
                                 0x03, 0x44, 0xac, 0x20};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, u2f, sizeof(u2f)), 0);

   instruction.live_after_mask = 1;
   static const uint8_t retained_u2f[] = {0xa7, 0x07, 0x54, 0x20,
                                          0x03, 0x44, 0x8c, 0x20};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, retained_u2f, sizeof(retained_u2f)), 0);

   instruction.op = AGX_APPLE9_VIR_I2F32;
   instruction.encoding = AGX_APPLE9_ENC_SINT_TO_FLOAT;
   static const uint8_t retained_i2f[] = {0xa7, 0x07, 0x54, 0x20,
                                          0x03, 0x44, 0x8c, 0x60};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, retained_i2f, sizeof(retained_i2f)), 0);
   instruction.live_after_mask = 0;

   instruction.op = AGX_APPLE9_VIR_F2I32;
   instruction.encoding = AGX_APPLE9_ENC_FLOAT_TO_SINT;
   static const uint8_t f2i[] = {0x27, 0x07, 0x54, 0x20, 0x03,
                                 0x44, 0xb4, 0x48, 0x03, 0x00};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, f2i, sizeof(f2i)), 0);

   instruction.op = AGX_APPLE9_VIR_F2U32;
   instruction.encoding = AGX_APPLE9_ENC_FLOAT_TO_UINT;
   static const uint8_t f2u[] = {0x27, 0x07, 0x54, 0x20, 0x03,
                                 0x44, 0xb4, 0x08, 0x03, 0x00};
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&instruction, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, f2u, sizeof(f2u)), 0);

   instruction.op = AGX_APPLE9_VIR_ISHR;
   instruction.encoding = AGX_APPLE9_ENC_SHIFT_EXTENDED;
   instruction.immediate = 7;
   uint8_t shift_phys[2] = {12, 13};
   static const uint8_t ishr[] = {0xa7, 0x01, 0x54, 0x18, 0x02,
                                  0x34, 0x1c, 0x78, 0x62, 0x00};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&instruction, shift_phys,
                                               &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, ishr, sizeof(ishr)), 0);

   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_SHIFT_EXTENDED, AGX_APPLE9_OPERAND_DEST, 15, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_SHIFT_EXTENDED, AGX_APPLE9_OPERAND_DEST, 16, 32));
}

TEST(Apple9Machine, AllocatorSafeExtendedIntegerForms)
{
   EXPECT_TRUE(
      agx_apple9_encoding_info(AGX_APPLE9_ENC_INT_ADD_EXTENDED)->allocator_safe);
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_INT_ADD_EXTENDED, AGX_APPLE9_OPERAND_DEST, 95, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      AGX_APPLE9_ENC_INT_ADD_EXTENDED, AGX_APPLE9_OPERAND_SRC0, 95, 32));
   EXPECT_TRUE(
      agx_apple9_encoding_info(AGX_APPLE9_ENC_INT_MAD_EXTENDED)->allocator_safe);
   for (auto role : {AGX_APPLE9_OPERAND_DEST, AGX_APPLE9_OPERAND_SRC0,
                     AGX_APPLE9_OPERAND_SRC1, AGX_APPLE9_OPERAND_SRC2}) {
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
         AGX_APPLE9_ENC_INT_MAD_EXTENDED, role, 95, 32));
   }
   EXPECT_TRUE(
      agx_apple9_encoding_info(AGX_APPLE9_ENC_MINMAX_COMPACT)->allocator_safe);
   for (auto role : {AGX_APPLE9_OPERAND_DEST, AGX_APPLE9_OPERAND_SRC0,
                     AGX_APPLE9_OPERAND_SRC1}) {
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(AGX_APPLE9_ENC_MINMAX_COMPACT,
                                                  role, 95, 32));
      EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
         AGX_APPLE9_ENC_MINMAX_COMPACT, role, 96, 32));
   }
}

TEST(Apple9Machine, WideSelectHasAsymmetricReach)
{
   const auto encoding = AGX_APPLE9_ENC_SELECT_GPR_WIDE;
   EXPECT_TRUE(agx_apple9_encoding_info(encoding)->allocator_safe);
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(
      encoding, AGX_APPLE9_OPERAND_DEST, 15, 32));
   EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(
      encoding, AGX_APPLE9_OPERAND_DEST, 16, 32));

   for (auto role : {AGX_APPLE9_OPERAND_SRC0, AGX_APPLE9_OPERAND_SRC1,
                     AGX_APPLE9_OPERAND_SRC2, AGX_APPLE9_OPERAND_SRC3}) {
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(encoding, role, 95, 32));
      EXPECT_FALSE(agx_apple9_encoding_accepts_gpr(encoding, role, 96, 32));
   }
}

TEST(Apple9Machine, LogicFormsEncodeMeasuredRegisterBanks)
{
   const auto extended = AGX_APPLE9_ENC_LOGIC_EXTENDED;
   EXPECT_TRUE(agx_apple9_encoding_info(extended)->allocator_safe);
   for (auto role : {AGX_APPLE9_OPERAND_DEST, AGX_APPLE9_OPERAND_SRC0,
                     AGX_APPLE9_OPERAND_SRC1}) {
      EXPECT_TRUE(agx_apple9_encoding_accepts_gpr(extended, role, 95, 32));
   }

   const unsigned two_high[] = {95, 64, 2};
   const unsigned both_sources_high[] = {0, 64, 65};
   const unsigned all_high[] = {95, 64, 65};
   EXPECT_TRUE(
      agx_apple9_encoding_accepts_gpr_tuple(extended, two_high, 3, 32));
   EXPECT_TRUE(agx_apple9_encoding_accepts_gpr_tuple(extended,
                                                     both_sources_high, 3, 32));
   EXPECT_FALSE(
      agx_apple9_encoding_accepts_gpr_tuple(extended, all_high, 3, 32));
}

TEST(Apple9Machine, StoreDataAndIndexAreAllocatable)
{
   const auto *data = agx_apple9_find_operand(AGX_APPLE9_ENC_DEVICE_STORE,
                                              AGX_APPLE9_OPERAND_STORE_DATA);
   ASSERT_NE(data, nullptr);
   EXPECT_EQ(data->files, AGX_APPLE9_FILE_GPR);
   EXPECT_TRUE(data->flags & AGX_APPLE9_OPERAND_ALLOCATABLE);
   EXPECT_EQ(data->max_index, 63);
   EXPECT_TRUE(
      agx_apple9_encoding_info(AGX_APPLE9_ENC_DEVICE_STORE)->allocator_safe);
}

TEST(Apple9Machine, DependencyLayoutsAreEncodingProperties)
{
   static const struct {
      agx_apple9_encoding encoding;
      agx_apple9_dependency_layout layout;
   } cases[] = {
      {AGX_APPLE9_ENC_FLOAT2_COMPACT,
       AGX_APPLE9_DEPENDENCY_INDEX_45_47},
      {AGX_APPLE9_ENC_FLOAT3_EXTENDED,
       AGX_APPLE9_DEPENDENCY_INDEX_61_63},
      {AGX_APPLE9_ENC_INT_ADD_EXTENDED,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_INT_MAD_EXTENDED,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_UINT_TO_FLOAT,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_FLOAT_TO_UINT,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_FLOAT_RECIPROCAL,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_SHIFT_EXTENDED,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_DEVICE_STORE,
       AGX_APPLE9_DEPENDENCY_MASK_12_17},
      {AGX_APPLE9_ENC_LOGIC_EXTENDED,
       AGX_APPLE9_DEPENDENCY_MASK_45_47_61_63},
      {AGX_APPLE9_ENC_DEVICE_LOAD, AGX_APPLE9_DEPENDENCY_NONE},
   };

   for (const auto &test : cases)
      EXPECT_EQ(agx_apple9_encoding_info(test.encoding)->dependency_layout,
                test.layout);
}

TEST(Apple9Packer, RegisterFormsMatchValidatedProbeTemplates)
{
   uint8_t phys[] = {64, 2, 95, 5};
   agx_apple9_vir_instr add = {
      .op = AGX_APPLE9_VIR_IADD,
      .encoding = AGX_APPLE9_ENC_INT_ADD_EXTENDED,
      .dest = 0,
      .src = {1, 2},
      .nr_srcs = 2,
   };
   agx_apple9_packed_instruction packed = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason))
      << reason;
   ASSERT_EQ(packed.length, 10u);

   /* Same all-sources-dead envelope as EXP-M4-16's hardware probe. */
   uint8_t expected[] = {0x9f, 0x01, 0x54, 0x00, 0x02,
                         0x00, 0x00, 0xa8, 0x17, 0x05};
   uint64_t word = 0;
   memcpy(&word, expected, sizeof(word));
   word &= ~(((1ull << 7) - 1) << 25);
   word |= 64ull << 25;
   word &= ~(((1ull << 7) - 1) << 42);
   word |= 2ull << 42;
   word &= ~(((1ull << 7) - 1) << 51);
   word |= 95ull << 51;
   memcpy(expected, &word, sizeof(word));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);

   uint8_t select_phys[] = {15, 95, 64, 63, 94};
   agx_apple9_vir_instr select = {
      .op = AGX_APPLE9_VIR_SELECT,
      .encoding = AGX_APPLE9_ENC_SELECT_GPR_WIDE,
      .dest = 0,
      .src = {1, 2, 3, 4},
      .immediate = AGX_APPLE9_SELECT_ULT,
      .nr_srcs = 4,
      .scoreboard_slot = 6,
   };
   ASSERT_TRUE(
      agx_apple9_pack_vir_instruction(&select, select_phys, &packed, &reason))
      << reason;
   static const uint8_t expected_select[] = {
      0xf2, 0xbf, 0x67, 0x81, 0x02, 0xfe, 0x05, 0xd5, 0x40, 0xbc,
   };
   ASSERT_EQ(packed.length, sizeof(expected_select));
   EXPECT_EQ(memcmp(packed.bytes, expected_select, sizeof(expected_select)), 0);
}

TEST(Apple9Packer, CompactMoveImmediateStopsAtSevenBitBoundary)
{
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_mov_imm(15, 0x7f, &packed));
   static const uint8_t expected[] = {0xfc, 0x7f};
   ASSERT_EQ(packed.length, sizeof(expected));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);
   EXPECT_FALSE(agx_apple9_pack_mov_imm(15, 0x80, &packed));
   EXPECT_FALSE(agx_apple9_pack_mov_imm(0, 0xff, &packed));
}

TEST(Apple9Packer, RawMoveImmediatePacksSixBitDestination)
{
   static const struct {
      unsigned dst;
      uint32_t value;
      uint8_t bytes[8];
   } cases[] = {
      {0, 0x12345678, {0x0c, 0xf8, 0x02, 0x12, 0x18, 0x08, 0xa2, 0x01}},
      {18, 0x3f800000, {0x2c, 0x80, 0x42, 0x3e, 0x00, 0x00, 0x00, 0x0c}},
      {34, 0x01020304, {0x2c, 0x84, 0x82, 0x00, 0x0c, 0x00, 0x10, 0x08}},
      {63, 0xffffffff, {0xfc, 0xff, 0xc2, 0xfe, 0x1e, 0x0c, 0xff, 0x0f}},
   };

   for (const auto &test : cases) {
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_mov_imm32(test.dst, test.value, &packed));
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0);
   }

   agx_apple9_packed_instruction packed = {};
   EXPECT_FALSE(agx_apple9_pack_mov_imm32(64, 0x12345678, &packed));
}

TEST(Apple9Packer, ExtendedLogicCarriesSourceLiveness)
{
   uint8_t phys[] = {4, 2, 3};
   agx_apple9_vir_instr logic = {
      .op = AGX_APPLE9_VIR_IXOR,
      .encoding = AGX_APPLE9_ENC_LOGIC_EXTENDED,
      .dest = 0,
      .src = {1, 2},
      .nr_srcs = 2,
   };

   static const struct {
      uint8_t mask;
      uint8_t bytes[10];
   } cases[] = {
      {0x0, {0x4b, 0x05, 0x1e, 0x07, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00}},
      {0x1, {0x4b, 0x85, 0x16, 0x07, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00}},
      {0x2, {0x4b, 0x05, 0x2e, 0x87, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00}},
      {0x3, {0x4b, 0x85, 0x26, 0x87, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00}},
   };

   const char *reason = nullptr;
   for (const auto &test : cases) {
      logic.live_after_mask = test.mask;
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&logic, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0)
         << "live_after_mask=" << unsigned(test.mask);
   }
}

TEST(Apple9Packer, ExtendedLogicUsesOneHotPendingSlotMask)
{
   uint8_t phys[] = {4, 2, 3};
   const char *reason = nullptr;
   for (auto op :
        {AGX_APPLE9_VIR_IAND, AGX_APPLE9_VIR_IOR, AGX_APPLE9_VIR_IXOR}) {
      agx_apple9_vir_instr logic = {
         .op = op,
         .encoding = AGX_APPLE9_ENC_LOGIC_EXTENDED,
         .dest = 0,
         .src = {1, 2},
         .nr_srcs = 2,
      };

      for (unsigned slot = 0; slot <= 6; ++slot) {
         logic.scoreboard_slot = slot;
         agx_apple9_packed_instruction packed = {};
         ASSERT_TRUE(
            agx_apple9_pack_vir_instruction(&logic, phys, &packed, &reason))
            << reason;
         ASSERT_EQ(packed.length, 10u);
         const unsigned mask = ((packed.bytes[5] >> 5) & 0x7) |
                               (((packed.bytes[7] >> 5) & 0x7) << 3);
         EXPECT_EQ(mask, slot == 0 ? 0u : 1u << (slot - 1))
            << "op=" << unsigned(op) << " slot=" << slot;
      }
   }
}

TEST(Apple9Packer, IntegerFamilyUsesSharedOneHotDependencyLayout)
{
   uint8_t phys[] = {4, 2, 3, 5};
   agx_apple9_vir_instr instructions[] = {
      {
         .op = AGX_APPLE9_VIR_IADD,
         .encoding = AGX_APPLE9_ENC_INT_ADD_EXTENDED,
         .dest = 0,
         .src = {1, 2},
         .nr_srcs = 2,
      },
      {
         .op = AGX_APPLE9_VIR_IMAD,
         .encoding = AGX_APPLE9_ENC_INT_MAD_EXTENDED,
         .dest = 0,
         .src = {1, 2, 3},
         .nr_srcs = 3,
      },
      {
         .op = AGX_APPLE9_VIR_U2F32,
         .encoding = AGX_APPLE9_ENC_UINT_TO_FLOAT,
         .dest = 0,
         .src = {1},
         .nr_srcs = 1,
      },
      {
         .op = AGX_APPLE9_VIR_F2U32,
         .encoding = AGX_APPLE9_ENC_FLOAT_TO_UINT,
         .dest = 0,
         .src = {1},
         .nr_srcs = 1,
      },
      {
         .op = AGX_APPLE9_VIR_FRCP,
         .encoding = AGX_APPLE9_ENC_FLOAT_RECIPROCAL,
         .dest = 0,
         .src = {1},
         .immediate = 0x02,
         .nr_srcs = 1,
      },
      {
         .op = AGX_APPLE9_VIR_ISHR,
         .encoding = AGX_APPLE9_ENC_SHIFT_EXTENDED,
         .dest = 0,
         .src = {1},
         .immediate = 3,
         .nr_srcs = 1,
      },
   };

   const char *reason = nullptr;
   for (auto &instruction : instructions) {
      for (unsigned slot = 0; slot <= 6; ++slot) {
         instruction.scoreboard_slot = slot;
         agx_apple9_packed_instruction packed = {};
         ASSERT_TRUE(agx_apple9_pack_vir_instruction(
            &instruction, phys, &packed, &reason))
            << "op=" << unsigned(instruction.op) << " slot=" << slot << " "
            << (reason ? reason : "");
         const unsigned mask =
            (packed.bytes[1] >> 4) | ((packed.bytes[2] & 0x3) << 4);
         EXPECT_EQ(mask, slot == 0 ? 0u : 1u << (slot - 1))
            << "op=" << unsigned(instruction.op) << " slot=" << slot;
      }
   }

   for (unsigned slot = 0; slot <= 6; ++slot) {
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_device_store_scalar(
         4, 2, 0, 32, (agx_apple9_scoreboard_slot)slot, true, &packed));
      const unsigned mask =
         (packed.bytes[1] >> 4) | ((packed.bytes[2] & 0x3) << 4);
      EXPECT_EQ(mask, slot == 0 ? 0u : 1u << (slot - 1))
         << "store slot=" << slot;
   }
}

TEST(Apple9Packer, IntegerMinmaxPreservesNativeAluState)
{
   uint8_t phys[] = {4, 2, 3};
   agx_apple9_vir_instr minmax = {
      .op = AGX_APPLE9_VIR_UMIN,
      .encoding = AGX_APPLE9_ENC_MINMAX_COMPACT,
      .dest = 0,
      .src = {1, 2},
      .nr_srcs = 2,
   };

   static const struct {
      uint8_t mask;
      uint8_t bytes[6];
   } cases[] = {
      {0x0, {0x42, 0x05, 0x3e, 0x07, 0x05, 0x00}},
      {0x1, {0x42, 0x85, 0x36, 0x07, 0x05, 0x00}},
      {0x2, {0x42, 0x05, 0x2e, 0x87, 0x05, 0x00}},
      {0x3, {0x42, 0x85, 0x26, 0x87, 0x05, 0x00}},
   };

   const char *reason = nullptr;
   for (const auto &test : cases) {
      minmax.live_after_mask = test.mask;
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&minmax, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0)
         << "live_after_mask=" << unsigned(test.mask);
   }
}

TEST(Apple9Packer, CompactFloatCarriesNativeStateAndRelease)
{
   uint8_t phys[] = {4, 2, 3};
   agx_apple9_vir_instr add = {
      .op = AGX_APPLE9_VIR_FADD,
      .encoding = AGX_APPLE9_ENC_FLOAT2_COMPACT,
      .dest = 0,
      .src = {1, 2},
      .nr_srcs = 2,
   };
   agx_apple9_vir_instr minimum = add;
   minimum.op = AGX_APPLE9_VIR_FMIN;
   minimum.encoding = AGX_APPLE9_ENC_MINMAX_COMPACT;

   static const struct {
      uint8_t mask;
      uint8_t add[6];
      uint8_t minimum[6];
   } cases[] = {
      {0x0,
       {0x49, 0x05, 0x3c, 0x07, 0x00, 0x00},
       {0x42, 0x05, 0x3e, 0x07, 0x01, 0x00}},
      {0x1,
       {0x49, 0x85, 0x34, 0x07, 0x00, 0x00},
       {0x42, 0x85, 0x36, 0x07, 0x01, 0x00}},
      {0x2,
       {0x49, 0x05, 0x2c, 0x87, 0x00, 0x00},
       {0x42, 0x05, 0x2e, 0x87, 0x01, 0x00}},
      {0x3,
       {0x49, 0x85, 0x24, 0x87, 0x00, 0x00},
       {0x42, 0x85, 0x26, 0x87, 0x01, 0x00}},
   };

   const char *reason = nullptr;
   for (const auto &test : cases) {
      add.live_after_mask = test.mask;
      minimum.live_after_mask = test.mask;
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.add));
      EXPECT_EQ(memcmp(packed.bytes, test.add, sizeof(test.add)), 0)
         << "fadd live_after_mask=" << unsigned(test.mask);

      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&minimum, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.minimum));
      EXPECT_EQ(memcmp(packed.bytes, test.minimum, sizeof(test.minimum)), 0)
         << "fmin live_after_mask=" << unsigned(test.mask);
   }

   add.live_after_mask = minimum.live_after_mask = 0x3;
   add.scoreboard_slot = minimum.scoreboard_slot = 6;
   static const uint8_t load_add[] = {
      0x49, 0x85, 0x24, 0x87, 0x00, 0xc0,
   };
   static const uint8_t load_minimum[] = {
      0x42, 0x85, 0x26, 0x87, 0x01, 0xc0,
   };
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, load_add, sizeof(load_add)), 0);
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&minimum, phys, &packed, &reason))
      << reason;
   EXPECT_EQ(memcmp(packed.bytes, load_minimum, sizeof(load_minimum)), 0);
}

TEST(Apple9Packer, CompactBinaryAluPacksAllScatteredRegisterBits)
{
   uint8_t phys[] = {95, 64, 79};
   agx_apple9_vir_instr add = {
      .op = AGX_APPLE9_VIR_FADD,
      .encoding = AGX_APPLE9_ENC_FLOAT2_COMPACT,
      .dest = 0,
      .src = {1, 2},
      .nr_srcs = 2,
   };
   agx_apple9_vir_instr minimum = add;
   minimum.op = AGX_APPLE9_VIR_UMIN;
   minimum.encoding = AGX_APPLE9_ENC_MINMAX_COMPACT;

   static const uint8_t expected_add[] = {
      0xf9, 0x01, 0x7c, 0x1f, 0x00, 0x15,
   };
   static const uint8_t expected_minimum[] = {
      0xf2, 0x01, 0x7e, 0x1f, 0x05, 0x15,
   };

   const char *reason = nullptr;
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason))
      << reason;
   ASSERT_EQ(packed.length, sizeof(expected_add));
   EXPECT_EQ(memcmp(packed.bytes, expected_add, sizeof(expected_add)), 0);

   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&minimum, phys, &packed, &reason))
      << reason;
   ASSERT_EQ(packed.length, sizeof(expected_minimum));
   EXPECT_EQ(memcmp(packed.bytes, expected_minimum, sizeof(expected_minimum)),
             0);
}

TEST(Apple9Packer, FmaCarriesNativeStateAndRelease)
{
   uint8_t phys[] = {4, 2, 3, 5};
   agx_apple9_vir_instr fma = {
      .op = AGX_APPLE9_VIR_FMA,
      .encoding = AGX_APPLE9_ENC_FLOAT3_EXTENDED,
      .dest = 0,
      .src = {1, 2, 3},
      .nr_srcs = 3,
   };

   static const struct {
      uint8_t mask;
      uint8_t bytes[8];
   } cases[] = {
      {0x0, {0x49, 0x05, 0x3e, 0x07, 0x81, 0x0a, 0x02, 0x00}},
      {0x1, {0x49, 0x85, 0x36, 0x07, 0x81, 0x0a, 0x02, 0x00}},
      {0x2, {0x49, 0x05, 0x2e, 0x87, 0x81, 0x0a, 0x02, 0x00}},
      {0x4, {0x49, 0x05, 0x3e, 0x07, 0x01, 0x8a, 0x02, 0x00}},
      {0x7, {0x49, 0x85, 0x26, 0x87, 0x01, 0x8a, 0x02, 0x00}},
   };

   const char *reason = nullptr;
   for (const auto &test : cases) {
      fma.live_after_mask = test.mask;
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_vir_instruction(&fma, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0)
         << "live_after_mask=" << unsigned(test.mask);
   }

   fma.live_after_mask = 0;
   fma.scoreboard_slot = 6;
   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&fma, phys, &packed, &reason))
      << reason;
   static const uint8_t load_fma[] = {
      0x49, 0x05, 0x3e, 0x07, 0x81, 0x0a, 0x02, 0xc0,
   };
   EXPECT_EQ(memcmp(packed.bytes, load_fma, sizeof(load_fma)), 0);
}

TEST(Apple9Packer, ImmediateFloatCarriesScoreboardSlot)
{
   uint8_t phys[] = {2, 2};
   agx_apple9_vir_instr add = {
      .op = AGX_APPLE9_VIR_FADD_IMM,
      .encoding = AGX_APPLE9_ENC_FLOAT2_IMMEDIATE_COMPACT,
      .dest = 0,
      .src = {1},
      .immediate = 0x40000000u,
      .nr_srcs = 1,
   };
   static const struct {
      uint8_t slot;
      uint8_t bytes[6];
   } cases[] = {
      {0, {0x29, 0xc1, 0x34, 0x05, 0x80, 0x00}},
      {1, {0x29, 0xc1, 0x34, 0x05, 0x80, 0x20}},
      {2, {0x29, 0xc1, 0x34, 0x05, 0x80, 0x40}},
      {3, {0x29, 0xc1, 0x34, 0x05, 0x80, 0x60}},
      {4, {0x29, 0xc1, 0x34, 0x05, 0x80, 0x80}},
      {5, {0x29, 0xc1, 0x34, 0x05, 0x80, 0xa0}},
      {6, {0x29, 0xc1, 0x34, 0x05, 0x80, 0xc0}},
   };

   const char *reason = nullptr;
   for (const auto &test : cases) {
      add.scoreboard_slot = test.slot;
      agx_apple9_packed_instruction packed = {};
      ASSERT_TRUE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, sizeof(test.bytes));
      EXPECT_EQ(memcmp(packed.bytes, test.bytes, sizeof(test.bytes)), 0)
         << "slot=" << unsigned(test.slot);
   }

   add.scoreboard_slot = 7;
   agx_apple9_packed_instruction packed = {};
   EXPECT_FALSE(agx_apple9_pack_vir_instruction(&add, phys, &packed, &reason));
   ASSERT_NE(reason, nullptr);
   EXPECT_NE(strstr(reason, "dependency"), nullptr);
}

TEST(Apple9Vir, ScalarLoadAutoStartsAtSlot6)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t load =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                          AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 0);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, load, AGX_APPLE9_DEVICE_LOAD_FIRST,
      AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   uint32_t sources[] = {load, ordinary};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(program.instructions[0].producer_scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_6);
   EXPECT_EQ(program.instructions[0].device_load_raw_token,
             AGX_APPLE9_DEVICE_LOAD_TOKEN_5101);
   EXPECT_EQ(program.instructions[1].scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_6);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, IndependentPendingGroupsAdvanceAndReuseSlots)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t loads[4];
   for (unsigned i = 0; i < 3; ++i) {
      loads[i] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                     AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   }

   uint32_t sources0[] = {loads[0], ordinary};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, sources0, 2, 0);
   loads[3] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                  AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 3);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, loads[3], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   for (unsigned i = 1; i < 4; ++i) {
      uint32_t sources[] = {loads[i], ordinary};
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                          AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   }

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   const uint8_t expected[] = {6, 1, 2, 6};
   unsigned seen = 0;
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      if (program.instructions[i].op == AGX_APPLE9_VIR_DEVICE_LOAD) {
         EXPECT_EQ(program.instructions[i].producer_scoreboard_slot,
                   expected[seen++]);
      }
   }
   EXPECT_EQ(seen, 4u);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, ScalarLoadPreferenceUsesFirstFreeSlotAcrossGaps)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t loads[7];

   for (unsigned i = 0; i < 6; ++i) {
      loads[i] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                     AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   }

   /* Slot 2 is third in the scalar-load preference and is the first slot
    * released.  Slots 6, 1, 3, 4, and 5 remain pending across the new load. */
   uint32_t release_sources[] = {loads[2], ordinary};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, release_sources, 2, 0);
   loads[6] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                  AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 6);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, loads[6], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));

   for (unsigned i : {0u, 1u, 3u, 4u, 5u, 6u}) {
      uint32_t sources[] = {loads[i], ordinary};
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                          AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   }

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   const uint8_t expected[] = {6, 1, 2, 3, 4, 5, 2};
   unsigned seen = 0;
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      if (program.instructions[i].op == AGX_APPLE9_VIR_DEVICE_LOAD) {
         EXPECT_EQ(program.instructions[i].producer_scoreboard_slot,
                   expected[seen++]);
      }
   }
   EXPECT_EQ(seen, 7u);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, SevenIndependentPendingGroupsMaterializeOldestHandoff)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t loads[7];

   for (unsigned i = 0; i < 7; ++i) {
      loads[i] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                     AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   }
   program.instructions[6].device_load_index_kind =
      AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR;
   uint32_t last_result = AGX_APPLE9_VREG_INVALID;
   for (unsigned i = 0; i < 7; ++i) {
      uint32_t sources[] = {loads[i], ordinary};
      last_result =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                             AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   }
   program.output = last_result;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   const uint8_t expected[] = {6, 6, 1, 2, 3, 4, 5};
   unsigned seen = 0, materialized = 0;
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      if (program.instructions[i].op == AGX_APPLE9_VIR_DEVICE_LOAD) {
         EXPECT_EQ(program.instructions[i].producer_scoreboard_slot,
                   expected[seen++]);
      }
      materialized += program.instructions[i].scoreboard_materialize;
   }
   EXPECT_EQ(seen, 7u);
   EXPECT_EQ(materialized, 1u);
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   bool general_materialization = false;
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      if (program.instructions[i].scoreboard_materialize) {
         EXPECT_EQ(program.instructions[i].encoding,
                   AGX_APPLE9_ENC_LOGIC_EXTENDED);
         general_materialization |=
            program.phys[program.instructions[i].dest] >= 16;
      }
   }
   EXPECT_TRUE(general_materialization);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, ReciprocalUsesSharedOneHotDependencySlots)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t loads[5];

   for (unsigned i = 0; i < ARRAY_SIZE(loads); ++i) {
      loads[i] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                     AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   }

   uint32_t reciprocal = agx_apple9_vir_emit(
      &program, AGX_APPLE9_VIR_FRCP, AGX_APPLE9_ENC_FLOAT_RECIPROCAL,
      &loads[4], 1, 0x02);
   for (unsigned i = 0; i < 4; ++i) {
      uint32_t sources[] = {loads[i], ordinary};
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                          AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   }
   program.output = reciprocal;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");

   unsigned seen_loads = 0, materializations = 0, reciprocals = 0;
   const uint8_t expected_slots[] = {6, 1, 2, 3, 4};
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      const auto &instruction = program.instructions[i];
      if (instruction.op == AGX_APPLE9_VIR_DEVICE_LOAD) {
         EXPECT_EQ(instruction.producer_scoreboard_slot,
                   expected_slots[seen_loads++]);
      }
      materializations += instruction.scoreboard_materialize;
      if (instruction.op == AGX_APPLE9_VIR_FRCP) {
         ++reciprocals;
         EXPECT_EQ(instruction.scoreboard_slot, AGX_APPLE9_SCOREBOARD_SLOT_4);
      }
   }
   EXPECT_EQ(seen_loads, ARRAY_SIZE(loads));
   EXPECT_EQ(materializations, 0u);
   EXPECT_EQ(reciprocals, 1u);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, MultiSourcePendingLoadsShareOneSlot)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t loads[2];
   for (unsigned i = 0; i < 2; ++i) {
      loads[i] = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                     AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   }
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FMUL,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, loads, 2, 0);

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(program.instructions[0].producer_scoreboard_slot, 6u);
   EXPECT_EQ(program.instructions[1].producer_scoreboard_slot, 6u);
   EXPECT_EQ(program.instructions[0].device_load_raw_token,
             AGX_APPLE9_DEVICE_LOAD_TOKEN_5101);
   EXPECT_EQ(program.instructions[1].device_load_raw_token,
             AGX_APPLE9_DEVICE_LOAD_TOKEN_5101);
   EXPECT_EQ(program.instructions[2].scoreboard_slot, 6u);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, FirstHandoffRetainsGprForLaterReads)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t ordinary = agx_apple9_vir_input(&program, 2);
   uint32_t load =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                          AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 0);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, load, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   ASSERT_TRUE(agx_apple9_vir_add_live_out(&program, index));
   uint32_t sources[] = {load, ordinary};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   uint32_t result =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FMUL,
                          AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);
   program.output = result;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(program.instructions[1].scoreboard_slot, 6u);
   EXPECT_EQ(program.instructions[2].scoreboard_slot, 0u);
   EXPECT_NE(program.instructions[1].live_after_mask & 1u, 0u);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, RawLoadTokenDefinesProducerScoreboardSlot)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t alu = agx_apple9_vir_input(&program, 2);
   const agx_apple9_device_load_contract contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR,
      .group_flags = 0,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_9100,
   };
   uint32_t load =
      agx_apple9_vir_emit_device_load(&program, 0, index, &contract);
   ASSERT_NE(load, AGX_APPLE9_VREG_INVALID);
   uint32_t sources[] = {load, alu};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                       AGX_APPLE9_ENC_FLOAT2_COMPACT, sources, 2, 0);

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(program.instructions[0].device_load_raw_token,
             AGX_APPLE9_DEVICE_LOAD_TOKEN_9100);
   EXPECT_EQ(program.instructions[0].producer_scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_3);
   EXPECT_EQ(program.instructions[1].scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_3);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, IntegerAddDirectlyConsumesSlot6)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t alu = agx_apple9_vir_input(&program, 2);
   uint32_t load =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                          AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 0);
   ASSERT_NE(load, AGX_APPLE9_VREG_INVALID);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, load, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));

   uint32_t sources[] = {load, alu};
   uint32_t result =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IADD,
                          AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources, 2, 0);
   program.output = result;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_EQ(program.instruction_count, 2u);
   EXPECT_EQ(program.instructions[0].producer_scoreboard_slot, 6u);
   EXPECT_EQ(program.instructions[1].scoreboard_slot, 6u);
   EXPECT_EQ(program.instructions[1].op, AGX_APPLE9_VIR_IADD);
   EXPECT_EQ(program.instructions[1].src[0], load);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, AllLogicDirectlyConsumePendingLoadGroups)
{
   for (auto op :
        {AGX_APPLE9_VIR_IAND, AGX_APPLE9_VIR_IOR, AGX_APPLE9_VIR_IXOR}) {
      agx_apple9_vir_program program;
      agx_apple9_vir_init(&program);
      uint32_t index = agx_apple9_vir_input(&program, 1);
      uint32_t loads[2];
      for (unsigned i = 0; i < 2; ++i) {
         loads[i] =
            agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                                AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, i);
         ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
            &program, loads[i], 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
      }
      agx_apple9_vir_emit(&program, op, AGX_APPLE9_ENC_LOGIC_EXTENDED, loads, 2,
                          0);

      const char *reason = nullptr;
      ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
         << (reason ? reason : "") << " op=" << unsigned(op);
      ASSERT_EQ(program.instruction_count, 3u);
      EXPECT_EQ(program.instructions[0].producer_scoreboard_slot, 6u);
      EXPECT_EQ(program.instructions[1].producer_scoreboard_slot, 6u);
      EXPECT_EQ(program.instructions[2].scoreboard_slot, 6u);
      EXPECT_FALSE(program.instructions[2].scoreboard_materialize);
      agx_apple9_vir_finish(&program);
   }
}

TEST(Apple9Vir, LogicNormalizesSinglePendingSourceIntoSourceA)
{
   for (auto op :
        {AGX_APPLE9_VIR_IAND, AGX_APPLE9_VIR_IOR, AGX_APPLE9_VIR_IXOR}) {
      agx_apple9_vir_program program;
      agx_apple9_vir_init(&program);
      uint32_t index = agx_apple9_vir_input(&program, 1);
      uint32_t ordinary = agx_apple9_vir_input(&program, 2);
      uint32_t load =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                             AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 0);
      ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
         &program, load, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));

      /* Deliberately present the pending operand in source B. */
      uint32_t sources[] = {ordinary, load};
      agx_apple9_vir_emit(&program, op, AGX_APPLE9_ENC_LOGIC_EXTENDED, sources,
                          2, 0);

      const char *reason = nullptr;
      ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
         << (reason ? reason : "") << " op=" << unsigned(op);
      ASSERT_EQ(program.instruction_count, 2u);
      EXPECT_EQ(program.instructions[0].producer_scoreboard_slot, 6u);
      EXPECT_EQ(program.instructions[1].scoreboard_slot, 6u);
      EXPECT_EQ(program.instructions[1].src[0], load);
      EXPECT_EQ(program.instructions[1].src[1], ordinary);
      agx_apple9_vir_finish(&program);
   }
}

TEST(Apple9Vir, ExplicitMultiSourceGroupRejectsDifferentSlots)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t alu_a = agx_apple9_vir_input(&program, 2);
   uint32_t alu_b = agx_apple9_vir_input(&program, 3);
   uint32_t load6 =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                          AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 0);
   uint32_t load1 =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_DEVICE_LOAD,
                          AGX_APPLE9_ENC_DEVICE_LOAD, &index, 1, 1);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, load6, 0, AGX_APPLE9_SCOREBOARD_SLOT_6));
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, load1, 0, AGX_APPLE9_SCOREBOARD_SLOT_1));
   uint32_t mixed_arms[4] = {alu_a, alu_b, load6, load1};
   agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_SELECT,
                       AGX_APPLE9_ENC_SELECT_GPR_WIDE, mixed_arms, 4,
                       AGX_APPLE9_SELECT_ULT);
   const char *reason = nullptr;
   EXPECT_FALSE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason));
   ASSERT_NE(reason, nullptr);
   EXPECT_STREQ(reason,
                "Apple9 multi-source scoreboard group uses different slots");
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Vir, VectorStoreDirectlyConsumesItsLoadTuple)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 4);
   const agx_apple9_device_load_contract retained = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };
   const agx_apple9_device_load_contract second = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };

   uint32_t earlier =
      agx_apple9_vir_emit_device_load(&program, 0, index, &retained);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, earlier, AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
      AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   uint32_t vector =
      agx_apple9_vir_emit_device_load_vector(&program, 1, index, 4, &second);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, vector, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));
   uint32_t lanes[] = {vector, vector + 1, vector + 2, vector + 3};
   ASSERT_TRUE(
      agx_apple9_vir_emit_device_store(&program, 2, index, lanes, 4, 32));
   uint32_t output_sources[] = {earlier, earlier};
   program.output =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, output_sources, 2, 0);

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");

   unsigned collect_index = UINT_MAX;
   unsigned store_index = UINT_MAX;
   for (unsigned i = 0; i < program.instruction_count; ++i) {
      collect_index = program.instructions[i].op == AGX_APPLE9_VIR_COLLECT
                         ? i
                         : collect_index;
      store_index = program.instructions[i].op == AGX_APPLE9_VIR_DEVICE_STORE
                       ? i
                       : store_index;
   }
   ASSERT_EQ(collect_index, UINT_MAX);
   ASSERT_NE(store_index, UINT_MAX);
   EXPECT_EQ(program.instructions[store_index].scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_1);
   for (unsigned c = 0; c < 4; ++c)
      EXPECT_EQ(program.instructions[store_index].src[c], vector + c);

   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   for (unsigned c = 1; c < 4; ++c)
      EXPECT_EQ(program.phys[vector + c], program.phys[vector] + c);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Packer, LowRegisterSelectMatchesCallerOwnedCompilerForm)
{
   uint8_t phys[] = {0, 0, 2, 4, 3};
   agx_apple9_vir_instr select = {
      .op = AGX_APPLE9_VIR_SELECT,
      .encoding = AGX_APPLE9_ENC_SELECT_GPR_WIDE,
      .dest = 0,
      .src = {1, 2, 3, 4},
      .immediate = AGX_APPLE9_SELECT_ULT,
      .nr_srcs = 4,
   };
   agx_apple9_packed_instruction packed = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&select, phys, &packed, &reason))
      << reason;
   static const uint8_t expected[] = {
      0x02, 0x01, 0x1f, 0x05, 0x82, 0x08, 0x05, 0x00, 0x80, 0x06,
   };
   ASSERT_EQ(packed.length, sizeof(expected));
   EXPECT_EQ(memcmp(packed.bytes, expected, sizeof(expected)), 0);

   select.live_after_mask = 0x3;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&select, phys, &packed, &reason))
      << reason;
   static const uint8_t expected_live_ab[] = {
      0x02, 0x81, 0x07, 0x85, 0x82, 0x08, 0x05, 0x00, 0x80, 0x06,
   };
   ASSERT_EQ(packed.length, sizeof(expected_live_ab));
   EXPECT_EQ(memcmp(packed.bytes, expected_live_ab, sizeof(expected_live_ab)),
             0);

   select.live_after_mask = 0xf;
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&select, phys, &packed, &reason))
      << reason;
   static const uint8_t expected_live_all[] = {
      0x02, 0x81, 0x07, 0x85, 0x02, 0x88, 0x05, 0x00, 0x00, 0x86,
   };
   ASSERT_EQ(packed.length, sizeof(expected_live_all));
   EXPECT_EQ(memcmp(packed.bytes, expected_live_all, sizeof(expected_live_all)),
             0);

   select.live_after_mask = 0;
   for (uint8_t slot = 0; slot <= 6; ++slot) {
      select.scoreboard_slot = slot;
      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&select, phys, &packed, &reason))
         << reason;
      EXPECT_EQ((packed.bytes[7] >> 5) & 7, slot);
   }

   uint8_t high_phys[] = {0, 64, 65, 66, 67};
   for (uint8_t slot = 0; slot <= 6; ++slot) {
      select.scoreboard_slot = slot;
      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&select, high_phys, &packed, &reason))
         << reason;
      EXPECT_EQ((packed.bytes[7] >> 5) & 7, slot);
   }

   select.scoreboard_slot = 7;
   EXPECT_FALSE(
      agx_apple9_pack_vir_instruction(&select, high_phys, &packed, &reason));
   ASSERT_NE(reason, nullptr);
   EXPECT_NE(strstr(reason, "dependency"), nullptr);
}

TEST(Apple9Packer, SelectConditionAndEqualityModeMatchHardwareSweeps)
{
   uint8_t phys[] = {0, 0, 2, 4, 3};
   agx_apple9_vir_instr select = {
      .op = AGX_APPLE9_VIR_SELECT,
      .encoding = AGX_APPLE9_ENC_SELECT_GPR_WIDE,
      .dest = 0,
      .src = {1, 2, 3, 4},
      .nr_srcs = 4,
   };
   agx_apple9_packed_instruction packed = {};
   const char *reason = nullptr;

   struct {
      uint32_t immediate;
      uint8_t mode;
      uint8_t condition;
   } cases[] = {
      {AGX_APPLE9_SELECT_FGT, 0x82, 0x02},
      {AGX_APPLE9_SELECT_FLT, 0x82, 0x03},
      {AGX_APPLE9_SELECT_UGT, 0x82, 0x04},
      {AGX_APPLE9_SELECT_ULT, 0x82, 0x05},
      {AGX_APPLE9_SELECT_IGT, 0x82, 0x06},
      {AGX_APPLE9_SELECT_ILT, 0x82, 0x07},
      {AGX_APPLE9_SELECT_FEQ | AGX_APPLE9_SELECT_EQUALITY, 0x86, 0x00},
   };

   for (const auto &test : cases) {
      select.immediate = test.immediate;
      ASSERT_TRUE(
         agx_apple9_pack_vir_instruction(&select, phys, &packed, &reason))
         << reason;
      ASSERT_EQ(packed.length, 10u);
      EXPECT_EQ(packed.bytes[4], test.mode);
      EXPECT_EQ(packed.bytes[6], test.condition);
   }
}

TEST(Apple9Allocator, ReleasesKilledSourcesAfterTheirConsumer)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t gid = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_GET_GLOBAL_ID,
                                      AGX_APPLE9_ENC_GET_SR, nullptr, 0, 0);
   uint32_t c =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                          AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, 7);
   uint32_t sources[] = {gid, c};
   uint32_t value =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IADD,
                          AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources, 2, 0);
   program.output = value;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason)) << reason;
   EXPECT_EQ(program.phys[gid], 0u);
   EXPECT_EQ(program.phys[c], 1u);
   EXPECT_NE(program.phys[value], program.phys[gid]);
   EXPECT_NE(program.phys[value], program.phys[c]);
   EXPECT_GE(program.phys[value], 16u);
   EXPECT_GE(program.max_phys_gpr, 16u);
   EXPECT_TRUE(agx_apple9_validate_vir_allocation(&program, &reason)) << reason;
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, ReportsNoSpillPressureLimit)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t values[17];
   for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
      values[i] =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                             AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, i);
   }
   program.output = values[ARRAY_SIZE(values) - 1];
   for (unsigned i = 0; i + 1 < ARRAY_SIZE(values); ++i)
      ASSERT_TRUE(agx_apple9_vir_add_live_out(&program, values[i]));

   const char *reason = nullptr;
   EXPECT_FALSE(agx_apple9_allocate_vir(&program, &reason));
   ASSERT_NE(reason, nullptr);
   EXPECT_STREQ(
      reason,
      "Apple9 no-spill allocator exhausted the compact destination bank");
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, UsesR16ThroughR63ForGeneralPressure)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t gid = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_GET_GLOBAL_ID,
                                      AGX_APPLE9_ENC_GET_SR, nullptr, 0, 0);
   uint32_t c =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                          AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, 1);
   uint32_t values[24];
   for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
      uint32_t sources[] = {gid, c};
      values[i] =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IADD,
                             AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources, 2, 0);
   }

   uint32_t reduced = values[0];
   for (unsigned i = 1; i < ARRAY_SIZE(values); ++i) {
      uint32_t sources[] = {reduced, values[i]};
      reduced =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IADD,
                             AGX_APPLE9_ENC_INT_ADD_EXTENDED, sources, 2, 0);
   }
   program.output = reduced;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason)) << reason;
   EXPECT_GE(program.peak_live_gprs, ARRAY_SIZE(values));
   EXPECT_GE(program.max_phys_gpr, 39u);
   EXPECT_LE(program.max_phys_gpr, 63u);
   EXPECT_TRUE(agx_apple9_validate_vir_allocation(&program, &reason)) << reason;
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, WideSelectDestinationDoesNotOverlapInputs)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);

   uint32_t sources[4];
   for (unsigned i = 0; i < 4; ++i) {
      sources[i] =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                             AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, i + 1);
   }

   uint32_t value = agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_SELECT,
                                        AGX_APPLE9_ENC_SELECT_GPR_WIDE, sources,
                                        4, AGX_APPLE9_SELECT_ULT);
   program.output = value;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason)) << reason;
   for (unsigned i = 0; i < 4; ++i)
      EXPECT_NE(program.phys[value], program.phys[sources[i]]);
   EXPECT_TRUE(agx_apple9_validate_vir_allocation(&program, &reason)) << reason;
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, PrecoloredFragmentInputsAndOutputsShareLiveness)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t red = agx_apple9_vir_input(&program, 0);
   uint32_t green = agx_apple9_vir_input(&program, 4);
   uint32_t add_sources[] = {red, green};
   uint32_t sum =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_FADD,
                          AGX_APPLE9_ENC_FLOAT2_COMPACT, add_sources, 2, 0);
   uint32_t zero =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                          AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, 0);
   uint32_t copy_sources[] = {sum, zero};
   uint32_t output =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, copy_sources, 2, 0);
   ASSERT_TRUE(agx_apple9_vir_set_fixed_phys(&program, output, 0));
   program.output = output;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason)) << reason;
   EXPECT_EQ(program.phys[red], 0u);
   EXPECT_EQ(program.phys[green], 4u);
   EXPECT_EQ(program.phys[output], 0u);
   EXPECT_NE(program.phys[sum], program.phys[red]);
   EXPECT_TRUE(agx_apple9_validate_vir_allocation(&program, &reason)) << reason;
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, NativeVectorLoadUsesOneAdjacentTupleAndOneSlot)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 4);
   const agx_apple9_device_load_contract contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR,
      .group_flags = AGX_APPLE9_DEVICE_LOAD_FIRST,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };
   uint32_t vector =
      agx_apple9_vir_emit_device_load_vector(&program, 0, index, 4, &contract);
   ASSERT_NE(vector, AGX_APPLE9_VREG_INVALID);
   uint32_t sources[] = {vector + 2, vector + 2};
   uint32_t output =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_UMIN,
                          AGX_APPLE9_ENC_MINMAX_COMPACT, sources, 2, 0);
   program.output = output;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << reason;
   EXPECT_EQ(program.instructions[0].producer_scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_6);
   EXPECT_EQ(program.instructions[1].scoreboard_slot,
             AGX_APPLE9_SCOREBOARD_SLOT_6);
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason)) << reason;
   EXPECT_GE(program.phys[vector], 16u);
   for (unsigned c = 1; c < 4; ++c)
      EXPECT_EQ(program.phys[vector + c], program.phys[vector] + c);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, PendingVectorTupleCannotOverlapLaterAsyncDestination)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 4);
   const agx_apple9_device_load_contract contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
      .group_flags = 0,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };

   uint32_t vector =
      agx_apple9_vir_emit_device_load_vector(&program, 0, index, 4, &contract);
   ASSERT_NE(vector, AGX_APPLE9_VREG_INVALID);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, vector, AGX_APPLE9_DEVICE_LOAD_HAS_NEXT,
      AGX_APPLE9_SCOREBOARD_SLOT_AUTO));

   const agx_apple9_device_load_contract final_contract = {
      .index_kind = AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR,
      .group_flags = 0,
      .raw_token = AGX_APPLE9_DEVICE_LOAD_TOKEN_5101,
   };
   uint32_t scalar =
      agx_apple9_vir_emit_device_load(&program, 1, index, &final_contract);
   ASSERT_NE(scalar, AGX_APPLE9_VREG_INVALID);
   ASSERT_TRUE(agx_apple9_vir_set_device_load_contract(
      &program, scalar, 0, AGX_APPLE9_SCOREBOARD_SLOT_AUTO));

   uint32_t vector_sources[] = {vector, vector + 2};
   uint32_t first =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IXOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, vector_sources, 2, 0);
   uint32_t scalar_sources[] = {scalar, first};
   uint32_t output =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IXOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, scalar_sources, 2, 0);
   program.output = output;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");

   const unsigned vector_base = program.phys[vector];
   const unsigned scalar_reg = program.phys[scalar];
   EXPECT_FALSE(scalar_reg >= vector_base && scalar_reg < vector_base + 4);
   for (unsigned c = 1; c < 4; ++c)
      EXPECT_EQ(program.phys[vector + c], vector_base + c);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, VectorStoreCollectsScalarSourcesBeforeAllocation)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t data[] = {
      agx_apple9_vir_input(&program, 2),
      agx_apple9_vir_input(&program, 4),
      agx_apple9_vir_input(&program, 6),
      agx_apple9_vir_input(&program, 8),
   };
   uint32_t index = agx_apple9_vir_input(&program, 10);
   ASSERT_TRUE(
      agx_apple9_vir_emit_device_store(&program, 0, index, data, 4, 32));
   ASSERT_EQ(program.instruction_count, 2u);
   const uint32_t tuple = program.instructions[0].dest;
   EXPECT_EQ(program.instructions[0].op, AGX_APPLE9_VIR_COLLECT);
   EXPECT_EQ(program.instructions[0].dest_components, 4u);
   EXPECT_EQ(program.instructions[1].op, AGX_APPLE9_VIR_DEVICE_STORE);
   for (unsigned c = 0; c < 4; ++c)
      EXPECT_EQ(program.instructions[1].src[c], tuple + c);

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   for (unsigned c = 1; c < 4; ++c)
      EXPECT_EQ(program.phys[tuple + c], program.phys[tuple] + c);
   for (unsigned c = 0; c < 4; ++c)
      EXPECT_NE(program.phys[tuple + c], program.phys[data[c]]);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, StoreIndexLifetimeUsesAccessDescriptorWithoutCopy)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 1);
   uint32_t first_data = agx_apple9_vir_input(&program, 2);
   ASSERT_TRUE(
      agx_apple9_vir_emit_device_store(&program, 0, index, &first_data, 1, 32));

   uint32_t sources[] = {index, index};
   uint32_t second_data =
      agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IOR,
                          AGX_APPLE9_ENC_LOGIC_EXTENDED, sources, 2, 0);
   ASSERT_TRUE(agx_apple9_vir_emit_device_store(&program, 1, index,
                                                &second_data, 1, 32));

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(program.instruction_count, 3u);
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");

   const auto &first_store = program.instructions[0];
   const auto &last_store = program.instructions[2];
   EXPECT_NE(first_store.live_after_mask & (1u << 1), 0u);
   EXPECT_EQ(last_store.live_after_mask & (1u << 1), 0u);
   EXPECT_EQ(program.phys[first_store.src[1]], program.phys[last_store.src[1]]);

   agx_apple9_packed_instruction packed = {};
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&first_store, program.phys,
                                               &packed, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(packed.bytes[6], 0x20);
   ASSERT_TRUE(agx_apple9_pack_vir_instruction(&last_store, program.phys,
                                               &packed, &reason))
      << (reason ? reason : "");
   EXPECT_EQ(packed.bytes[6], 0x21);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, CollectCoalescesAnAlreadyAdjacentKilledTuple)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t index = agx_apple9_vir_input(&program, 13);
   uint32_t data[4];
   for (unsigned c = 0; c < 4; ++c) {
      data[c] =
         agx_apple9_vir_emit(&program, AGX_APPLE9_VIR_IMM,
                             AGX_APPLE9_ENC_MOV_IMM_COMPACT, nullptr, 0, c + 1);
   }
   ASSERT_TRUE(
      agx_apple9_vir_emit_device_store(&program, 0, index, data, 4, 32));
   const uint32_t tuple = program.instructions[4].dest;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   for (unsigned c = 0; c < 4; ++c)
      EXPECT_EQ(program.phys[tuple + c], program.phys[data[c]]);
   agx_apple9_vir_finish(&program);
}

TEST(Apple9Allocator, CollectRespectsValuesLiveAcrossVectorStore)
{
   agx_apple9_vir_program program;
   agx_apple9_vir_init(&program);
   uint32_t live = agx_apple9_vir_input(&program, 16);
   uint32_t data[] = {
      agx_apple9_vir_input(&program, 2),
      agx_apple9_vir_input(&program, 4),
      agx_apple9_vir_input(&program, 6),
      agx_apple9_vir_input(&program, 8),
   };
   uint32_t index = agx_apple9_vir_input(&program, 10);
   ASSERT_TRUE(
      agx_apple9_vir_emit_device_store(&program, 0, index, data, 4, 32));
   ASSERT_TRUE(agx_apple9_vir_add_live_out(&program, live));
   const uint32_t tuple = program.instructions[0].dest;

   const char *reason = nullptr;
   ASSERT_TRUE(agx_apple9_assign_vir_scoreboard_slots(&program, &reason))
      << (reason ? reason : "");
   ASSERT_TRUE(agx_apple9_allocate_vir(&program, &reason))
      << (reason ? reason : "");
   for (unsigned c = 0; c < 4; ++c)
      EXPECT_NE(program.phys[tuple + c], program.phys[live]);
   agx_apple9_vir_finish(&program);
}

static nir_builder
apple9_compute_builder(const char *name)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                  &agx_nir_options, "%s", name);
   b.shader->info.workgroup_size[0] = 32;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   return b;
}

static nir_def *
apple9_global_id_x(nir_builder *b)
{
   return nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
}

static void
apple9_store_output(nir_builder *b, nir_def *index, nir_def *value)
{
   nir_store_ssbo(b, value, nir_imm_int(b, 0), nir_imul_imm(b, index, 4));
}

static nir_shader *
apple9_constant_store_shader(uint32_t value)
{
   nir_builder b = apple9_compute_builder("apple9_constant_store");
   nir_def *gid = apple9_global_id_x(&b);
   apple9_store_output(&b, gid, nir_imm_int(&b, value));
   return b.shader;
}

static nir_shader *
apple9_large_constant_add_shader()
{
   nir_builder b = apple9_compute_builder("apple9_large_constant_add");
   nir_def *gid = apple9_global_id_x(&b);
   apple9_store_output(&b, gid, nir_iadd_imm(&b, gid, 0x12345678));
   return b.shader;
}

static nir_shader *
apple9_ssbo_reduce_shader(unsigned input_count, bool floating,
                          enum gl_access_qualifier access = ACCESS_NON_WRITEABLE)
{
   nir_builder b = apple9_compute_builder("apple9_ssbo_reduce");
   b.shader->info.num_ssbos = input_count + 1;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *offset = nir_imul_imm(&b, gid, 4);
   nir_def *value = floating ? nir_imm_float(&b, 1.0f) : nir_imm_int(&b, 1);

   for (unsigned binding = 1; binding <= input_count; ++binding) {
      nir_def *loaded = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, binding),
                                      offset, .access = access);
      value =
         floating ? nir_fadd(&b, value, loaded) : nir_iadd(&b, value, loaded);
   }

   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), offset);
   return b.shader;
}

static nir_shader *
apple9_arbitrary_integer_shader()
{
   nir_builder b = apple9_compute_builder("apple9_arbitrary_integer");
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *a = nir_imul_imm(&b, gid, 3);
   nir_def *mixed = nir_ixor(&b, a, nir_iadd_imm(&b, gid, 7));
   apple9_store_output(&b, gid, nir_iadd_imm(&b, mixed, 11));
   return b.shader;
}

static nir_shader *
apple9_arbitrary_float_shader()
{
   nir_builder b = apple9_compute_builder("apple9_arbitrary_float");
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *a = nir_fmul(&b, gid, nir_imm_float(&b, 1.25f));
   nir_def *c = nir_fadd(&b, gid, nir_imm_float(&b, 7.5f));
   apple9_store_output(&b, gid, nir_ffma(&b, a, c, nir_imm_float(&b, -3.0f)));
   return b.shader;
}

enum apple9_reciprocal_shape {
   APPLE9_RECIPROCAL_DIRECT_STORE,
   APPLE9_RECIPROCAL_RETAIN_SOURCE,
   APPLE9_RECIPROCAL_MATERIALIZED_SOURCE,
   APPLE9_RECIPROCAL_RESULT_FANOUT,
};

static nir_shader *
apple9_reciprocal_shader(enum apple9_reciprocal_shape shape)
{
   nir_builder b = apple9_compute_builder("apple9_reciprocal");
   b.shader->info.num_ssbos =
      shape == APPLE9_RECIPROCAL_MATERIALIZED_SOURCE ? 3 : 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *offset = nir_imul_imm(&b, gid, 4);
   nir_def *x = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), offset,
                              .access = ACCESS_NON_WRITEABLE);
   nir_def *source = x;
   if (shape == APPLE9_RECIPROCAL_MATERIALIZED_SOURCE) {
      nir_def *y = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 2), offset,
                                 .access = ACCESS_NON_WRITEABLE);
      source = nir_fadd(&b, x, y);
   }

   nir_def *reciprocal = nir_frcp(&b, source);
   if (shape == APPLE9_RECIPROCAL_RESULT_FANOUT) {
      /* Make the first use a store and the later use an ALU operation.  The
       * native result hint describes the complete lifetime, not merely the
       * first consumer. */
      apple9_store_output(&b, gid, reciprocal);
      apple9_store_output(&b, nir_iadd_imm(&b, gid, 64),
                          nir_fadd(&b, reciprocal, source));
      return b.shader;
   }

   nir_def *result = shape == APPLE9_RECIPROCAL_RETAIN_SOURCE
                        ? nir_fadd(&b, reciprocal, source)
                        : reciprocal;
   apple9_store_output(&b, gid, result);
   return b.shader;
}

static nir_shader *
apple9_vector_load_shader()
{
   nir_builder b = apple9_compute_builder("apple9_vector_load");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *loaded =
      nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), nir_imul_imm(&b, gid, 16),
                    .access = ACCESS_NON_WRITEABLE, .align_mul = 16);
   nir_def *low =
      nir_ixor(&b, nir_channel(&b, loaded, 2), nir_channel(&b, loaded, 0));
   nir_def *high =
      nir_iand(&b, nir_channel(&b, loaded, 3), nir_channel(&b, loaded, 1));
   apple9_store_output(&b, gid, nir_iadd(&b, low, high));
   return b.shader;
}

static nir_shader *
apple9_vector_copy_shader(unsigned components)
{
   nir_builder b = apple9_compute_builder("apple9_vector_copy");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   const unsigned stride = components == 2 ? 8 : 16;
   nir_def *offset = nir_imul_imm(&b, gid, stride);
   nir_def *loaded =
      nir_load_ssbo(&b, components, 32, nir_imm_int(&b, 1), offset,
                    .access = ACCESS_NON_WRITEABLE, .align_mul = stride);
   nir_store_ssbo(&b, loaded, nir_imm_int(&b, 0), offset);
   return b.shader;
}

static nir_shader *
apple9_vector_alu_store_shader()
{
   nir_builder b = apple9_compute_builder("apple9_vector_alu_store");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *offset = nir_imul_imm(&b, gid, 16);
   nir_def *loaded =
      nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 1), offset,
                    .access = ACCESS_NON_WRITEABLE, .align_mul = 16);
   nir_def *lanes[4];
   for (unsigned c = 0; c < 4; ++c)
      lanes[c] = nir_iadd_imm(&b, nir_channel(&b, loaded, c), c + 1);
   nir_store_ssbo(&b, nir_vec(&b, lanes, 4), nir_imm_int(&b, 0), offset);
   return b.shader;
}

static nir_shader *
apple9_multiple_stores_one_binding_shader()
{
   nir_builder b = apple9_compute_builder("apple9_multiple_stores_one_binding");
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *base = nir_imul_imm(&b, gid, 8);
   nir_store_ssbo(&b, nir_iadd_imm(&b, gid, 11), nir_imm_int(&b, 0), base);
   nir_store_ssbo(&b, nir_ixor(&b, gid, nir_imm_int(&b, 0x55)),
                  nir_imm_int(&b, 0), nir_iadd_imm(&b, base, 4));
   return b.shader;
}

static nir_shader *
apple9_multiple_output_bindings_shader(bool alias_input)
{
   nir_builder b = apple9_compute_builder("apple9_multiple_output_bindings");
   b.shader->info.num_ssbos = alias_input ? 2 : 3;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *offset = nir_imul_imm(&b, gid, 4);
   const unsigned input_binding = alias_input ? 0 : 2;
   nir_def *loaded =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, input_binding), offset,
                    .access = alias_input ? (enum gl_access_qualifier)0
                                          : ACCESS_NON_WRITEABLE);
   nir_store_ssbo(&b, nir_iadd_imm(&b, loaded, 10), nir_imm_int(&b, 0), offset);
   nir_store_ssbo(&b, nir_ixor(&b, loaded, nir_imm_int(&b, 0x55)),
                  nir_imm_int(&b, 1), offset);
   return b.shader;
}

static nir_shader *
apple9_multiple_scalar_vector_stores_shader()
{
   nir_builder b =
      apple9_compute_builder("apple9_multiple_scalar_vector_stores");
   b.shader->info.num_ssbos = 3;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *loaded =
      nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 2), nir_imul_imm(&b, gid, 16),
                    .access = ACCESS_NON_WRITEABLE, .align_mul = 16);
   nir_def *lanes[4];
   for (unsigned c = 0; c < 4; ++c)
      lanes[c] = nir_iadd_imm(&b, nir_channel(&b, loaded, c), c + 1);
   nir_store_ssbo(&b, nir_vec(&b, lanes, 4), nir_imm_int(&b, 0),
                  nir_imul_imm(&b, gid, 16));
   nir_store_ssbo(
      &b, nir_ixor(&b, nir_channel(&b, loaded, 0), nir_channel(&b, loaded, 3)),
      nir_imm_int(&b, 1), nir_imul_imm(&b, gid, 4));
   return b.shader;
}

static nir_shader *
apple9_narrow_load_shader(unsigned bits, bool sign_extend)
{
   nir_builder b = apple9_compute_builder("apple9_narrow_load");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *offset = nir_imul_imm(&b, gid, bits / 8);
   nir_def *loaded =
      nir_load_ssbo(&b, 1, bits, nir_imm_int(&b, 1), offset,
                    .access = ACCESS_NON_WRITEABLE, .align_mul = bits / 8);
   nir_def *extended =
      sign_extend ? nir_i2i32(&b, loaded) : nir_u2u32(&b, loaded);
   apple9_store_output(&b, gid, extended);
   return b.shader;
}

static nir_shader *
apple9_narrow_store_shader(unsigned bits)
{
   nir_builder b = apple9_compute_builder("apple9_narrow_store");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *loaded =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imul_imm(&b, gid, 4),
                    .access = ACCESS_NON_WRITEABLE);
   nir_def *narrowed = bits == 8 ? nir_u2u8(&b, loaded) : nir_u2u16(&b, loaded);
   nir_store_ssbo(&b, narrowed, nir_imm_int(&b, 0),
                  nir_imul_imm(&b, gid, bits / 8));
   return b.shader;
}

static nir_shader *
apple9_ubo_load_shader()
{
   nir_builder b = apple9_compute_builder("apple9_ubo_load");
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *element = nir_iand_imm(&b, nir_iadd_imm(&b, gid, 7), 63);
   nir_def *loaded =
      nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 0), nir_imul_imm(&b, element, 4),
                   .align_mul = 4, .range = 64 * sizeof(uint32_t));
   apple9_store_output(&b, gid, nir_ixor(&b, loaded, nir_imm_int(&b, 0x5a)));
   return b.shader;
}

static nir_shader *
apple9_nested_dependent_load_shader()
{
   nir_builder b = apple9_compute_builder("apple9_nested_dependent_load");
   b.shader->info.num_ssbos = 4;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *linear_offset = nir_imul_imm(&b, gid, 4);
   nir_def *root = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), linear_offset,
                                 .access = ACCESS_NON_WRITEABLE);
   nir_def *j = nir_iand_imm(&b, nir_ixor(&b, root, gid), 63);
   nir_def *middle =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 2), nir_imul_imm(&b, j, 4),
                    .access = ACCESS_NON_WRITEABLE);
   nir_def *affine = nir_iadd_imm(&b, nir_imul_imm(&b, gid, 3), 1);
   nir_def *k = nir_iand_imm(&b, nir_ixor(&b, middle, affine), 63);
   nir_def *leaf =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 3), nir_imul_imm(&b, k, 4),
                    .access = ACCESS_NON_WRITEABLE);
   apple9_store_output(&b, gid, nir_ixor(&b, nir_ixor(&b, leaf, root), middle));
   return b.shader;
}

static nir_shader *
apple9_dynamic_scatter_shader()
{
   nir_builder b = apple9_compute_builder("apple9_dynamic_scatter");
   b.shader->info.num_ssbos = 3;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *linear_offset = nir_imul_imm(&b, gid, 4);
   nir_def *loaded_index =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), linear_offset,
                    .access = ACCESS_NON_WRITEABLE);
   nir_def *scatter_index = nir_iand_imm(&b, loaded_index, 7);
   nir_def *value = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 2),
                                  nir_imul_imm(&b, scatter_index, 4),
                                  .access = ACCESS_NON_WRITEABLE);
   nir_def *result = nir_iadd(&b, value, nir_imul_imm(&b, gid, 17));
   nir_store_ssbo(&b, result, nir_imm_int(&b, 0),
                  nir_imul_imm(&b, scatter_index, 4));
   return b.shader;
}

static nir_shader *
apple9_procedural_scatter_shader()
{
   nir_builder b = apple9_compute_builder("apple9_procedural_scatter");
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *scatter_index =
      nir_iand_imm(&b, nir_imul_imm(&b, gid, 3), 7);
   nir_store_ssbo(&b, nir_iadd_imm(&b, gid, 100), nir_imm_int(&b, 0),
                  nir_imul_imm(&b, scatter_index, 4));
   return b.shader;
}

static nir_shader *
apple9_unbounded_scatter_shader()
{
   nir_builder b = apple9_compute_builder("apple9_unbounded_scatter");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *loaded_index =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imul_imm(&b, gid, 4),
                    .access = ACCESS_NON_WRITEABLE);
   nir_store_ssbo(&b, nir_iadd_imm(&b, gid, 100), nir_imm_int(&b, 0),
                  nir_imul_imm(&b, loaded_index, 4));
   return b.shader;
}

static nir_shader *
apple9_variable_shift_shader(nir_op op)
{
   nir_builder b = apple9_compute_builder("apple9_variable_shift");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *value =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imul_imm(&b, gid, 4),
                    .access = ACCESS_NON_WRITEABLE);
   nir_def *shifted = nir_build_alu(&b, op, value, gid, nullptr, nullptr);
   apple9_store_output(&b, gid, shifted);
   return b.shader;
}

static nir_shader *
apple9_system_load_index_shader()
{
   nir_builder b = apple9_compute_builder("apple9_system_load_index");
   b.shader->info.num_ssbos = 2;
   nir_def *gid = apple9_global_id_x(&b);
   nir_def *local = nir_load_local_invocation_index(&b);
   nir_def *direct =
      nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), nir_imul_imm(&b, local, 4),
                    .access = ACCESS_NON_WRITEABLE);
   nir_def *derived_index = nir_iand_imm(&b, nir_iadd_imm(&b, local, 17), 63);
   nir_def *derived = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1),
                                    nir_imul_imm(&b, derived_index, 4),
                                    .access = ACCESS_NON_WRITEABLE);
   apple9_store_output(&b, gid, nir_ixor(&b, direct, derived));
   return b.shader;
}

enum apple9_test_system_value {
   APPLE9_TEST_LOCAL_ID,
   APPLE9_TEST_LOCAL_INDEX,
   APPLE9_TEST_WORKGROUP_ID,
   APPLE9_TEST_WORKGROUP_SIZE,
   APPLE9_TEST_SUBGROUP_INVOCATION,
   APPLE9_TEST_SUBGROUP_ID,
   APPLE9_TEST_SUBGROUP_SIZE,
};

static nir_shader *
apple9_system_value_shader(enum apple9_test_system_value system,
                           unsigned component)
{
   nir_builder b = apple9_compute_builder("apple9_system_value");
   nir_def *value = nullptr;
   switch (system) {
   case APPLE9_TEST_LOCAL_ID:
      value = nir_channel(&b, nir_load_local_invocation_id(&b), component);
      break;
   case APPLE9_TEST_LOCAL_INDEX:
      value = nir_load_local_invocation_index(&b);
      break;
   case APPLE9_TEST_WORKGROUP_ID:
      value = nir_channel(&b, nir_load_workgroup_id(&b), component);
      break;
   case APPLE9_TEST_WORKGROUP_SIZE:
      value = nir_channel(&b, nir_load_workgroup_size(&b), component);
      break;
   case APPLE9_TEST_SUBGROUP_INVOCATION:
      value = nir_load_subgroup_invocation(&b);
      break;
   case APPLE9_TEST_SUBGROUP_ID:
      value = nir_load_subgroup_id(&b);
      break;
   case APPLE9_TEST_SUBGROUP_SIZE:
      value = nir_load_subgroup_size(&b);
      break;
   }
   apple9_store_output(&b, apple9_global_id_x(&b), value);
   return b.shader;
}

static bool
apple9_binary_contains_get_sr_zext16(const struct agx_shader_part *compiled,
                                     uint8_t selector)
{
   const uint8_t *binary = (const uint8_t *)compiled->binary;
   for (size_t i = 0; i + 8 <= compiled->info.binary_size; ++i) {
      const uint8_t *bytes = binary + i;
      if ((bytes[0] & 0xf) == 0x4 && bytes[1] == selector && bytes[2] == 0x10 &&
          bytes[3] == 0x06 && bytes[4] == ((bytes[0] & 0xf0) | 0x03) &&
          bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 1)
         return true;
   }
   return false;
}

static bool
apple9_binary_contains_get_sr(const struct agx_shader_part *compiled,
                              uint8_t selector)
{
   const uint8_t *binary = (const uint8_t *)compiled->binary;
   for (size_t i = 0; i + 4 <= compiled->info.binary_size; ++i) {
      const uint8_t *bytes = binary + i;
      if ((bytes[0] & 0xf) == 0xc && bytes[1] == selector && bytes[2] == 0x10 &&
          (bytes[3] & 0x1f) == 0x06)
         return true;
   }
   return false;
}

static void
apple9_expect_compile(nir_shader *nir, enum agx_apple9_compute_abi expected_abi)
{
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(compiled.info.stage, MESA_SHADER_COMPUTE);
   EXPECT_GT(compiled.info.binary_size, 0u);
   EXPECT_EQ(profile.abi, expected_abi);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, ConstantStoreUsesGenericPipeline)
{
   apple9_expect_compile(apple9_constant_store_shader(42),
                         AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
}

TEST(Apple9Compiler, LargeConstantUsesOneRawLiteralAndAllocatedStore)
{
   nir_shader *nir = apple9_constant_store_shader(0x12345678);
   struct agx_shader_part compiled = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason))
      << (reason ? reason : "no diagnostic");

   const uint8_t *binary = (const uint8_t *)compiled.binary;
   unsigned literals = 0, stores = 0;
   unsigned literal_dst = UINT_MAX, store_data = UINT_MAX;
   for (unsigned i = 0; i + 8 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = binary + i;
      if ((bytes[0] & 0x0f) == 0x0c && bytes[1] == 0xf8 &&
          (bytes[2] & 0x1f) == 0x02 && bytes[3] == 0x12 && bytes[4] == 0x18 &&
          bytes[5] == 0x08 && bytes[6] == 0xa2 && bytes[7] == 0x01) {
         literals++;
         literal_dst = (bytes[0] >> 4) | ((bytes[2] & 0xc0) >> 2);
      }
   }
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = binary + i;
      if (bytes[0] == 0xe7 && bytes[1] == 0x00 && bytes[2] == 0x54) {
         stores++;
         store_data = bytes[3] >> 1;
      }
   }

   EXPECT_EQ(literals, 1u);
   EXPECT_EQ(stores, 1u);
   EXPECT_GE(literal_dst, 16u);
   EXPECT_LT(literal_dst, 64u);
   EXPECT_EQ(store_data, literal_dst);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, AluConstantUsesSixBitModeTwoLiteral)
{
   nir_shader *nir = apple9_large_constant_add_shader();
   struct agx_shader_part compiled = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason))
      << (reason ? reason : "no diagnostic");

   const uint8_t *binary = (const uint8_t *)compiled.binary;
   unsigned literals = 0;
   for (unsigned i = 0; i + 8 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = binary + i;
      if ((bytes[0] & 0x0f) == 0x0c && bytes[1] == 0xf8 &&
          (bytes[2] & 0x1f) == 0x02 && bytes[3] == 0x12 && bytes[4] == 0x18 &&
          bytes[5] == 0x08 && bytes[6] == 0xa2 && bytes[7] == 0x01) {
         literals++;
         const unsigned dst = (bytes[0] >> 4) | ((bytes[2] & 0xc0) >> 2);
         EXPECT_GE(dst, 16u);
         EXPECT_LT(dst, 64u);
      }
   }

   EXPECT_EQ(literals, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, IntegerAndFloatDoNotDependOnInputCount)
{
   static const enum agx_apple9_compute_abi abi[] = {
      AGX_APPLE9_COMPUTE_ABI_INVALID,
      AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET,
      AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET,
      AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET,
   };

   for (unsigned inputs = 1; inputs <= 3; ++inputs) {
      for (bool floating : {false, true}) {
         SCOPED_TRACE(testing::Message()
                      << "inputs=" << inputs << " floating=" << floating);
         apple9_expect_compile(apple9_ssbo_reduce_shader(inputs, floating),
                               abi[inputs]);
      }
   }
}

TEST(Apple9Compiler, RejectsVolatileAndCoherentAccess)
{
   for (enum gl_access_qualifier access : {ACCESS_VOLATILE, ACCESS_COHERENT}) {
      nir_shader *nir = apple9_ssbo_reduce_shader(2, false, access);
      struct agx_shader_part compiled = {};
      const char *reason = nullptr;
      EXPECT_FALSE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason));
      EXPECT_NE(reason, nullptr);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, GeneralIntegerAndFloatDagsCompile)
{
   apple9_expect_compile(apple9_arbitrary_integer_shader(),
                         AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   apple9_expect_compile(apple9_arbitrary_float_shader(),
                         AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
}

TEST(Apple9Compiler, ReciprocalUsesNativeHandoffAndLifetimeForms)
{
   static const struct {
      enum apple9_reciprocal_shape shape;
      uint8_t handoff;
      uint8_t result_hint;
      uint8_t source_lifetime;
   } cases[] = {
      {APPLE9_RECIPROCAL_DIRECT_STORE, 0x56, 0x02, 0x10},
      {APPLE9_RECIPROCAL_RETAIN_SOURCE, 0x56, 0x03, 0x00},
      {APPLE9_RECIPROCAL_MATERIALIZED_SOURCE, 0x54, 0x02, 0x10},
      {APPLE9_RECIPROCAL_RESULT_FANOUT, 0x56, 0x03, 0x00},
   };

   for (const auto &test : cases) {
      SCOPED_TRACE(testing::Message() << "shape=" << test.shape);
      nir_shader *nir = apple9_reciprocal_shader(test.shape);
      struct agx_shader_part compiled = {};
      struct agx_apple9_compute_profile profile = {};
      const char *reason = nullptr;
      ASSERT_TRUE(
         agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
         << (reason ? reason : "no diagnostic");

      const uint8_t *binary = (const uint8_t *)compiled.binary;
      unsigned reciprocals = 0;
      for (unsigned i = 0; i + 10 <= compiled.info.binary_size; ++i) {
         const uint8_t *bytes = binary + i;
         if (bytes[0] != 0xaf || bytes[1] != 0x00 || bytes[7] != 0x48 ||
             bytes[8] != 0x20 || bytes[9] != 0x00)
            continue;
         ++reciprocals;
         EXPECT_EQ(bytes[2], test.handoff);
         EXPECT_EQ(bytes[4], test.result_hint);
         EXPECT_EQ(bytes[6], test.source_lifetime);
         EXPECT_LT(bytes[3] >> 1, AGX_APPLE9_GPR_COUNT);
         EXPECT_LT(bytes[5] >> 2, 64u);
      }
      EXPECT_EQ(reciprocals, 1u);
      EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
      free(compiled.binary);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, VectorLoadsUseOneNativeScoreboardTuple)
{
   nir_shader *nir = apple9_vector_load_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   EXPECT_GT(compiled.info.binary_size, 0u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, NativeVectorLoadsAndStoresCoverTwoThreeAndFourLanes)
{
   for (unsigned components : {2u, 3u, 4u}) {
      SCOPED_TRACE(components);
      nir_shader *nir = apple9_vector_copy_shader(components);
      struct agx_shader_part compiled = {};
      struct agx_apple9_compute_profile profile = {};
      const char *reason = nullptr;
      ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
         << (reason ? reason : "no diagnostic");
      EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);

      unsigned vector_loads = 0, vector_stores = 0;
      unsigned load_data = UINT_MAX, store_data = UINT_MAX;
      const uint8_t load_token = components == 2   ? 0x59
                                 : components == 3 ? 0x5d
                                                   : 0x57;
      const uint8_t store_token = components == 2   ? 0x19
                                  : components == 3 ? 0x1d
                                                    : 0x17;
      for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
         const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
         if (bytes[0] == 0x67 && bytes[8] == load_token) {
            vector_loads++;
            load_data = bytes[3];
         }
         if (bytes[0] == 0xe7 && bytes[8] == store_token) {
            vector_stores++;
            store_data = bytes[3];
            EXPECT_EQ(bytes[2], 0x56);
         }
      }
      EXPECT_EQ(vector_loads, 1u);
      EXPECT_EQ(vector_stores, 1u);
      EXPECT_EQ(store_data, load_data);
      free(compiled.binary);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, ComputedVectorStoreUsesPreRaTupleCollection)
{
   nir_shader *nir = apple9_vector_alu_store_shader();
   struct agx_shader_part compiled = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason))
      << (reason ? reason : "no diagnostic");

   unsigned vector_stores = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      if (bytes[0] == 0xe7 && bytes[8] == 0x17) {
         vector_stores++;
         EXPECT_EQ(bytes[2], 0x54);
         EXPECT_EQ(bytes[3] & 1, 0u);
         EXPECT_LE((bytes[3] >> 1) + 4, 64u);
      }
   }
   EXPECT_EQ(vector_stores, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, MultipleStoresShareOneWritableResource)
{
   nir_shader *nir = apple9_multiple_stores_one_binding_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");

   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   EXPECT_EQ(profile.resource_binding_count, 1u);
   EXPECT_EQ(profile.resource_binding[0], 0u);
   EXPECT_EQ(profile.resource_read_mask, 0u);
   EXPECT_EQ(profile.resource_write_mask, 1u);

   unsigned stores = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      if (bytes[0] == 0xe7 && bytes[2] == 0x54) {
         stores++;
         EXPECT_EQ(bytes[4], AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE);
      }
   }
   EXPECT_EQ(stores, 2u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, MultipleWritableBindingsUseSemanticResourceMasks)
{
   for (bool alias_input : {false, true}) {
      SCOPED_TRACE(alias_input ? "read/write alias" : "separate input");
      nir_shader *nir = apple9_multiple_output_bindings_shader(alias_input);
      struct agx_shader_part compiled = {};
      struct agx_apple9_compute_profile profile = {};
      const char *reason = nullptr;
      ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
         << (reason ? reason : "no diagnostic");

      if (alias_input) {
         EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
         ASSERT_EQ(profile.resource_binding_count, 2u);
         EXPECT_EQ(profile.resource_binding[0], 1u);
         EXPECT_EQ(profile.resource_binding[1], 0u);
         EXPECT_EQ(profile.resource_read_mask, 0x2u);
         EXPECT_EQ(profile.resource_write_mask, 0x3u);
      } else {
         EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
         ASSERT_EQ(profile.resource_binding_count, 3u);
         EXPECT_EQ(profile.resource_binding[0], 2u);
         EXPECT_EQ(profile.resource_binding[1], 1u);
         EXPECT_EQ(profile.resource_binding[2], 0u);
         EXPECT_EQ(profile.resource_read_mask, 0x1u);
         EXPECT_EQ(profile.resource_write_mask, 0x6u);
      }

      unsigned stores = 0;
      uint8_t store_arguments[2] = {};
      uint8_t access_desc[2] = {};
      for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
         const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
         if (bytes[0] == 0xe7 && bytes[2] == 0x54 && stores < 2) {
            store_arguments[stores++] = bytes[4];
            access_desc[stores - 1] = bytes[6];
         }
      }
      EXPECT_EQ(stores, 2u);
      EXPECT_EQ(store_arguments[0],
                AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE +
                   (alias_input ? 1u : 2u));
      EXPECT_EQ(store_arguments[1],
                AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE +
                   (alias_input ? 0u : 1u));
      EXPECT_EQ(access_desc[0], 0x20);
      EXPECT_EQ(access_desc[1], 0x21);
      free(compiled.binary);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, MultipleScalarAndVectorStoresUseAllocatedSources)
{
   nir_shader *nir = apple9_multiple_scalar_vector_stores_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   EXPECT_EQ(profile.resource_read_mask, 0x1u);
   EXPECT_EQ(profile.resource_write_mask, 0x6u);

   unsigned vector_stores = 0, scalar_stores = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      if (bytes[0] != 0xe7 || bytes[2] != 0x54)
         continue;
      if (bytes[8] == 0x17) {
         vector_stores++;
         EXPECT_EQ(bytes[4],
                   AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE + 2u);
      } else if (bytes[8] == 0x11) {
         scalar_stores++;
         EXPECT_EQ(bytes[4],
                   AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE + 1u);
      }
   }
   EXPECT_EQ(vector_stores, 1u);
   EXPECT_EQ(scalar_stores, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, MaterializedScalarStoreUsesAllocatedSource)
{
   nir_shader *nir = apple9_ssbo_reduce_shader(2, false);
   struct agx_shader_part compiled = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason))
      << (reason ? reason : "no diagnostic");

   unsigned stores = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      if (bytes[0] == 0xe7 && bytes[2] == 0x54) {
         stores++;
         EXPECT_EQ(bytes[3] & 1, 0u);
         EXPECT_LT(bytes[3] >> 1, 64u);
      }
   }
   EXPECT_EQ(stores, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, NativeNarrowLoadsExtendAndStoresTruncate)
{
   for (unsigned bits : {8u, 16u}) {
      for (bool sign_extend : {false, true}) {
         SCOPED_TRACE(testing::Message()
                      << "load bits=" << bits << " signed=" << sign_extend);
         nir_shader *nir = apple9_narrow_load_shader(bits, sign_extend);
         struct agx_shader_part compiled = {};
         struct agx_apple9_compute_profile profile = {};
         const char *reason = nullptr;
         ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
            << (reason ? reason : "no diagnostic");
         EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);

         unsigned narrow_loads = 0;
         const uint8_t format = bits == 8 ? 0x21 : 0x01;
         const uint8_t tail = bits == 8 ? 0x42 : 0x44;
         for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
            const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
            narrow_loads += bytes[0] == 0x67 && (bytes[8] & 0x3f) == format &&
                            bytes[12] == tail;
         }
         EXPECT_EQ(narrow_loads, 1u);
         free(compiled.binary);
         ralloc_free(nir);
      }

      SCOPED_TRACE(testing::Message() << "store bits=" << bits);
      nir_shader *nir = apple9_narrow_store_shader(bits);
      struct agx_shader_part compiled = {};
      struct agx_apple9_compute_profile profile = {};
      const char *reason = nullptr;
      ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
         << (reason ? reason : "no diagnostic");
      EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);

      unsigned narrow_stores = 0;
      const uint8_t format = bits == 8 ? 0x21 : 0x01;
      const uint8_t tail = bits == 8 ? 0x10 : 0x11;
      for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
         const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
         narrow_stores +=
            bytes[0] == 0xe7 && bytes[8] == format && bytes[12] == tail;
      }
      EXPECT_EQ(narrow_stores, 1u);
      free(compiled.binary);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, UboLoadsUseTypedNativeResourceArguments)
{
   nir_shader *nir = apple9_ubo_load_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   ASSERT_EQ(profile.resource_binding_count, 2u);
   EXPECT_EQ(profile.resource_kind[0], AGX_APPLE9_COMPUTE_RESOURCE_UBO);
   EXPECT_EQ(profile.resource_binding[0], 0u);
   EXPECT_EQ(profile.resource_kind[1], AGX_APPLE9_COMPUTE_RESOURCE_SSBO);
   EXPECT_EQ(profile.resource_binding[1], 0u);
   EXPECT_GT(compiled.info.binary_size, 0u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, NestedDependentLoadsRetainEarlierResults)
{
   nir_shader *nir = apple9_nested_dependent_load_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   EXPECT_GT(compiled.info.binary_size, 0u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, DynamicLoadIndexDrivesScatter)
{
   nir_shader *nir = apple9_dynamic_scatter_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");

   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   ASSERT_EQ(profile.resource_binding_count, 3u);
   EXPECT_EQ(profile.resource_binding[0], 2u);
   EXPECT_EQ(profile.resource_binding[1], 1u);
   EXPECT_EQ(profile.resource_binding[2], 0u);

   unsigned stores = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      if (bytes[0] == 0xe7 && bytes[2] == 0x54 &&
          bytes[4] == AGX_APPLE9_COMPUTE_VISIBLE_ARGUMENT_BASE + 2) {
         stores++;
         EXPECT_EQ(bytes[6], 0x21);
      }
   }
   EXPECT_EQ(stores, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, ProceduralDynamicScatterCompiles)
{
   nir_shader *nir = apple9_procedural_scatter_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");

   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   ASSERT_EQ(profile.resource_binding_count, 1u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, ArbitraryLoadedIndexNeedsNoRangeProof)
{
   nir_shader *nir = apple9_unbounded_scatter_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
   EXPECT_EQ(profile.resource_binding_count, 2u);
   EXPECT_EQ(profile.resource_read_mask, 0x1u);
   EXPECT_EQ(profile.resource_write_mask, 0x2u);
   free(compiled.binary);
   ralloc_free(nir);
}

TEST(Apple9Compiler, VariableShiftsUseGeneralValidatedLowering)
{
   for (nir_op op : {nir_op_ishl, nir_op_ishr, nir_op_ushr}) {
      nir_shader *nir = apple9_variable_shift_shader(op);
      struct agx_shader_part compiled = {};
      struct agx_apple9_compute_profile profile = {};
      const char *reason = nullptr;
      ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
         << (reason ? reason : "no diagnostic");
      EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
      EXPECT_GT(compiled.info.binary_size, 100u);
      free(compiled.binary);
      ralloc_free(nir);
   }
}

TEST(Apple9Compiler, GenericComputeSystemRegisterTable)
{
   struct selector_case {
      enum apple9_test_system_value system;
      unsigned component;
      uint8_t selector;
      bool zext16;
   };
   static const selector_case cases[] = {
      {APPLE9_TEST_LOCAL_ID, 0, 0xa4, true},
      {APPLE9_TEST_LOCAL_ID, 1, 0xa5, true},
      {APPLE9_TEST_LOCAL_ID, 2, 0xa6, true},
      {APPLE9_TEST_LOCAL_INDEX, 0, 0xa7, true},
      {APPLE9_TEST_WORKGROUP_ID, 0, 0x9c, false},
      {APPLE9_TEST_WORKGROUP_ID, 1, 0x9d, false},
      {APPLE9_TEST_WORKGROUP_ID, 2, 0x9e, false},
      {APPLE9_TEST_WORKGROUP_SIZE, 0, 0x98, true},
      {APPLE9_TEST_WORKGROUP_SIZE, 1, 0x99, true},
      {APPLE9_TEST_WORKGROUP_SIZE, 2, 0x9a, true},
      {APPLE9_TEST_SUBGROUP_INVOCATION, 0, 0x82, true},
      {APPLE9_TEST_SUBGROUP_ID, 0, 0x85, true},
   };

   for (const auto &test : cases) {
      SCOPED_TRACE(testing::Message()
                   << "selector=" << (unsigned)test.selector);
      nir_shader *nir = apple9_system_value_shader(test.system, test.component);
      struct agx_shader_part compiled = {};
      const char *reason = nullptr;
      ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, nullptr, &reason))
         << (reason ? reason : "no diagnostic");

      if (test.zext16) {
         EXPECT_TRUE(
            apple9_binary_contains_get_sr_zext16(&compiled, test.selector));
      } else {
         EXPECT_TRUE(apple9_binary_contains_get_sr(&compiled, test.selector));
      }
      free(compiled.binary);
      ralloc_free(nir);
   }

   apple9_expect_compile(
      apple9_system_value_shader(APPLE9_TEST_SUBGROUP_SIZE, 0),
      AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);
}

TEST(Apple9Compiler, SystemRegisterAndDerivedLoadIndicesShareTheSsaPath)
{
   nir_shader *nir = apple9_system_load_index_shader();
   struct agx_shader_part compiled = {};
   struct agx_apple9_compute_profile profile = {};
   const char *reason = nullptr;
   ASSERT_TRUE(agx_compile_apple9_tiny(nir, &compiled, &profile, &reason))
      << (reason ? reason : "no diagnostic");
   EXPECT_EQ(profile.abi, AGX_APPLE9_COMPUTE_ABI_SSBO8_SUPERSET);

   bool saw_local_index = false;
   unsigned scalar_loads = 0;
   for (unsigned i = 0; i + 14 <= compiled.info.binary_size; ++i) {
      const uint8_t *bytes = (const uint8_t *)compiled.binary + i;
      saw_local_index |= (bytes[0] & 0xf) == 0x4 && bytes[1] == 0xa7 &&
                         bytes[2] == 0x10 && bytes[3] == 0x06 &&
                         bytes[4] == ((bytes[0] & 0xf0) | 0x03);
      scalar_loads += bytes[0] == 0x67 && (bytes[8] & 0xf8) == 0x50;
   }
   EXPECT_TRUE(saw_local_index);
   EXPECT_EQ(scalar_loads, 2u);

   free(compiled.binary);
   ralloc_free(nir);
}
