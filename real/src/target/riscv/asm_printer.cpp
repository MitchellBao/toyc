#include "asm_printer.h"

#include "frame.h"
#include "peephole.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::riscv {
namespace {

std::string sanitizeLabel(const std::string& name)
{
    std::string result;
    result.reserve(name.size() + 8);
    for (char ch : name) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }
    return result;
}

bool fitsI12(std::int32_t value)
{
    return value >= -2048 && value <= 2047;
}

bool definesValue(const ir::Instruction& inst)
{
    return inst.dst.id >= 0
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal;
}

class FunctionEmitter {
public:
    FunctionEmitter(const ir::Function& function, const std::unordered_map<std::string, std::string>& globals, std::ostream& out)
        : function_(function), globals_(globals), out_(out)
    {
        collectLayout();
    }

    void emit()
    {
        out_ << ".globl " << function_.name << "\n";
        out_ << function_.name << ":\n";
        emitPrologue();
        storeIncomingParams();
        for (int i = 0; i < static_cast<int>(function_.blocks.size()); ++i) {
            out_ << labelFor(i) << ":\n";
            for (const ir::Instruction& inst : function_.blocks[static_cast<std::size_t>(i)].instructions) {
                emitInstruction(inst);
            }
            emitTerminator(function_.blocks[static_cast<std::size_t>(i)].terminator);
        }
    }

private:
    void collectLayout()
    {
        std::unordered_set<std::string> locals;
        int valueCount = 0;
        bool hasCall = false;
        int maxCallArgs = 0;

        for (const std::string& param : function_.params) {
            locals.insert(param);
        }
        for (const ir::BasicBlock& block : function_.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                if ((inst.kind == ir::InstructionKind::LoadLocal || inst.kind == ir::InstructionKind::StoreLocal) && !inst.symbol.empty()) {
                    locals.insert(inst.symbol);
                }
                if (definesValue(inst)) {
                    valueOffsets_[inst.dst.id] = 0;
                    ++valueCount;
                }
                if (inst.kind == ir::InstructionKind::Call) {
                    hasCall = true;
                    maxCallArgs = std::max(maxCallArgs, static_cast<int>(inst.operands.size()));
                }
            }
        }

        outgoingArgBytes_ = std::max(0, maxCallArgs - 8) * 4;
        savesRa_ = hasCall;
        int nextOffset = outgoingArgBytes_;
        for (const std::string& local : locals) {
            localOffsets_[local] = nextOffset;
            nextOffset += 4;
        }
        for (auto& [value, offset] : valueOffsets_) {
            (void)value;
            offset = nextOffset;
            nextOffset += 4;
        }

        const int localBytes = nextOffset - outgoingArgBytes_;
        const int saveBytes = savesRa_ ? 4 : 0;
        frameSize_ = alignTo(outgoingArgBytes_ + localBytes + saveBytes, 16);
        raOffset_ = frameSize_ - 4;
    }

    void emitPrologue()
    {
        if (frameSize_ > 0) {
            out_ << "  addi sp, sp, -" << frameSize_ << "\n";
        }
        if (savesRa_) {
            out_ << "  sw ra, " << raOffset_ << "(sp)\n";
        }
    }

    void emitEpilogue()
    {
        if (savesRa_) {
            out_ << "  lw ra, " << raOffset_ << "(sp)\n";
        }
        if (frameSize_ > 0) {
            out_ << "  addi sp, sp, " << frameSize_ << "\n";
        }
        out_ << "  ret\n";
    }

    void storeIncomingParams()
    {
        for (int i = 0; i < static_cast<int>(function_.params.size()); ++i) {
            const auto found = localOffsets_.find(function_.params[static_cast<std::size_t>(i)]);
            if (found == localOffsets_.end()) {
                continue;
            }
            if (i < 8) {
                out_ << "  sw a" << i << ", " << found->second << "(sp)\n";
            } else {
                const int callerOffset = frameSize_ + (i - 8) * 4;
                out_ << "  lw t0, " << callerOffset << "(sp)\n";
                out_ << "  sw t0, " << found->second << "(sp)\n";
            }
        }
    }

    std::string labelFor(int block) const
    {
        return ".L_" + sanitizeLabel(function_.name) + "_" + std::to_string(block);
    }

    int offsetForValue(ir::Value value) const
    {
        const auto found = valueOffsets_.find(value.id);
        if (found == valueOffsets_.end()) {
            throw std::runtime_error("missing stack slot for IR value");
        }
        return found->second;
    }

    int offsetForLocal(const std::string& symbol) const
    {
        const auto found = localOffsets_.find(symbol);
        if (found == localOffsets_.end()) {
            throw std::runtime_error("missing stack slot for local: " + symbol);
        }
        return found->second;
    }

    void loadOperand(const ir::Operand& operand, const std::string& reg)
    {
        if (operand.isImmediate) {
            out_ << "  li " << reg << ", " << operand.immediate << "\n";
        } else {
            out_ << "  lw " << reg << ", " << offsetForValue(operand.value) << "(sp)\n";
        }
    }

    void storeValue(ir::Value value, const std::string& reg)
    {
        out_ << "  sw " << reg << ", " << offsetForValue(value) << "(sp)\n";
    }

    void emitInstruction(const ir::Instruction& inst)
    {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            loadOperand(inst.operands.empty() ? ir::Operand::imm(0) : inst.operands[0], "t0");
            storeValue(inst.dst, "t0");
            return;
        case ir::InstructionKind::Copy:
            loadOperand(inst.operands[0], "t0");
            storeValue(inst.dst, "t0");
            return;
        case ir::InstructionKind::LoadLocal:
            out_ << "  lw t0, " << offsetForLocal(inst.symbol) << "(sp)\n";
            storeValue(inst.dst, "t0");
            return;
        case ir::InstructionKind::StoreLocal:
            loadOperand(inst.operands[0], "t0");
            out_ << "  sw t0, " << offsetForLocal(inst.symbol) << "(sp)\n";
            return;
        case ir::InstructionKind::LoadGlobal:
            out_ << "  la t0, " << globalLabel(inst.symbol) << "\n";
            out_ << "  lw t0, 0(t0)\n";
            storeValue(inst.dst, "t0");
            return;
        case ir::InstructionKind::StoreGlobal:
            loadOperand(inst.operands[0], "t0");
            out_ << "  la t1, " << globalLabel(inst.symbol) << "\n";
            out_ << "  sw t0, 0(t1)\n";
            return;
        case ir::InstructionKind::Unary:
            emitUnary(inst);
            return;
        case ir::InstructionKind::Binary:
            emitBinary(inst);
            return;
        case ir::InstructionKind::Call:
            emitCall(inst);
            return;
        }
    }

    std::string globalLabel(const std::string& name) const
    {
        const auto found = globals_.find(name);
        if (found == globals_.end()) {
            throw std::runtime_error("unknown global: " + name);
        }
        return found->second;
    }

    void emitUnary(const ir::Instruction& inst)
    {
        loadOperand(inst.operands[0], "t0");
        switch (inst.unaryOp) {
        case ir::UnaryOpcode::Plus:
            break;
        case ir::UnaryOpcode::Minus:
            out_ << "  neg t0, t0\n";
            break;
        case ir::UnaryOpcode::Not:
            out_ << "  seqz t0, t0\n";
            break;
        }
        storeValue(inst.dst, "t0");
    }

    void emitBinary(const ir::Instruction& inst)
    {
        loadOperand(inst.operands[0], "t0");
        loadOperand(inst.operands[1], "t1");
        switch (inst.binaryOp) {
        case ir::BinaryOpcode::Add:
            out_ << "  add t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Sub:
            out_ << "  sub t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Mul:
            out_ << "  mul t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Div:
            out_ << "  div t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Mod:
            out_ << "  rem t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Equal:
            out_ << "  sub t0, t0, t1\n";
            out_ << "  seqz t0, t0\n";
            break;
        case ir::BinaryOpcode::NotEqual:
            out_ << "  sub t0, t0, t1\n";
            out_ << "  snez t0, t0\n";
            break;
        case ir::BinaryOpcode::Less:
            out_ << "  slt t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::Greater:
            out_ << "  slt t0, t1, t0\n";
            break;
        case ir::BinaryOpcode::LessEqual:
            out_ << "  slt t0, t1, t0\n";
            out_ << "  xori t0, t0, 1\n";
            break;
        case ir::BinaryOpcode::GreaterEqual:
            out_ << "  slt t0, t0, t1\n";
            out_ << "  xori t0, t0, 1\n";
            break;
        case ir::BinaryOpcode::LogicalAnd:
            out_ << "  snez t0, t0\n";
            out_ << "  snez t1, t1\n";
            out_ << "  and t0, t0, t1\n";
            break;
        case ir::BinaryOpcode::LogicalOr:
            out_ << "  or t0, t0, t1\n";
            out_ << "  snez t0, t0\n";
            break;
        }
        storeValue(inst.dst, "t0");
    }

    void emitCall(const ir::Instruction& inst)
    {
        for (int i = static_cast<int>(inst.operands.size()) - 1; i >= 8; --i) {
            loadOperand(inst.operands[static_cast<std::size_t>(i)], "t0");
            out_ << "  sw t0, " << ((i - 8) * 4) << "(sp)\n";
        }
        for (int i = 0; i < static_cast<int>(inst.operands.size()) && i < 8; ++i) {
            loadOperand(inst.operands[static_cast<std::size_t>(i)], "a" + std::to_string(i));
        }
        out_ << "  call " << inst.symbol << "\n";
        if (inst.dst.id >= 0) {
            storeValue(inst.dst, "a0");
        }
    }

    void emitTerminator(const ir::Terminator& terminator)
    {
        switch (terminator.kind) {
        case ir::TerminatorKind::Jump:
            out_ << "  j " << labelFor(terminator.trueBlock) << "\n";
            break;
        case ir::TerminatorKind::Branch:
            loadOperand(terminator.condition, "t0");
            out_ << "  beqz t0, " << labelFor(terminator.falseBlock) << "\n";
            out_ << "  j " << labelFor(terminator.trueBlock) << "\n";
            break;
        case ir::TerminatorKind::Return:
            if (terminator.hasReturnValue) {
                loadOperand(terminator.returnValue, "a0");
            }
            emitEpilogue();
            break;
        }
    }

    const ir::Function& function_;
    const std::unordered_map<std::string, std::string>& globals_;
    std::ostream& out_;
    std::unordered_map<std::string, int> localOffsets_;
    std::unordered_map<int, int> valueOffsets_;
    int outgoingArgBytes_ = 0;
    int frameSize_ = 0;
    int raOffset_ = 0;
    bool savesRa_ = false;
};

} // namespace

void AsmPrinter::print(const ir::Module& module, std::ostream& out) const
{
    std::ostringstream buffer;
    std::unordered_map<std::string, std::string> globals;
    buffer << ".data\n";
    for (const ir::Global& global : module.globals) {
        const std::string label = "g_" + sanitizeLabel(global.name);
        globals[global.name] = label;
        buffer << ".globl " << label << "\n";
        buffer << label << ":\n";
        buffer << "  .word " << global.init << "\n";
    }

    buffer << "\n.text\n";
    for (const ir::Function& function : module.functions) {
        FunctionEmitter emitter(function, globals, buffer);
        emitter.emit();
    }

    out << peephole(buffer.str());
}

} // namespace toyc::riscv
