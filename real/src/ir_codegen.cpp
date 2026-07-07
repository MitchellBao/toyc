#include "ir_codegen.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc {
namespace {

class IrCodegenError final : public std::runtime_error {
public:
    explicit IrCodegenError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

struct LocalSlot {
    bool inRegister = false;
    int reg = -1;
    int offset = 0;
};

struct ValueAlias {
    enum class Kind {
        Immediate,
        Value,
        Local,
        Register,
    };

    Kind kind = Kind::Immediate;
    std::int32_t immediate = 0;
    ir::Value value;
    std::string local;
    std::string reg;
};

int alignTo(int value, int alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

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

const char* savedRegName(int index)
{
    static const char* regs[] = {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
    return regs[index];
}

class FunctionEmitter {
public:
    FunctionEmitter(const ir::Function& function, std::ostream& out)
        : function_(function), out_(out)
    {
    }

    void emit()
    {
        collectLayout();
        emitPrologue();
        emitBlocks();
        emitEpilogue();
    }

private:
    bool hasCalls() const
    {
        for (const auto& block : function_.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.kind == ir::InstructionKind::Call) {
                    return true;
                }
            }
        }
        return false;
    }

    void collectLayout()
    {
        savesRa_ = hasCalls();
        std::unordered_map<std::string, int> localUseCounts;
        std::unordered_set<std::string> locals;

        auto touchLocal = [&](const std::string& symbol, int weight) {
            if (symbol.empty()) {
                return;
            }
            locals.insert(symbol);
            localUseCounts[symbol] += weight;
        };

        for (const std::string& param : function_.params) {
            touchLocal(param, 4);
        }

        int maxValue = -1;
        for (const auto& block : function_.blocks) {
            for (const auto& inst : block.instructions) {
                maxValue = std::max(maxValue, inst.dst.id);
                for (const auto& operand : inst.operands) {
                    countValueUse(operand);
                }
                switch (inst.kind) {
                case ir::InstructionKind::LoadLocal:
                    touchLocal(inst.symbol, 3);
                    break;
                case ir::InstructionKind::StoreLocal:
                    touchLocal(inst.symbol, 2);
                    break;
                default:
                    break;
                }
            }
            if (block.hasTerminator) {
                countValueUse(block.terminator.condition);
                countValueUse(block.terminator.returnValue);
            }
        }

        std::vector<std::string> orderedLocals(locals.begin(), locals.end());
        std::sort(orderedLocals.begin(), orderedLocals.end(), [&](const std::string& lhs, const std::string& rhs) {
            if (localUseCounts[lhs] != localUseCounts[rhs]) {
                return localUseCounts[lhs] > localUseCounts[rhs];
            }
            return lhs < rhs;
        });

        const int regBudget = std::min<int>(11, static_cast<int>(orderedLocals.size()));
        for (int i = 0; i < regBudget; ++i) {
            locals_[orderedLocals[i]] = LocalSlot{true, i, 0};
            usedSavedRegs_.push_back(i);
        }

        savedAreaBytes_ = 4; // s0
        if (savesRa_) {
            savedAreaBytes_ += 4;
        }
        savedAreaBytes_ += static_cast<int>(usedSavedRegs_.size()) * 4;

        int localStackSlots = 0;
        for (const std::string& local : orderedLocals) {
            if (locals_.contains(local)) {
                continue;
            }
            ++localStackSlots;
            locals_[local] = LocalSlot{false, -1, -(savedAreaBytes_ + 4 * localStackSlots)};
        }

        valueOffsets_.assign(static_cast<std::size_t>(maxValue + 1), 0);
        for (int value = 0; value <= maxValue; ++value) {
            ++localStackSlots;
            valueOffsets_[static_cast<std::size_t>(value)] = -(savedAreaBytes_ + 4 * localStackSlots);
        }

        frameSize_ = alignTo(savedAreaBytes_ + localStackSlots * 4, 16);
        remainingValueUses_ = valueUseCounts_;
    }

    std::string blockLabel(int block) const
    {
        return ".L_" + sanitizeLabel(function_.name) + "_" + std::to_string(block);
    }

    void emitPrologue()
    {
        out_ << "\n.globl " << function_.name << "\n";
        out_ << function_.name << ":\n";
        out_ << "  addi sp, sp, -" << frameSize_ << "\n";

        int saveOffset = frameSize_ - 4;
        if (savesRa_) {
            out_ << "  sw ra, " << saveOffset << "(sp)\n";
            raOffset_ = saveOffset;
            saveOffset -= 4;
        }
        out_ << "  sw s0, " << saveOffset << "(sp)\n";
        s0Offset_ = saveOffset;
        saveOffset -= 4;
        for (int reg : usedSavedRegs_) {
            out_ << "  sw " << savedRegName(reg) << ", " << saveOffset << "(sp)\n";
            savedRegOffsets_[reg] = saveOffset;
            saveOffset -= 4;
        }

        out_ << "  addi s0, sp, " << frameSize_ << "\n";
        for (std::size_t i = 0; i < function_.params.size(); ++i) {
            if (i < 8) {
                storeLocal(function_.params[i], "a" + std::to_string(i));
            } else {
                out_ << "  lw t0, " << static_cast<int>((i - 8) * 4) << "(s0)\n";
                storeLocal(function_.params[i], "t0");
            }
        }
    }

    void emitBlocks()
    {
        for (std::size_t i = 0; i < function_.blocks.size(); ++i) {
            out_ << blockLabel(static_cast<int>(i)) << ":\n";
            const auto& block = function_.blocks[i];
            for (const auto& inst : block.instructions) {
                emitInstruction(inst);
            }
            if (block.hasTerminator) {
                emitTerminator(block.terminator);
            } else {
                out_ << "  j " << returnLabel() << "\n";
            }
        }
    }

    void emitEpilogue()
    {
        out_ << returnLabel() << ":\n";
        for (int reg : usedSavedRegs_) {
            out_ << "  lw " << savedRegName(reg) << ", " << savedRegOffsets_[reg] << "(sp)\n";
        }
        if (savesRa_) {
            out_ << "  lw ra, " << raOffset_ << "(sp)\n";
        }
        out_ << "  lw s0, " << s0Offset_ << "(sp)\n";
        out_ << "  addi sp, sp, " << frameSize_ << "\n";
        out_ << "  ret\n";
    }

    std::string returnLabel() const
    {
        return ".L_" + sanitizeLabel(function_.name) + "_return";
    }

    void emitInstruction(const ir::Instruction& inst)
    {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            aliasImmediate(inst.dst, inst.operands.at(0).immediate);
            break;
        case ir::InstructionKind::Copy:
            aliasOperand(inst.dst, inst.operands.at(0));
            break;
        case ir::InstructionKind::Unary:
            loadOperand(inst.operands.at(0), "a0");
            emitUnary(inst.unaryOp);
            storeValue(inst.dst, "a0");
            break;
        case ir::InstructionKind::Binary:
            emitBinary(inst);
            break;
        case ir::InstructionKind::LoadGlobal:
            out_ << "  la t0, g_" << sanitizeLabel(inst.symbol) << "\n";
            out_ << "  lw a0, 0(t0)\n";
            storeValue(inst.dst, "a0");
            break;
        case ir::InstructionKind::StoreGlobal:
            loadOperand(inst.operands.at(0), "a0");
            out_ << "  la t0, g_" << sanitizeLabel(inst.symbol) << "\n";
            out_ << "  sw a0, 0(t0)\n";
            break;
        case ir::InstructionKind::LoadLocal:
            aliasLocal(inst.dst, inst.symbol);
            break;
        case ir::InstructionKind::StoreLocal:
            loadOperand(inst.operands.at(0), "a0");
            storeLocal(inst.symbol, "a0");
            break;
        case ir::InstructionKind::Call:
            emitCall(inst);
            storeValue(inst.dst, "a0");
            break;
        }
    }

    void emitUnary(ir::UnaryOpcode op)
    {
        switch (op) {
        case ir::UnaryOpcode::Plus:
            break;
        case ir::UnaryOpcode::Minus:
            clobberRegisterAliases("a0");
            out_ << "  neg a0, a0\n";
            break;
        case ir::UnaryOpcode::Not:
            clobberRegisterAliases("a0");
            out_ << "  seqz a0, a0\n";
            break;
        }
    }

    void emitBinary(const ir::Instruction& inst)
    {
        const auto lhs = inst.operands.at(0);
        const auto rhs = inst.operands.at(1);
        loadOperand(lhs, "t0");

        if (rhs.isImmediate && emitImmediateBinary(inst.binaryOp, rhs.immediate)) {
            storeValue(inst.dst, "a0");
            return;
        }

        loadOperand(rhs, "a0");
        clobberRegisterAliases("a0");
        switch (inst.binaryOp) {
        case ir::BinaryOpcode::Add:
            out_ << "  add a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::Sub:
            out_ << "  sub a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::Mul:
            out_ << "  mul a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::Div:
            out_ << "  div a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::Mod:
            out_ << "  rem a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::Equal:
            out_ << "  sub a0, t0, a0\n";
            out_ << "  seqz a0, a0\n";
            break;
        case ir::BinaryOpcode::NotEqual:
            out_ << "  sub a0, t0, a0\n";
            out_ << "  snez a0, a0\n";
            break;
        case ir::BinaryOpcode::Less:
            out_ << "  slt a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::LessEqual:
            out_ << "  slt a0, a0, t0\n";
            out_ << "  xori a0, a0, 1\n";
            break;
        case ir::BinaryOpcode::Greater:
            out_ << "  slt a0, a0, t0\n";
            break;
        case ir::BinaryOpcode::GreaterEqual:
            out_ << "  slt a0, t0, a0\n";
            out_ << "  xori a0, a0, 1\n";
            break;
        case ir::BinaryOpcode::LogicalAnd:
            out_ << "  snez t0, t0\n";
            out_ << "  snez a0, a0\n";
            out_ << "  and a0, t0, a0\n";
            break;
        case ir::BinaryOpcode::LogicalOr:
            out_ << "  or a0, t0, a0\n";
            out_ << "  snez a0, a0\n";
            break;
        }
        storeValue(inst.dst, "a0");
    }

    bool emitImmediateBinary(ir::BinaryOpcode op, std::int32_t imm)
    {
        switch (op) {
        case ir::BinaryOpcode::Add:
            if (imm >= -2048 && imm <= 2047) {
                clobberRegisterAliases("a0");
                out_ << "  addi a0, t0, " << imm << "\n";
                return true;
            }
            break;
        case ir::BinaryOpcode::Sub:
            if (-imm >= -2048 && -imm <= 2047) {
                clobberRegisterAliases("a0");
                out_ << "  addi a0, t0, " << -imm << "\n";
                return true;
            }
            break;
        case ir::BinaryOpcode::Less:
            if (imm >= -2048 && imm <= 2047) {
                clobberRegisterAliases("a0");
                out_ << "  slti a0, t0, " << imm << "\n";
                return true;
            }
            break;
        case ir::BinaryOpcode::Equal:
            if (imm == 0) {
                clobberRegisterAliases("a0");
                out_ << "  seqz a0, t0\n";
                return true;
            }
            break;
        case ir::BinaryOpcode::NotEqual:
            if (imm == 0) {
                clobberRegisterAliases("a0");
                out_ << "  snez a0, t0\n";
                return true;
            }
            break;
        default:
            break;
        }
        return false;
    }

    void emitCall(const ir::Instruction& inst)
    {
        const int argCount = static_cast<int>(inst.operands.size());
        for (const auto& arg : inst.operands) {
            loadOperand(arg, "a0");
            out_ << "  addi sp, sp, -4\n";
            out_ << "  sw a0, 0(sp)\n";
        }

        const int evalBytes = argCount * 4;
        const int stackArgBytes = std::max(0, argCount - 8) * 4;
        const int reserveBytes = alignTo(evalBytes + stackArgBytes, 16) - evalBytes;
        if (reserveBytes > 0) {
            out_ << "  addi sp, sp, -" << reserveBytes << "\n";
        }

        for (int i = 0; i < std::min(8, argCount); ++i) {
            const int offset = reserveBytes + (argCount - 1 - i) * 4;
            out_ << "  lw a" << i << ", " << offset << "(sp)\n";
        }
        for (int i = 8; i < argCount; ++i) {
            const int sourceOffset = reserveBytes + (argCount - 1 - i) * 4;
            out_ << "  lw t0, " << sourceOffset << "(sp)\n";
            out_ << "  sw t0, " << (i - 8) * 4 << "(sp)\n";
        }

        clobberRegisterAliases("a0");
        out_ << "  call " << inst.symbol << "\n";
        if (reserveBytes + evalBytes > 0) {
            out_ << "  addi sp, sp, " << reserveBytes + evalBytes << "\n";
        }
    }

    void emitTerminator(const ir::Terminator& terminator)
    {
        switch (terminator.kind) {
        case ir::TerminatorKind::Jump:
            out_ << "  j " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::TerminatorKind::Branch:
            if (terminator.condition.isImmediate) {
                out_ << "  j " << blockLabel(terminator.condition.immediate != 0 ? terminator.trueBlock : terminator.falseBlock) << "\n";
                break;
            }
            loadOperand(terminator.condition, "a0");
            out_ << "  bnez a0, " << blockLabel(terminator.trueBlock) << "\n";
            out_ << "  j " << blockLabel(terminator.falseBlock) << "\n";
            break;
        case ir::TerminatorKind::Return:
            if (terminator.hasReturnValue) {
                loadOperand(terminator.returnValue, "a0");
            } else {
                out_ << "  li a0, 0\n";
            }
            out_ << "  j " << returnLabel() << "\n";
            break;
        }
    }

    void loadOperand(const ir::Operand& operand, const std::string& reg)
    {
        if (operand.isImmediate) {
            clobberRegisterAliases(reg);
            out_ << "  li " << reg << ", " << operand.immediate << "\n";
            return;
        }
        loadValue(operand.value, reg, 0);
        consumeValueUse(operand.value);
    }

    void loadValue(ir::Value value, const std::string& reg, int depth)
    {
        if (value.id < 0 || value.id >= static_cast<int>(valueOffsets_.size())) {
            throw IrCodegenError("invalid IR value");
        }
        if (depth < 16) {
            const auto alias = valueAliases_.find(value.id);
            if (alias != valueAliases_.end()) {
                emitAlias(alias->second, reg, depth + 1);
                return;
            }
        }
        clobberRegisterAliases(reg);
        out_ << "  lw " << reg << ", " << valueOffsets_[static_cast<std::size_t>(value.id)] << "(s0)\n";
    }

    void storeValue(ir::Value value, const std::string& reg)
    {
        if (value.id < 0) {
            return;
        }
        if (value.id >= static_cast<int>(valueOffsets_.size())) {
            throw IrCodegenError("invalid IR destination");
        }
        if (valueUseCount(value) == 0) {
            return;
        }
        if (valueUseCount(value) == 1) {
            aliasRegister(value, reg);
            return;
        }
        valueAliases_.erase(value.id);
        out_ << "  sw " << reg << ", " << valueOffsets_[static_cast<std::size_t>(value.id)] << "(s0)\n";
    }

    void emitAlias(const ValueAlias& alias, const std::string& reg, int depth)
    {
        switch (alias.kind) {
        case ValueAlias::Kind::Immediate:
            out_ << "  li " << reg << ", " << alias.immediate << "\n";
            break;
        case ValueAlias::Kind::Value:
            loadValue(alias.value, reg, depth);
            break;
        case ValueAlias::Kind::Local:
            loadLocal(alias.local, reg);
            break;
        case ValueAlias::Kind::Register:
            if (alias.reg != reg) {
                clobberRegisterAliases(reg);
                out_ << "  mv " << reg << ", " << alias.reg << "\n";
            }
            break;
        }
    }

    void aliasImmediate(ir::Value value, std::int32_t immediate)
    {
        if (value.id >= 0 && valueUseCount(value) > 0) {
            valueAliases_[value.id] = ValueAlias{ValueAlias::Kind::Immediate, immediate, {}, {}, {}};
        }
    }

    void aliasOperand(ir::Value value, const ir::Operand& operand)
    {
        if (value.id < 0 || valueUseCount(value) == 0) {
            return;
        }
        if (operand.isImmediate) {
            aliasImmediate(value, operand.immediate);
        } else {
            const auto alias = valueAliases_.find(operand.value.id);
            if (alias != valueAliases_.end()) {
                valueAliases_[value.id] = alias->second;
            } else {
                valueAliases_[value.id] = ValueAlias{ValueAlias::Kind::Value, 0, operand.value, {}, {}};
            }
            consumeValueUse(operand.value);
        }
    }

    void aliasLocal(ir::Value value, const std::string& symbol)
    {
        if (value.id >= 0 && valueUseCount(value) > 0) {
            valueAliases_[value.id] = ValueAlias{ValueAlias::Kind::Local, 0, {}, symbol, {}};
        }
    }

    void aliasRegister(ir::Value value, const std::string& reg)
    {
        if (value.id >= 0 && valueUseCount(value) > 0) {
            valueAliases_[value.id] = ValueAlias{ValueAlias::Kind::Register, 0, {}, {}, reg};
        }
    }

    void clobberRegisterAliases(const std::string& reg)
    {
        std::vector<int> toErase;
        for (const auto& [value, alias] : valueAliases_) {
            if (alias.kind == ValueAlias::Kind::Register && alias.reg == reg) {
                if (value >= 0 && value < static_cast<int>(valueOffsets_.size())) {
                    out_ << "  sw " << reg << ", " << valueOffsets_[static_cast<std::size_t>(value)] << "(s0)\n";
                }
                toErase.push_back(value);
            }
        }
        for (int value : toErase) {
            valueAliases_.erase(value);
        }
    }

    void loadLocal(const std::string& symbol, const std::string& reg)
    {
        const auto found = locals_.find(symbol);
        if (found == locals_.end()) {
            throw IrCodegenError("unknown local: " + symbol);
        }
        const LocalSlot& slot = found->second;
        if (slot.inRegister) {
            clobberRegisterAliases(reg);
            out_ << "  mv " << reg << ", " << savedRegName(slot.reg) << "\n";
        } else {
            clobberRegisterAliases(reg);
            out_ << "  lw " << reg << ", " << slot.offset << "(s0)\n";
        }
    }

    void storeLocal(const std::string& symbol, const std::string& reg)
    {
        const auto found = locals_.find(symbol);
        if (found == locals_.end()) {
            throw IrCodegenError("unknown local: " + symbol);
        }
        const LocalSlot& slot = found->second;
        if (slot.inRegister) {
            out_ << "  mv " << savedRegName(slot.reg) << ", " << reg << "\n";
        } else {
            out_ << "  sw " << reg << ", " << slot.offset << "(s0)\n";
        }
    }

    void countValueUse(const ir::Operand& operand)
    {
        if (!operand.isImmediate && operand.value.id >= 0) {
            ++valueUseCounts_[operand.value.id];
        }
    }

    int valueUseCount(ir::Value value) const
    {
        const auto found = valueUseCounts_.find(value.id);
        return found == valueUseCounts_.end() ? 0 : found->second;
    }

    void consumeValueUse(ir::Value value)
    {
        const auto found = remainingValueUses_.find(value.id);
        if (found == remainingValueUses_.end()) {
            return;
        }
        --found->second;
        if (found->second <= 0) {
            valueAliases_.erase(value.id);
        }
    }

    const ir::Function& function_;
    std::ostream& out_;
    std::unordered_map<std::string, LocalSlot> locals_;
    std::vector<int> valueOffsets_;
    std::unordered_map<int, int> valueUseCounts_;
    std::unordered_map<int, int> remainingValueUses_;
    std::unordered_map<int, ValueAlias> valueAliases_;
    std::vector<int> usedSavedRegs_;
    std::unordered_map<int, int> savedRegOffsets_;
    bool savesRa_ = false;
    int frameSize_ = 16;
    int savedAreaBytes_ = 0;
    int raOffset_ = 0;
    int s0Offset_ = 0;
};

} // namespace

bool IrRiscVCodeGenerator::canGenerate(const ir::Module& module) const
{
    (void)module;
    return true;
}

void IrRiscVCodeGenerator::generate(const ir::Module& module, std::ostream& out) const
{
    out << ".data\n";
    for (const auto& global : module.globals) {
        out << ".globl g_" << sanitizeLabel(global.name) << "\n";
        out << "g_" << sanitizeLabel(global.name) << ":\n";
        out << "  .word " << global.init << "\n";
    }

    out << "\n.text\n";
    for (const auto& function : module.functions) {
        FunctionEmitter(function, out).emit();
    }
}

} // namespace toyc
