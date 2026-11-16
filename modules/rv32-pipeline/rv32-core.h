/*
  © 2026 Intel Corporation

  This software and the related documents are Intel copyrighted materials, and
  your use of them is governed by the express license under which they were
  provided to you ("License"). Unless the License provides otherwise, you may
  not use, modify, copy, publish, distribute, disclose or transmit this software
  or the related documents without Intel's prior written permission.

  This software and the related documents are provided as is, with no express or
  implied warranties, other than those that are expressly stated in the License.
*/

#ifndef RV32_CORE_H
#define RV32_CORE_H

#include "cosim-core.h"

#define RV32_NUM_REGS   32

// Decode the instruction word into its constituent fields.
typedef struct {
  uint32 raw; // raw instruction word
  uint32 opcode; // opcode field, bits 6:0
  uint32 rd; // destination register, bits 11:7
  uint32 funct3; // funct3 field, bits 14:12
  uint32 rs1; // source register 1, bits 19:15
  uint32 rs2; // source register 2, bits 24:20
  uint32 funct7; // funct7 field, bits 31:25
} decode_t;

// ALU operations for the RV32I Integer ALU. The ALU performs arithmetic and logical operations on the operands provided by the instruction.
typedef enum {
        ALU_ADD,
        ALU_SUB,
        ALU_SLL,
        ALU_SLT,
        ALU_SLTU,
        ALU_XOR,
        ALU_SRL,
        ALU_SRA,
        ALU_OR,
        ALU_AND
} alu_op_t;

typedef enum {
  WB_ALU, // ALU result: OP, OP-IMM
  WB_PC4, // Return address: JAL, JALR
  WB_IMM, // imm << 12: LUI
  WB_PC_IMM // PC + (imm << 12): AUIPC
} wb_sel_t;

typedef struct {
  bool reg_write;
  bool alu_src_imm;
  alu_op_t alu_op;
  wb_sel_t wb_sel;
  bool is_branch;
  bool is_jal;
  bool is_jalr;
  bool uses_rs1;
  bool uses_rs2;
  bool is_ebreak;
  bool is_illegal;
} ctrl_t;

typedef struct {
  bool valid;
  uint32 pc;
  uint32 instr;
} if_id_t;

typedef struct {
  bool valid;
  uint32 pc;
  uint32 instr;
  decode_t d;
  ctrl_t ctrl;
  uint32 rs1_val;
  uint32 rs2_val;
  uint32 imm;
} id_ex_t;

typedef struct {
  bool valid;
  uint32 pc;
  uint32 instr;
  uint32 rd;
  bool reg_write;
  uint32 wb_value;
  bool is_ebreak;
  bool is_illegal;
  bool is_misaligned;
  uint32 bad_target;
} ex_wb_t;


typedef struct rv32 {
        cosim_core_t *core;  /* pass to cosim_core_* functions */
        conf_object_t *obj;  /* the core object, for SIM_LOG_* */

        uint32 pc;
        uint32 x[RV32_NUM_REGS];   /* use reg_read_x/reg_write_x: x0 is zero */

        if_id_t if_id;
        id_ex_t id_ex;
        ex_wb_t ex_wb;

        uint64 cycles;
        uint64 retired;
        uint64 stalls;
        uint64 flushes;
        uint64 forwards;

        bool fwd_en;

} rv32_t;

bool read_simics_memory(rv32_t *cpu, uint32 addr, uint8 *buf, unsigned len);

void rv32_halt(rv32_t *cpu, const char *reason);

void execute(rv32_t *cpu);

char *rv32_disassemble(uint32 instr);

void rv32_init_class(cosim_core_class_t *core_class);
struct rv32 *rv32_new_instance(cosim_core_t *core);
void rv32_delete_instance(rv32_t *cpu);
rv32_t *rv32_from_core(cosim_core_t *core);
logical_address_t rv32_get_pc(cosim_core_t *core);
void rv32_set_pc(cosim_core_t *core, logical_address_t pc);
void rv32_execute(cosim_core_t *core);

#endif
