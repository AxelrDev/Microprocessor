# © 2026 Intel Corporation
#
# This software and the related documents are Intel copyrighted materials, and
# your use of them is governed by the express license under which they were
# provided to you ("License"). Unless the License provides otherwise, you may
# not use, modify, copy, publish, distribute, disclose or transmit this software
# or the related documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or
# implied warranties, other than those that are expressly stated in the License.

import re
import simics


def mem_write8(core, addr, value):
    """Write the byte value (0..255) to address addr of the core's memory."""
    simics.SIM_write_phys_memory(core, addr, value, 1)

# opcodes

OP_LUI = 0b0110111
OP_AUIPC = 0b0010111
OP_JAL = 0b1101111
OP_JALR = 0b1100111
OP_BRANCH = 0b1100011
OP_INM = 0b0010011
OP_REG = 0b0110011

EBREAK_WORD = 0x00100073

# Tabla de codificación de instrucciones RISC-V a código máquina

# R-type instructions: opcode, funct3, funct7
R_TYPE = {
    'add': (0b000, 0b0000000),
    'sub': (0b000, 0b0100000),
    'sll': (0b001, 0b0000000),
    'slt': (0b010, 0b0000000),
    'sltu': (0b011, 0b0000000),
    'xor': (0b100, 0b0000000),
    'srl': (0b101, 0b0000000),
    'sra': (0b101, 0b0100000),
    'or': (0b110, 0b0000000),
    'and': (0b111, 0b0000000),
}

# I ALU instructions: opcode, funct3
I_ALU = {
    'addi': (0b000),
    'slti': (0b010),
    'sltiu': (0b011),
    'xori': (0b100),
    'ori': (0b110),
    'andi': (0b111),
}

# Shift instructions: opcode, funct3, funct7

SHIFT = {
    'slli': (0b001, 0b0000000),
    'srli': (0b101, 0b0000000),
    'srai': (0b101, 0b0100000),
}

#Branch instructions: opcode, funct3
BRANCH = {
    'beq': (0b000),
    'bne': (0b001),
    'blt': (0b100),
    'bge': (0b101),
    'bltu': (0b110),
    'bgeu': (0b111),
}

# U-type instructions: opcode

U_TYPE = {
    'lui': (0b0110111),
    'auipc': (0b0010111),
}

# Analysis of operands and instruction encoding

REG_RE = re.compile(r"x(\d{1,2})")
MEMOMORY_OPERAND_RE = re.compile(r"^(.*)\(\s*([^)]*?)\s*\)$")

class AsmError(Exception):
    """Exception raised when an error occurs in the assembly program."""

def parse_register(reg_str):
    t = reg_str.strip().lower()
    m = REG_RE.fullmatch(t)
    if m is None or int(m.group(1)) > 31:
        raise AsmError("Invalid register: '%s' is sperated 0x ... x31" % reg_str.strip())
    return int(m.group(1))

def parse_int(reg_str):
    t = reg_str.strip().lower()
    if not t:
        raise AsmError("Number expected")
    sign = 1
    if t[0] in '+-':
        if t[0] == '-':
            sign = -1
        t = t[1:]
    try:
        value = int(t[2:], 16) if t.startswith('0x') else int(t, 10)
    except ValueError:
        raise AsmError("Invalid number: '%s'" % reg_str.strip()) from None
    return sign * value

def check_range(value, lo, hi, what):
    if not (lo <= value <= hi):
        raise AsmError("%s %d is out of range [%d, %d]" % (what, value, lo, hi))
    
def check_even(value, what):
    if value % 2 != 0:
        raise AsmError("%s %d is not even" % (what, value))
    
def expect(operands, count, mnemonic):
    if len(operands) != count:
        raise AsmError("Instruction '%s' expects %d operands, got %d" % (mnemonic, count, len(operands)))


# Codification of instructions

def  encode_r(funct7, rs2, rs1, funct3, rd, opcode):
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode

def encode_i(imm, rs1, funct3, rd, opcode):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode

def encode_b(offset, rs2, rs1, funct3):
    imm = offset & 0x1FFF
    return ((((imm >> 12) & 0x1) << 31) | (((imm >> 5) & 0x3F) << 25) 
            | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) 
            | (((imm >> 1) & 0xF) << 8) | (((imm >> 11) & 0x1) << 7) | OP_BRANCH)


def encode_u(imm20, rd, opcode):
    return ((imm20 & 0xFFFFF) << 12) | (rd << 7) | opcode

def encode_j(offset, rd):
    imm = offset & 0x1FFFFF
    return ((((imm >> 20) & 0x1) << 31) | (((imm >> 1) & 0x3FF) << 21) 
            | (((imm >> 11) & 0x1) << 20) | (((imm >> 12) & 0xFF) << 12) 
            | (rd << 7) | OP_JAL)

# Ensamble of one line of assembly code

def split_line(line):
    code = line.split('#', 1)[0].strip()
    if not code:
        return None, []
    parts = code.split(None, 1)
    mnemonic = parts[0].lower()
    if len(parts) == 1:
        return mnemonic, []
    return mnemonic, [op.strip() for op in parts[1].split(',')]

def assemble_instruction(mnemonic, operands):
    if mnemonic in R_TYPE:
        expect(operands, 3, mnemonic)
        funct3, funct7 = R_TYPE[mnemonic]
        return encode_r(funct7, parse_register(operands[2]), parse_register(operands[1]), funct3, parse_register(operands[0]), OP_REG)
    
    if mnemonic in I_ALU:
        expect(operands, 3, mnemonic)
        imm = parse_int(operands[2])
        check_range(imm, -2048, 2047, "Immediate")
        return encode_i(imm, parse_register(operands[1]), I_ALU[mnemonic], parse_register(operands[0]), OP_INM)
    
    if mnemonic in SHIFT:
        expect(operands, 3, mnemonic)
        funct3, funct7 = SHIFT[mnemonic]
        shamt = parse_int(operands[2])
        check_range(shamt, 0, 31, "Shamt")
        return encode_r(funct7, shamt, parse_register(operands[1]), funct3, parse_register(operands[0]), OP_INM)
    
    if mnemonic in BRANCH:
        expect(operands, 3, mnemonic)
        offset = parse_int(operands[2])
        check_range(offset, -4096, 4094, "Branch offset")
        check_even(offset, "Branch offset")
        return encode_b(offset, parse_register(operands[1]), parse_register(operands[0]), BRANCH[mnemonic])
    
    if mnemonic  == 'jal':
        expect(operands, 2, mnemonic)
        offset = parse_int(operands[1])
        check_range(offset, -1048576, 1048574, "JAL offset")
        check_even(offset, "JAL offset")
        return encode_j(offset, parse_register(operands[0]))
    
    if mnemonic == 'jalr':
        expect(operands, 2, mnemonic)
        m = MEMOMORY_OPERAND_RE.match(operands[1])
        if m is None:
            raise AsmError("Invalid JALR operand: '%s'" % operands[1])
        imm = parse_int(m.group(1).strip() or '0')
        check_range(imm, -2048, 2047, "JALR immediate")
        return encode_i(imm, parse_register(m.group(2)), 0b000, parse_register(operands[0]), OP_JALR)
    
    if mnemonic in U_TYPE:
        expect(operands, 2, mnemonic)
        imm = parse_int(operands[1])
        check_range(imm, -0x80000, 0xfffff, " upper immediate")
        return encode_u(imm, parse_register(operands[0]), U_TYPE[mnemonic])
    
    if mnemonic == 'ebreak':
        expect(operands, 0, mnemonic)
        return EBREAK_WORD
    
    raise AsmError("Unknown instruction: '%s'" % mnemonic)

def assemble_file(path):
    """Read the assembly program in the text file path and return a list of
    machine code words of its instructions."""
    words = []
    with open(path, encoding="utf-8") as f:
        for linen, line in enumerate(f, start=1):
            mnemonic, operands = split_line(line)
            if mnemonic is None:
                continue
            try:
                words.append(assemble_instruction(mnemonic, operands))
            except AsmError as e:
                raise AsmError("%s:%d: %s\n -> %s"
                               % (path, linen, line.strip())) from None
    return words

def load_program(core, path, address):
    """Read the assembly program in the text file path and write the machine
    code of its instructions into memory, the first one at address."""
    try:
        words = assemble_file(path)
    except AsmError as e:
        raise simics.SimExc_General(str(e)) from None
    for i, word in enumerate(words):
        base = address + i * 4
        for b in range(4):
            mem_write8(core, base + b, (word >> (b * 8)) & 0xFF)

    simics.SIM_log_info(1, core, 0, "%d instructions loaded from '%s' at 0x%X" % (len(words), path, address))
