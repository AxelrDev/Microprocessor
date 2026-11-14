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

// Decides whether a branch instruction should be taken based on the funct3 field and the values of registers a and b.
static bool
branch_taken(uint32 funct3, uint32 a, uint32 b, bool *valid)
{
        *valid = true;
        switch (funct3) {
        case 0x0: return a == b; // BEQ
        case 0x1: return a != b; // BNE
        case 0x4: return (int32)a < (int32)b; // BLT
        case 0x5: return (int32)a >= (int32)b; // BGE
        case 0x6: return a < b; // BLTU
        case 0x7: return a >= b; // BGEU
        default:
                *valid = false;
                return false;
        }
}

static ctrl_t
control_unit(const decode_t *d, uint32 instr)
{
        ctrl_t c = {0};
        bool legal = true;

        switch (d->opcode) {
        case OPCODE_OP:
                legal = alu_control(d->funct3, d->funct7, false, &c.alu_op);
                c.uses_rs1 = true;
                c.uses_rs2 = true;
                c.reg_write = true;
                c.wb_sel = WB_ALU;
                break;
        
        case OPCODE_OP_IMM:
                legal = alu_control(d->funct3, d->funct7, true, &c.alu_op);
                c.uses_rs1 = true;
                c.alu_src_imm = true;
                c.reg_write = true;
                c.wb_sel = WB_ALU;
                break;
        case OPCODE_LUI:
                c.reg_write = true;
                c.wb_sel = WB_IMM;
                break;
        case OPCODE_AUIPC:
                c.reg_write = true;
                c.wb_sel = WB_PC_IMM;
                break;
        case OPCODE_JAL:
                c.reg_write = true;
                c.wb_sel = WB_PC4;
                c.is_jal = true;
                break;
        case OPCODE_JALR:
                legal = (d->funct3 == 0x0);
                c.reg_write = true;
                c.wb_sel = WB_PC4;
                c.is_jalr = true;
                c.uses_rs1 = true;
                break;
        case OPCODE_BRANCH:
                legal = (d->funct3 != 0x2 && d->funct3 != 0x3); // Only valid funct3 values for branches
                c.is_branch = true;
                c.uses_rs1 = true;
                c.uses_rs2 = true;
                break;

        case OPCODE_SYSTEM:
                legal = (instr == INSTUCTION_EBREAK);
                c.is_ebreak = true;
                break;
        default:
                legal = false;
                break;
        }

        if (!legal) {
                ctrl_t bad = {0};
                bad.is_illegal = true;
                return bad;
        }

        return c;
}

static uint32
imm_gen(const decode_t *d, uint32 instr)
{
        switch (d->opcode) {
        case OPCODE_OP_IMM:
        case OPCODE_JALR:
                return (uint32)imm_i(instr);
        case OPCODE_BRANCH:
                return (uint32)imm_b(instr);
        case OPCODE_JAL:
                return (uint32)imm_j(instr);
        case OPCODE_LUI:
        case OPCODE_AUIPC:
                return imm_u(instr);
        default:
                return 0; // For unsupported opcodes, return 0
        }
}


void
execute(rv32_t *cpu)
{
        uint32 pc = cpu->pc;
        uint32 instr = fetch(cpu, pc);
        decode_t d = decode(instr);

        uint32 rs1_val = reg_read_x(cpu, d.rs1);
        uint32 rs2_val = reg_read_x(cpu, d.rs2);

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

         uint32 next_pc = pc + 4; // Default next PC is the next instruction
         bool register_write = false;
         uint32 wb_value = 0;
         bool legal = true;

         switch (d.opcode) {
         case OPCODE_OP:
         case OPCODE_OP_IMM:{
                bool is_imm = (d.opcode == OPCODE_OP_IMM);
                alu_op_t op;
                if (!alu_control(d.funct3, d.funct7, is_imm, &op)) {
                        legal = false;
                        break;
                }
                uint32 b = is_imm ? imm_i(instr) : rs2_val;
                wb_value = alu_execute(op, rs1_val, b);
                register_write = true;
                break;
         }
         case OPCODE_LUI:
                wb_value = imm_u(instr);
                register_write = true;
                break;
         case OPCODE_AUIPC:
                wb_value = pc + imm_u(instr);
                register_write = true;
                break;
        case OPCODE_JAL:
                next_pc = pc + imm_j(instr);
                wb_value = pc + 4; // Store return address in rd
                register_write = true;
                break;
        case OPCODE_JALR:
                if (d.funct3 != 0x0) {
                        legal = false;
                        break;
                }
                next_pc = (rs1_val + (uint32)imm_i(instr)) & ~1u; // Clear the least significant bit
                wb_value = pc + 4; // Store return address in rd
                register_write = true;
                break;
        case OPCODE_BRANCH: {
                bool valid;
                bool taken = branch_taken(d.funct3, rs1_val, rs2_val, &valid);
                if (!valid) {
                        legal = false;
                        break;
                }
                if (taken) {
                        next_pc = pc + (uint32)imm_b(instr);
                }
                break;
        }
        default:
                legal = false;
                break;
        }

        if (!legal) {
                illegal_instruction(cpu, pc, instr);
                return;
        }

        if (next_pc & 0x3) {
                SIM_LOG_ERROR(cpu->obj, 0, "instruction address misaligned: 0x%08x at pc 0x%08x", next_pc, pc);
                rv32_halt(cpu, "instruction address misaligned");
                return;
        }

        if(register_write )
                reg_write_x(cpu, d.rd, wb_value);
        
                

        cpu->pc = next_pc; // Move to the next instruction
        increment_step(cpu);
}

/* Text for Simics' disassemble CLI command (e.g. "disassemble 0x0 10")
   for the instruction word instr. Return a string allocated with Simics'
   allocators, e.g. MM_STRDUP() or a strbuf as below; Simics frees it. */

static const char *const alu_name_r[] = {
        [ALU_ADD] = "add",
        [ALU_SUB] = "sub",
        [ALU_SLL] = "sll",
        [ALU_SLT] = "slt",
        [ALU_SLTU] = "sltu",
        [ALU_XOR] = "xor",
        [ALU_SRL] = "srl",
        [ALU_SRA] = "sra",
        [ALU_OR]  = "or",
        [ALU_AND] = "and"
};

static const char *const alu_name_i[] = {
        [ALU_ADD] = "addi",
        [ALU_SLL] = "slli",
        [ALU_SLT] = "slti",
        [ALU_SLTU] = "sltiu",
        [ALU_XOR] = "xori",
        [ALU_SRL] = "srli",
        [ALU_SRA] = "srai",
        [ALU_OR]  = "ori",
        [ALU_AND] = "andi"
};

// Indexed by funct3, with NULL for illegal funct3 values.
static const char *const branch_name[8] = {
       "beq", "bne", NULL, NULL, "blt", "bge", "bltu", "bgeu"
};

char *
rv32_disassemble(uint32 instr)
{
        decode_t d = decode(instr);
        strbuf_t b = SB_INIT;

        switch (d.opcode) {
        case OPCODE_OP:
        case OPCODE_OP_IMM: {
                bool is_imm = (d.opcode == OPCODE_OP_IMM);
                alu_op_t op;
                if (!alu_control(d.funct3, d.funct7, is_imm, &op))
                        break;
                if (!is_imm)
                        sb_addfmt(&b, "%s x%u, x%u, x%u", alu_name_r[op], d.rd, d.rs1, d.rs2);
                else if (op == ALU_SLL || op == ALU_SRL || op == ALU_SRA)
                        sb_addfmt(&b, "%s x%u, x%u, %u", alu_name_i[op], d.rd, d.rs1, d.rs2);
                else
                        sb_addfmt(&b, "%s x%u, x%u, %d", alu_name_i[op], d.rd, d.rs1, imm_i(instr));
                break;
        }
        case OPCODE_LUI:
                sb_addfmt(&b, "lui x%u, 0x%x", d.rd, imm_u(instr) >> 12);
                break;
        case OPCODE_AUIPC:
                sb_addfmt(&b, "auipc x%u, 0x%x", d.rd, imm_u(instr) >> 12);
                break;
        case OPCODE_JAL:
                sb_addfmt(&b, "jal x%u, %d", d.rd, imm_j(instr));
                break;
        case OPCODE_JALR:
                if (d.funct3 == 0x0)
                        sb_addfmt(&b, "jalr x%u, %d(x%u)", d.rd, imm_i(instr), d.rs1);
                break;
        case OPCODE_BRANCH:
                if (branch_name[d.funct3] != NULL)
                        sb_addfmt(&b, "%s x%u, x%u, %d", branch_name[d.funct3], d.rs1, d.rs2, imm_b(instr));
                break;
        case OPCODE_SYSTEM:
                if (instr == INSTUCTION_EBREAK)
                        sb_addfmt(&b, "ebreak");
                break;
        }
        if (sb_len(&b) == 0)
                sb_addfmt(&b, ".word 0x%08x", instr);
        return sb_detach(&b);
}
