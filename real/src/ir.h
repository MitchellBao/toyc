#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace toyc::ir {

enum class Type {
    Int,
    Void,
};

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

struct Value {
    int id = -1;
};

struct Operand {
    bool isImmediate = false;
    std::int32_t immediate = 0;
    Value value;

    static Operand imm(std::int32_t value);
    static Operand ref(Value value);
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

struct BasicBlock {
    std::string label;
    std::vector<Instruction> instructions;
    Terminator terminator;
};

struct Function {
    Type returnType = Type::Int;
    std::string name;
    std::vector<std::string> params;
    std::vector<BasicBlock> blocks;
    int nextValue = 0;
};

struct Global {
    bool isConst = false;
    std::string name;
    std::int32_t init = 0;
};

struct Module {
    std::vector<Global> globals;
    std::vector<Function> functions;
};

const char* instructionKindName(InstructionKind kind);
const char* terminatorKindName(TerminatorKind kind);

} // namespace toyc::ir
