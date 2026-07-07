#pragma once

#include "value.h"

#include <string>
#include <vector>

namespace toyc::ir {

enum class BinaryOpcode {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
};

enum class UnaryOpcode {
    Plus,
    Minus,
    Not,
};

enum class InstructionKind {
    Const,
    Copy,
    Unary,
    Binary,
    LoadGlobal,
    StoreGlobal,
    LoadLocal,
    StoreLocal,
    Call,
};

enum class TerminatorKind {
    Jump,
    Branch,
    Return,
};

struct Instruction {
    InstructionKind kind = InstructionKind::Const;
    Value dst;
    std::vector<Operand> operands;
    std::string symbol;
    BinaryOpcode binaryOp = BinaryOpcode::Add;
    UnaryOpcode unaryOp = UnaryOpcode::Plus;
    bool hasSideEffect = false;
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::Return;
    Operand condition;
    int trueBlock = -1;
    int falseBlock = -1;
    Operand returnValue;
    bool hasReturnValue = false;
};

const char* instructionKindName(InstructionKind kind);
const char* terminatorKindName(TerminatorKind kind);

} // namespace toyc::ir
