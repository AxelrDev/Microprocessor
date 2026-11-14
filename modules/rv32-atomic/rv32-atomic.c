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

#include <simics/util/strbuf.h>
#include "cosim.h"
#include "rv32-core.h"
#include "rv32-helpers.h"

// table of opcodes for the RV32I base instruction set
#define OPCODE_LUI 0x37
#define OPCODE_AUIPC 0x17
#define OPCODE_JAL 0x6F
#define OPCODE_JALR 0x67
#define OPCODE_BRANCH 0x63
#define OPCODE_OP_IMM 0x13
#define OPCODE_OP 0x33
#define OPCODE_SYSTEM 0x73

#define INSTUCTION_EBREAK 0x00100073u

typedef struct {
        uint32 raw; // raw instruction word
        uint32 opcode; // opcode field, bits 6:0
        uint32 rd; // destination register, bits 11:7
        uint32 funct3; // funct3 field, bits 14:12
        uint32 rs1; // source register 1, bits 19:15
        uint32 rs2; // source register 2, bits 24:20
        uint32 funct7; // funct7 field, bits 31:25
} decode_t;

// Fetch the instruction word at the given program counter (pc) from memory. 
// The instruction word is 32 bits (4 bytes) long, and we read it in little-endian order (least significant byte first). 
// The function returns the instruction word as a uint32 value.
static uint32
fetch(rv32_t *cpu, uint32 pc)
{
        return (uint32)mem_read8(cpu, pc) 
        | ((uint32)mem_read8(cpu, pc + 1) << 8)
        | ((uint32)mem_read8(cpu, pc + 2) << 16)
        | ((uint32)mem_read8(cpu, pc + 3) << 24);
}

static decode_t
decode(uint32 instr)
{
        decode_t d;
        d.raw = instr;
        d.opcode = instr & 0x7F; // bits 6:0
        d.rd = (instr >> 7) & 0x1F; // bits 11:7
        d.funct3 = (instr >> 12) & 0x7; // bits 14:12
        d.rs1 = (instr >> 15) & 0x1F; // bits 19:15
        d.rs2 = (instr >> 20) & 0x1F; // bits 24:20
        d.funct7 = (instr >> 25) & 0x7F; // bits 31:25
        return d;
}

// Decode the instruction word into its constituent fields. The instruction word is 32 bits long, 
// and we extract the opcode, destination register (rd), funct3, source registers (rs1 and rs2), 
// and funct7 fields from it. The function returns a decode_t structure containing these fields.
static int32
sign_extend(uint32 value, unsigned bits)
{
        uint32 sign = 1u << (bits - 1);
        value &= (sign << 1) - 1; // Mask to the specified number of bits
        return (int32)((value ^ sign) - sign); // Sign-extend the value
}

// Extract the immediate value for I-type instructions from the instruction word.
static int32
imm_i(uint32 instr)
{
        return sign_extend(instr >> 20, 12);
}

// Extract the immediate value for B-type instructions from the instruction word.
static int32
imm_b(uint32 instr)
{
        uint32 imm = ((instr >> 31) & 0x1) << 12 // bit 12
                | ((instr >> 7) & 0x1) << 11 // bit 11
                | ((instr >> 25) & 0x3F) << 5 // bits 10:5
                | ((instr >> 8) & 0xF) << 1; // bits 4:1
        return sign_extend(imm, 13);
}

// Extract the immediate value for U-type instructions from the instruction word.
static uint32
imm_u(uint32 instr)
{
        return instr & 0xFFFFF000u; // bits 31:12
}

// Extract the immediate value for J-type instructions from the instruction word.
static int32
imm_j(uint32 instr)
{
        uint32 imm = ((instr >> 31) & 0x1) << 20 // bit 20
                | ((instr >> 12) & 0xFF) << 12 // bits 19:12
                | ((instr >> 20) & 0x1) << 11 // bit 11
                | ((instr >> 21) & 0x3FF) << 1; // bits 10:1
        return sign_extend(imm, 21);
}

// Operations of the RV32I Integer ALU. The ALU performs arithmetic and logical operations on the operands provided by the instruction.
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
        ALU_AND,
} alu_op_t;

// Execute the ALU operation specified by op on the operands a and b. The result of the operation is returned as a uint32 value.
static uint32
alu_execute(alu_op_t op, uint32 a, uint32 b)
{
        uint32 shamt = b & 0x1F; // Shift amount is the lower 5 bits of b
        switch (op) {
        case ALU_ADD:
                return a + b;
        case ALU_SUB:
                return a - b;
        case ALU_SLL:
                return a << shamt;
        case ALU_SLT:
                return (int32)a < (int32)b ? 1 : 0;
        case ALU_SLTU:
                return a < b ? 1 : 0;
        case ALU_XOR:
                return a ^ b;
        case ALU_SRL:
                return a >> shamt;
        case ALU_SRA:
                if (a & 0x80000000u)
                        return (a >> shamt) | ~(0xFFFFFFFFu >> shamt);
                return a >> shamt;
        case ALU_OR:
                return a | b;
        case ALU_AND:
                return a & b;
        }
        return 0; // Should never reach here
}

static bool
alu_control(uint32 funct3, uint32 funct7,bool is_imm, alu_op_t *op)
{
        switch (funct3) {
        case 0x0:
                if (is_imm || funct7 == 0x00){
                        *op = ALU_ADD;
                        return true;
                }
                if (funct7 == 0x20) {
                        *op = ALU_SUB;
                        return true;
                }
                return false;
        case 0x1:
                if(funct7 != 0x00) return false;
                *op = ALU_SLL;
                return true;
        case 0x5:
                if (funct7 == 0x00) {
                        *op = ALU_SRL;
                        return true;
                }
                if (funct7 == 0x20) {
                        *op = ALU_SRA;
                        return true;
                }
                return false;
        case 0x2: *op = ALU_SLT; break;
        case 0x3: *op = ALU_SLTU; break;
        case 0x4: *op = ALU_XOR; break;
        case 0x6: *op = ALU_OR; break;
        case 0x7: *op = ALU_AND; break;
        default: return false;
        }

        return is_imm || funct7 == 0x00;
                
}

void
execute(rv32_t *cpu)
{
        uint32 pc = cpu->pc;
        uint32 instr = fetch(cpu, pc);
        decode_t d = decode(instr);

        SIM_LOG_INFO(2, cpu->obj, 0, 
         "pc=0x%08x, instr=0x%08x, opcode=0x%02x, rd=x%u, funct3=0x%u, rs1=x%u, rs2=x%u, funct7=0x%02x"
         " |I=%d B=%d U=0x%08x J=%d",
         pc, instr, d.opcode, d.rd, d.funct3, d.rs1, d.rs2, d.funct7,
         imm_i(instr), imm_b(instr), imm_u(instr), imm_j(instr));

         increment_cycle(cpu);

         if (instr == INSTUCTION_EBREAK) {
                 increment_step(cpu);
                 ebreak(cpu);
                 return;
         }
         
         if (d.opcode == OPCODE_OP || d.opcode == OPCODE_OP_IMM) {
                 bool is_imm = (d.opcode == OPCODE_OP_IMM);
                 alu_op_t op;
                 if (!alu_control(d.funct3, d.funct7, is_imm, &op)) {
                         illegal_instruction(cpu, pc, instr);
                         return;
                 }

                 uint32 a = reg_read_x(cpu, d.rs1);
                 uint32 b = is_imm ? (uint32)imm_i(instr) : reg_read_x(cpu, d.rs2);

                 reg_write_x(cpu, d.rd, alu_execute(op, a, b));
        }

        cpu->pc += 4; // Move to the next instruction
        increment_step(cpu);
}

/* Text for Simics' disassemble CLI command (e.g. "disassemble 0x0 10")
   for the instruction word instr. Return a string allocated with Simics'
   allocators, e.g. MM_STRDUP() or a strbuf as below; Simics frees it. */
char *
rv32_disassemble(uint32 instr)
{
        strbuf_t b = SB_INIT;
        sb_addfmt(&b, ".word 0x%08x", instr);
        return sb_detach(&b);
}
