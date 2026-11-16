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

#include <stdio.h>
#include <string.h>
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

static void pipeline_clear (rv32_t *cpu) {
        memset(&cpu->if_id, 0, sizeof(cpu->if_id));
        memset(&cpu->id_ex, 0, sizeof(cpu->id_ex));
        memset(&cpu->ex_wb, 0, sizeof(cpu->ex_wb));
}

static void stage_if (rv32_t *cpu, if_id_t *out) {
        out->valid = true;
        out->pc = cpu->pc;
        out->instr = fetch(cpu, cpu->pc);
}

static void stage_id (rv32_t *cpu, const if_id_t *in, id_ex_t *out) {
        memset(out, 0, sizeof(*out));
        if (!in->valid) {
                return;
        }
        out->valid = true;
        out->pc = in->pc;
        out->instr = in->instr;
        out->d = decode(in->instr);
        out->ctrl = control_unit(&out->d, in->instr);
        out->imm = imm_gen(&out->d, in->instr);
        out->rs1_val = reg_read_x(cpu, out->d.rs1);
        out->rs2_val = reg_read_x(cpu, out->d.rs2);
}

static void stage_ex (rv32_t *cpu, const id_ex_t *in, ex_wb_t *out, bool *redirect, uint32 *target) {
        memset(out, 0, sizeof(*out));
        *redirect = false;
        *target = 0;
        if (!in->valid) {
                return;
        }

        uint32 a = in->rs1_val;
        uint32 b = in->rs2_val;
        
        const ex_wb_t *prev = &cpu->ex_wb;
        if (cpu->fwd_en && prev->valid && prev->reg_write && prev->rd != 0) {
                if (in->ctrl.uses_rs1 && in->d.rs1 == prev->rd) {
                        a = prev->wb_value;
                        cpu->forwards++;
                        SIM_LOG_INFO(3, cpu->obj, 0,
                                     "forward rs1 x%u = 0x%08x (EX/WB -> EX) a pc 0x%08x",
                                     prev->rd, prev->wb_value, in->pc);
                }

                if (in->ctrl.uses_rs2 && in->d.rs2 == prev->rd) {
                        b = prev->wb_value;
                        cpu->forwards++;
                        SIM_LOG_INFO(3, cpu->obj, 0,
                                     "forward rs2 x%u = 0x%08x (EX/WB -> EX) a pc 0x%08x",
                                     prev->rd, prev->wb_value, in->pc);
                }
        }

        uint32 op2 = in->ctrl.alu_src_imm ? in->imm : b;

        out->valid = true;
        out->pc = in->pc;
        out->instr = in->instr;
        out->rd = in->d.rd;
        out->reg_write = in->ctrl.reg_write;
        out->is_ebreak = in->ctrl.is_ebreak;
        out->is_illegal = in->ctrl.is_illegal;

        switch (in->ctrl.wb_sel) {
                case WB_ALU:
                        out->wb_value = alu_execute(in->ctrl.alu_op, a, op2);
                        break;
                case WB_PC4:
                        out->wb_value = in->pc + 4;
                        break;
                case WB_IMM:
                        out->wb_value = in->imm;
                        break;
                case WB_PC_IMM:
                        out->wb_value = in->pc + in->imm;
                        break;
        }

        bool valid;
        if (in->ctrl.is_jal) {
                *redirect = true;
                *target = in->pc + in->imm;
        } else if (in->ctrl.is_jalr) {
                *redirect = true;
                *target = (a + in->imm) & ~1u;
        } else if (in->ctrl.is_branch && branch_taken(in->d.funct3, a, b, &valid)) {
                *redirect = true;
                *target = in->pc + in->imm;
        }
        
        if (*redirect && (*target & 0x3)) {
                out->is_misaligned = true;
                out->bad_target = *target;
                out->reg_write = false;
                *redirect = false; // Don't actually redirect, just report the misalignment
                *target = 0;
        }
}

static bool stage_wb (rv32_t *cpu, const ex_wb_t *in) {
        if (!in->valid) {
                return false;
        }

        if (in->is_illegal) {
                cpu->pc = in->pc;
                illegal_instruction(cpu, in->pc, in->instr);
                return true;
        }

        if (in->is_misaligned) {
                cpu->pc = in->pc;
                SIM_LOG_ERROR(cpu->obj, 0,
                              "instruction address misaligned: 0x%08x at pc 0x%08x",
                              in->bad_target, in->pc);
                rv32_halt(cpu, "instruction address misaligned");
                return true;
        }
        
        if (in->reg_write && in->rd != 0) {
                reg_write_x(cpu, in->rd, in->wb_value);
        }
        cpu->retired++;
        increment_step(cpu);

        if (in->is_ebreak) {
                cpu->pc = in->pc;
                ebreak(cpu);
                return true;
        }

        return false;
        
}

static const char * slot(char *buf, size_t len, bool valid, uint32 pc) {
        if (valid) {
                snprintf(buf, len, "%08x", pc);
        } else {
                snprintf(buf, len, "--------");
        }
        return buf;
}

static void trace_cycle(rv32_t *cpu) {
        char s_if[12], s_id[12], s_ex[12], s_wb[12];
        SIM_LOG_INFO(3, cpu->obj, 0,
                     "ciclo %4u | IF %s | ID %s | EX %s | WB %s",
                     (unsigned)cpu->cycles,
                     slot(s_if, sizeof s_if, true, cpu->pc),
                     slot(s_id, sizeof s_id, cpu->if_id.valid, cpu->if_id.pc),
                     slot(s_ex, sizeof s_ex, cpu->id_ex.valid, cpu->id_ex.pc),
                     slot(s_wb, sizeof s_wb, cpu->ex_wb.valid, cpu->ex_wb.pc));
}

static bool hazard_detected (const rv32_t *cpu, const id_ex_t *id, const id_ex_t *ex) {
        if (cpu->fwd_en || !id->valid || !ex->valid) {
                return false;
        }

        if (!ex->ctrl.reg_write || ex->d.rd == 0) {
                return false;
        }

        return (id->ctrl.uses_rs1 && id->d.rs1 == ex->d.rd) || (id->ctrl.uses_rs2 && id->d.rs2 == ex->d.rd);
}

static void print_stats(rv32_t *cpu) {
        double cpi = cpu->retired > 0 ? (double)cpu->cycles / cpu->retired : 0.0;
        SIM_LOG_INFO(1, cpu->obj, 0,
                     "Pipeline: fowarding %s, cycles %llu, instructions %llu, "
                     "CPI= %.2f, stalls %llu, flushes %llu, forwards %llu",
                         cpu->fwd_en ? "on" : "off",
                         (unsigned long long)cpu->cycles,
                         (unsigned long long)cpu->retired, cpi,
                         (unsigned long long)cpu->stalls,
                         (unsigned long long)cpu->flushes, 
                         (unsigned long long)cpu->forwards);
}

void execute (rv32_t *cpu) {
        increment_cycle(cpu);
        cpu->cycles++;
        trace_cycle(cpu);

        if (stage_wb(cpu, &cpu->ex_wb)) {
                pipeline_clear(cpu);
                print_stats(cpu);
                return;
        }

        ex_wb_t next_ex_wb;
        id_ex_t next_id_ex;
        if_id_t next_if_id;
        bool redirect;
        uint32 target;

        stage_ex(cpu, &cpu->id_ex, &next_ex_wb, &redirect, &target);
        stage_id(cpu, &cpu->if_id, &next_id_ex);
        stage_if(cpu, &next_if_id);

        bool stall = false;
        if (redirect) {
                cpu->flushes++;
                SIM_LOG_INFO(3, cpu->obj, 0,
                             "flush: salto en pc 0x%08x a 0x%08x, se descartan"
                             " ID 0x%08x e IF 0x%08x",
                             cpu->id_ex.pc, target, cpu->if_id.pc, cpu->pc);
                memset(&next_id_ex, 0, sizeof(next_id_ex));
                memset(&next_if_id, 0, sizeof(next_if_id));
        } else {
                stall = hazard_detected(cpu, &next_id_ex, &cpu->id_ex);
                if (stall) {
                        cpu->stalls++;
                        SIM_LOG_INFO(3, cpu->obj, 0,
                                     "stall: pc 0x%08x espera a x%u (pc 0x%08x en EX)",
                                     cpu->if_id.pc, cpu->id_ex.d.rd, cpu->id_ex.pc);
                        memset(&next_id_ex, 0, sizeof(next_id_ex));
                        next_if_id = cpu->if_id;
                }
        }

        cpu->ex_wb = next_ex_wb;
        cpu->id_ex = next_id_ex;
        cpu->if_id = next_if_id;
        if (redirect) {
                cpu->pc = target;
        } else if (!stall) {
                cpu->pc = cpu->pc + 4;
        }
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
