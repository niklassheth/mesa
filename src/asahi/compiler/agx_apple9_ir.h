/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AGX_APPLE9_IR_H
#define AGX_APPLE9_IR_H

#include <stdbool.h>
#include <stdint.h>

#include "agx_apple9_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_APPLE9_VREG_INVALID          UINT32_MAX
#define AGX_APPLE9_PHYS_INVALID          UINT8_MAX
#define AGX_APPLE9_MAX_VIR_SRCS          5
/*
 * A deliberately small semantic IR for the first real Apple9 compiler.
 *
 * Values are scalar 32-bit SSA virtual registers.  Consecutive values may be
 * grouped into an adjacent physical-register tuple by an explicit COLLECT
 * pseudo.  Encoding selection is explicit before allocation, so physical-
 * register constraints come from the Apple9 machine table instead of being
 * implicit in byte templates.
 */
enum agx_apple9_vir_opcode {
   AGX_APPLE9_VIR_IMM,
   AGX_APPLE9_VIR_GET_GLOBAL_ID,
   AGX_APPLE9_VIR_GET_SR,
   AGX_APPLE9_VIR_DEVICE_LOAD,
   AGX_APPLE9_VIR_U2F32,
   AGX_APPLE9_VIR_I2F32,
   AGX_APPLE9_VIR_F2I32,
   AGX_APPLE9_VIR_F2U32,
   AGX_APPLE9_VIR_IADD,
   AGX_APPLE9_VIR_IMUL,
   AGX_APPLE9_VIR_ISUB,
   AGX_APPLE9_VIR_IMAD,
   AGX_APPLE9_VIR_IAND,
   AGX_APPLE9_VIR_IOR,
   AGX_APPLE9_VIR_IXOR,
   AGX_APPLE9_VIR_ISHR,
   AGX_APPLE9_VIR_IMIN,
   AGX_APPLE9_VIR_IMAX,
   AGX_APPLE9_VIR_UMIN,
   AGX_APPLE9_VIR_UMAX,
   AGX_APPLE9_VIR_FADD,
   AGX_APPLE9_VIR_FSUB,
   AGX_APPLE9_VIR_FMUL,
   AGX_APPLE9_VIR_FADD_IMM,
   AGX_APPLE9_VIR_FMUL_IMM,
   AGX_APPLE9_VIR_FMIN,
   AGX_APPLE9_VIR_FMAX,
   AGX_APPLE9_VIR_FMA,
   AGX_APPLE9_VIR_SELECT,
   AGX_APPLE9_VIR_COLLECT,
   AGX_APPLE9_VIR_DEVICE_STORE,
};

enum agx_apple9_device_store_form {
   AGX_APPLE9_DEVICE_STORE_IMPLICIT_ALU,

   /* The native direct-copy corpus always feeds the 0x56 store from a
    * slot-6 device load.  Hardware experiments suggest a broader mechanism,
    * but keep that synthetic-only behavior out of the initial compiler: a
    * caller may select this form only for a slot-6 producer.  The store still
    * names the value through its half-register/GPR field; it does not encode a
    * separate slot selector. */
   AGX_APPLE9_DEVICE_STORE_IMPLICIT_DEVICE_LOAD_SLOT6,
};

enum agx_apple9_select_condition {
   AGX_APPLE9_SELECT_FEQ = 0x00,
   AGX_APPLE9_SELECT_FGT = 0x02,
   AGX_APPLE9_SELECT_FLT = 0x03,
   AGX_APPLE9_SELECT_UGT = 0x04,
   AGX_APPLE9_SELECT_ULT = 0x05,
   AGX_APPLE9_SELECT_IGT = 0x06,
   AGX_APPLE9_SELECT_ILT = 0x07,
};

#define AGX_APPLE9_SELECT_EQUALITY (1u << 8)

/*
 * Apple9 asynchronous producers publish through a six-entry scoreboard.
 * Slot 0 is the ordinary/materialized GPR path; slots 1--6 are transient
 * pending-result handoffs.  AUTO is compiler IR only and must be resolved
 * after final instruction scheduling, before packing.
 */
enum agx_apple9_scoreboard_slot {
   AGX_APPLE9_SCOREBOARD_SLOT_NONE = 0,
   AGX_APPLE9_SCOREBOARD_SLOT_1 = 1,
   AGX_APPLE9_SCOREBOARD_SLOT_2 = 2,
   AGX_APPLE9_SCOREBOARD_SLOT_3 = 3,
   AGX_APPLE9_SCOREBOARD_SLOT_4 = 4,
   AGX_APPLE9_SCOREBOARD_SLOT_5 = 5,
   AGX_APPLE9_SCOREBOARD_SLOT_6 = 6,
   AGX_APPLE9_SCOREBOARD_SLOT_AUTO = 0xff,
};

enum agx_apple9_device_load_group_flags {
   AGX_APPLE9_DEVICE_LOAD_FIRST = 1u << 0,
   AGX_APPLE9_DEVICE_LOAD_HAS_NEXT = 1u << 1,
};

enum agx_apple9_device_load_index_kind {
   /* Byte 5's low seven bits name a real architectural GPR.  Bit 7 is clear
    * when that SSA value has a later consumer and set when this load is its
    * final consumer.  VIR packing derives the bit from allocator liveness;
    * the raw packer accepts the explicit form for byte-level tests. */
   AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
   AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR,

   /* Compatibility spellings retained while the existing capture tiers are
    * migrated.  They describe lifetime, not how the index was computed. */
   AGX_APPLE9_DEVICE_LOAD_INDEX_DIRECT_GPR =
      AGX_APPLE9_DEVICE_LOAD_INDEX_RETAINED_GPR,
   AGX_APPLE9_DEVICE_LOAD_INDEX_COMPUTED_GPR =
      AGX_APPLE9_DEVICE_LOAD_INDEX_LAST_USE_GPR,
};

/* Human-order names for the two raw bytes at instruction offsets 8 and 9.
 * For example, 0x5101 is serialized as 51:01, independent of host endian. */
enum agx_apple9_device_load_raw_token {
   AGX_APPLE9_DEVICE_LOAD_TOKEN_1100 = 0x1100,
   AGX_APPLE9_DEVICE_LOAD_TOKEN_5100 = 0x5100,
   AGX_APPLE9_DEVICE_LOAD_TOKEN_9100 = 0x9100,
   AGX_APPLE9_DEVICE_LOAD_TOKEN_D100 = 0xd100,
   AGX_APPLE9_DEVICE_LOAD_TOKEN_1101 = 0x1101,
   AGX_APPLE9_DEVICE_LOAD_TOKEN_5101 = 0x5101,
};

struct agx_apple9_device_load_contract {
   enum agx_apple9_device_load_index_kind index_kind;
   uint8_t group_flags;
   uint16_t raw_token;

   /* Capture-scoped byte-2 bit 1.  Native dependent-load probes correlate it
    * with the first DEVICE_LOAD index consumer of a DEVICE_LOAD result.  It
    * is independent of scoreboard allocation and group framing. */
   bool index_first_load_consumer;
};

struct agx_apple9_vir_instr {
   enum agx_apple9_vir_opcode op;
   enum agx_apple9_encoding encoding;
   uint32_t dest;
   /* Number of adjacent SSA values and physical GPRs defined at dest.
    * Scalar instructions define one value. Native vector memory loads and
    * COLLECT define one two-, three-, or four-register tuple. A native load
    * uses one scoreboard allocation for the complete tuple. */
   uint8_t dest_components;
   /* Scalar device-memory element width.  Zero is the established u32
    * default; compiler-authored narrow operations use 8 or 16 explicitly. */
   uint8_t memory_bits;
   /* Number of scalar lanes read or written by a memory instruction.  Loads
    * also define an adjacent dest_components tuple; stores have no SSA
    * destination and carry their ordered data values in src[0..N-1]. */
   uint8_t memory_components;
   uint32_t src[AGX_APPLE9_MAX_VIR_SRCS];
   uint32_t immediate;
   uint8_t nr_srcs;

   /* Scoreboard slot published by an asynchronous producer.  Ordinary ALU
    * instructions leave this at NONE.  AUTO is resolved by the scheduled
    * scoreboard pass and is never serialized. */
   uint8_t producer_scoreboard_slot;

   /* Device-load framing fields, independent of the load/cache token.  The
    * FIRST/HAS_NEXT enum names are labels for measured low-index bits. */
   uint8_t device_load_group_flags;
   enum agx_apple9_device_load_index_kind device_load_index_kind;
   bool device_load_index_first_consumer;

   /* Raw scalar-load instruction bytes 8:9.  These encode the producer's
    * scoreboard slot. */
   uint16_t device_load_raw_token;

   /* Bit s is set when src[s] has another consumer after this instruction. */
   uint8_t live_after_mask;

   /*
    * Scoreboard slot consumed by this instruction.  Slot 0 reads only the
    * ordinary GPR path.  The machine encoding is instruction-specific: FALU
    * uses a binary selector while integer logic uses a six-bit one-hot mask.
    * This is independent of per-source release bits.
    */
   uint8_t scoreboard_slot;

   /* Conservative identity bridge inserted when an asynchronous load's first
    * user has no proven slot-bearing form.  It consumes the assigned slot
    * immediately and leaves an ordinary GPR value for the original users. */
   bool scoreboard_materialize;

   /* Selected only after the scoreboard pass has proven whether an exact
    * load-to-store handoff owns native slot 6. */
   enum agx_apple9_device_store_form device_store_form;

   /*
    * Fragment iterator/perspective producers are addressed by compact ALU
    * through operand tokens, not by their architectural GPR numbers.  A value
    * of 0xff means that this source uses the ordinary GPR encoding.  These
    * per-source tokens are distinct from the instruction-wide scoreboard
    * slot and are kept on the use until the graphics compiler is generalized.
    */
};

struct agx_apple9_vir_program {
   struct agx_apple9_vir_instr *instructions;
   unsigned instruction_count;
   unsigned instruction_capacity;
   unsigned value_count;
   uint32_t output;

   /*
    * Values may enter or leave the bounded program in fixed physical GPRs.
    * This is used by fragment interpolation/color packing, whose surrounding
    * stage instructions have an independently validated register contract.
   */
   uint8_t *fixed_phys;
   /* Optional per-value upper bound used to propagate a constrained
    * consumer's compact-register requirement back to its producer. */
   uint8_t *max_phys;
   uint32_t *live_out;
   unsigned live_out_count;
   unsigned live_out_capacity;
   bool reserved_gprs[AGX_APPLE9_GPR_COUNT];

   /* Filled by agx_apple9_allocate_vir(). */
   uint8_t *phys;
   unsigned peak_live_gprs;
   unsigned max_phys_gpr;
};

void agx_apple9_vir_init(struct agx_apple9_vir_program *program);
void agx_apple9_vir_finish(struct agx_apple9_vir_program *program);

uint32_t agx_apple9_vir_emit(struct agx_apple9_vir_program *program,
                             enum agx_apple9_vir_opcode op,
                             enum agx_apple9_encoding encoding,
                             const uint32_t *src, unsigned nr_srcs,
                             uint32_t immediate);

uint32_t agx_apple9_vir_input(struct agx_apple9_vir_program *program,
                              unsigned phys);
uint32_t agx_apple9_vir_emit_device_load(
   struct agx_apple9_vir_program *program, unsigned binding, uint32_t index,
   const struct agx_apple9_device_load_contract *contract);
uint32_t agx_apple9_vir_emit_device_load_vector(
   struct agx_apple9_vir_program *program, unsigned binding, uint32_t index,
   unsigned components, const struct agx_apple9_device_load_contract *contract);
/* Form an adjacent register tuple from independent scalar SSA values before
 * register allocation. The pseudo is coalesced when possible and otherwise
 * lowered to copies after allocation, following the Apple8 AGX IR model. */
uint32_t agx_apple9_vir_emit_collect(struct agx_apple9_vir_program *program,
                                    const uint32_t *src,
                                    unsigned components);
bool agx_apple9_vir_emit_device_store(
   struct agx_apple9_vir_program *program, unsigned binding, uint32_t index,
   const uint32_t *data, unsigned components, unsigned bits);
bool agx_apple9_vir_set_device_load_contract(
   struct agx_apple9_vir_program *program, uint32_t value, uint8_t group_flags,
   enum agx_apple9_scoreboard_slot scoreboard_slot);
bool agx_apple9_vir_set_device_load_raw_contract(
   struct agx_apple9_vir_program *program, uint32_t value, uint8_t group_flags,
   uint16_t raw_token);
bool agx_apple9_vir_set_device_load_index_kind(
   struct agx_apple9_vir_program *program, uint32_t value,
   enum agx_apple9_device_load_index_kind index_kind);
bool agx_apple9_vir_set_fixed_phys(struct agx_apple9_vir_program *program,
                                   uint32_t value, unsigned phys);
bool agx_apple9_vir_add_live_out(struct agx_apple9_vir_program *program,
                                 uint32_t value);
/*
 * Scalar/adjacent-tuple linear-scan allocator. General encodings prefer
 * r16-r63 so the r0-r15 compact-result bank stays available to hard-low
 * instructions, then fall back to that low bank. Destinations remain distinct
 * from their inputs; killed sources become available to following
 * instructions. Spilling waits on the Dynamic-Caching scratch ABI.
 */
bool agx_apple9_allocate_vir(struct agx_apple9_vir_program *program,
                             const char **reason);

bool
agx_apple9_validate_vir_allocation(const struct agx_apple9_vir_program *program,
                                   const char **reason);

/* Allocate pending asynchronous producers to scoreboard slots from final VIR
 * order.  The first capable consumer performs the handoff; later reads use
 * slot 0. */
bool
agx_apple9_assign_vir_scoreboard_slots(struct agx_apple9_vir_program *program,
                                       const char **reason);

/* One physical Apple9 instruction, used by the compiler and packer tests. */
struct agx_apple9_packed_instruction {
   uint8_t bytes[16];
   uint8_t length;
};

bool agx_apple9_pack_vir_instruction(
   const struct agx_apple9_vir_instr *instruction, const uint8_t *phys,
   struct agx_apple9_packed_instruction *packed, const char **reason);

bool
agx_apple9_pack_get_global_id(unsigned dst, unsigned component,
                              struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_get_sr(unsigned dst, uint8_t selector, uint8_t datapath,
                            struct agx_apple9_packed_instruction *packed);
bool
agx_apple9_pack_get_sr_zext16(unsigned dst, uint8_t selector,
                              struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_mov_imm(unsigned dst, unsigned value,
                             struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_mov_imm32(unsigned dst, uint32_t value,
                               struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_mov(unsigned dst, unsigned src,
                         struct agx_apple9_packed_instruction *packed);


bool
agx_apple9_pack_device_load_u32(unsigned dst, unsigned index, unsigned binding,
                                uint8_t group_flags,
                                enum agx_apple9_scoreboard_slot scoreboard_slot,
                                struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_device_load_u32_raw(
   unsigned dst, unsigned index, unsigned binding,
   enum agx_apple9_device_load_index_kind index_kind, uint8_t group_flags,
   bool index_first_load_consumer, uint16_t raw_token,
   struct agx_apple9_packed_instruction *packed);

/* Aligned native vector memory forms.  A load defines components consecutive
 * 32-bit GPRs beginning at dst and publishes the complete tuple through one
 * scoreboard slot.  A store consumes components consecutive GPRs beginning
 * at data and allocates no scoreboard slot of its own.  Device-store
 * access_desc bit 0 is the index last-use control: clear retains the index
 * GPR and set releases it after the address read. */
bool agx_apple9_pack_device_load_vector_u32_raw(
   unsigned dst, unsigned index, unsigned binding, unsigned components,
   enum agx_apple9_device_load_index_kind index_kind, uint8_t group_flags,
   bool index_first_load_consumer, uint16_t raw_token,
   struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_device_load_scalar_raw(
   unsigned dst, unsigned index, unsigned binding, unsigned bits,
   enum agx_apple9_device_load_index_kind index_kind, uint8_t group_flags,
   bool index_first_load_consumer, uint16_t raw_token,
   struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_device_store_scalar(
   unsigned data, unsigned index, unsigned binding, unsigned bits,
   enum agx_apple9_device_store_form form, bool release_index,
   struct agx_apple9_packed_instruction *packed);
bool agx_apple9_pack_device_store_vector_u32(
   unsigned data, unsigned index, unsigned binding, unsigned components,
   enum agx_apple9_device_store_form form, bool release_index,
   struct agx_apple9_packed_instruction *packed);

#ifdef __cplusplus
}
#endif

#endif
