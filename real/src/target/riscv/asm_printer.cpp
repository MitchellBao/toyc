#include "asm_printer.h"

#include "analysis/liveness.h"
#include "frame.h"
#include "peephole.h"
#include "regalloc.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
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

bool isPositivePowerOfTwo(std::int32_t value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

struct DivMagic {
    std::int32_t multiplier = 0;
    int shift = 0;
    bool addDividend = false;
};

DivMagic computePositiveDivMagic(std::int32_t divisor)
{
    const std::uint32_t twoP31 = 0x80000000u;
    const std::uint32_t ad = static_cast<std::uint32_t>(divisor);
    const std::uint32_t anc = twoP31 - 1 - (twoP31 - 1) % ad;
    int p = 31;
    std::uint32_t q1 = twoP31 / anc;
    std::uint32_t r1 = twoP31 - q1 * anc;
    std::uint32_t q2 = twoP31 / ad;
    std::uint32_t r2 = twoP31 - q2 * ad;
    std::uint32_t delta = 0;
    do {
        ++p;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) {
            ++q1;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) {
            ++q2;
            r2 -= ad;
        }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));

    DivMagic result;
    result.multiplier = static_cast<std::int32_t>(q2 + 1);
    result.shift = p - 32;
    result.addDividend = result.multiplier < 0;
    return result;
}

int log2PowerOfTwo(std::int32_t value)
{
    int shift = 0;
    while ((std::int32_t{1} << shift) != value) {
        ++shift;
    }
    return shift;
}

bool definesValue(const ir::Instruction& inst)
{
    return inst.dst.id >= 0
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal;
}

struct AssemblyStats {
    std::size_t lines = 0;
    std::size_t lw = 0;
    std::size_t sw = 0;
    std::size_t mv = 0;
    std::size_t j = 0;
    std::size_t addiZero = 0;
};

std::string trim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool startsWithOpcode(const std::string& line, const std::string& opcode)
{
    return line == opcode || line.rfind(opcode + " ", 0) == 0 || line.rfind(opcode + "\t", 0) == 0;
}

AssemblyStats measureAssembly(const std::string& assembly)
{
    AssemblyStats stats;
    std::istringstream in(assembly);
    std::string line;
    while (std::getline(in, line)) {
        const std::string text = trim(line);
        if (text.empty()) {
            continue;
        }
        ++stats.lines;
        if (startsWithOpcode(text, "lw")) {
            ++stats.lw;
        } else if (startsWithOpcode(text, "sw")) {
            ++stats.sw;
        } else if (startsWithOpcode(text, "mv")) {
            ++stats.mv;
        } else if (startsWithOpcode(text, "j")) {
            ++stats.j;
        } else if (startsWithOpcode(text, "addi")) {
            const std::size_t lastComma = text.find_last_of(',');
            if (lastComma != std::string::npos && trim(text.substr(lastComma + 1)) == "0") {
                ++stats.addiZero;
            }
        }
    }
    return stats;
}

void printAssemblyStats(std::ostream& out, const AssemblyStats& before, const AssemblyStats& after)
{
    out << "[toycc] stage=backend-peephole"
        << " asm_lines=" << before.lines << "->" << after.lines
        << " lw=" << before.lw << "->" << after.lw
        << " sw=" << before.sw << "->" << after.sw
        << " mv=" << before.mv << "->" << after.mv
        << " j=" << before.j << "->" << after.j
        << " addi0=" << before.addiZero << "->" << after.addiZero
        << '\n';
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
                    ++valueCount;
                }
                if (inst.kind == ir::InstructionKind::Call) {
                    hasCall = true;
                    maxCallArgs = std::max(maxCallArgs, static_cast<int>(inst.operands.size()));
                }
            }
        }

        const auto intervals = analysis::computeLocalIntervals(function_);
        allocatedValueRegs_ = RegisterAllocator().allocate(intervals, computeLiveAcrossCalls(intervals), !hasCall);
        collectSavedRegs();

        outgoingArgBytes_ = std::max(0, maxCallArgs - 8) * 4;
        savesRa_ = hasCall;
        int nextOffset = outgoingArgBytes_;
        for (const std::string& local : locals) {
            localOffsets_[local] = nextOffset;
            nextOffset += 4;
        }
        for (const ir::BasicBlock& block : function_.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                if (definesValue(inst) && allocatedValueRegs_.find(inst.dst.id) == allocatedValueRegs_.end()) {
                    valueOffsets_[inst.dst.id] = nextOffset;
                    nextOffset += 4;
                }
            }
        }

        for (const std::string& reg : savedRegs_) {
            savedRegOffsets_[reg] = nextOffset;
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
            adjustStack(-frameSize_);
        }
        if (savesRa_) {
            storeStack("ra", raOffset_);
        }
        for (const std::string& reg : savedRegs_) {
            storeStack(reg, savedRegOffsets_.at(reg));
        }
    }

    void emitEpilogue()
    {
        for (auto iter = savedRegs_.rbegin(); iter != savedRegs_.rend(); ++iter) {
            loadStack(*iter, savedRegOffsets_.at(*iter));
        }
        if (savesRa_) {
            loadStack("ra", raOffset_);
        }
        if (frameSize_ > 0) {
            adjustStack(frameSize_);
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
                storeStack("a" + std::to_string(i), found->second);
            } else {
                const int callerOffset = frameSize_ + (i - 8) * 4;
                loadStack("t0", callerOffset);
                storeStack("t0", found->second);
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

    const std::string* allocatedReg(ir::Value value) const
    {
        const auto found = allocatedValueRegs_.find(value.id);
        if (found == allocatedValueRegs_.end()) {
            return nullptr;
        }
        return &found->second;
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
            if (const std::string* allocated = allocatedReg(operand.value)) {
                if (*allocated != reg) {
                    out_ << "  mv " << reg << ", " << *allocated << "\n";
                }
                return;
            }
            loadStack(reg, offsetForValue(operand.value));
        }
    }

    std::string readOperand(const ir::Operand& operand, const std::string& scratch)
    {
        if (!operand.isImmediate) {
            if (const std::string* allocated = allocatedReg(operand.value)) {
                return *allocated;
            }
        }
        loadOperand(operand, scratch);
        return scratch;
    }

    std::string writeReg(ir::Value value, const std::string& fallback) const
    {
        if (const std::string* allocated = allocatedReg(value)) {
            return *allocated;
        }
        return fallback;
    }

    void storeValue(ir::Value value, const std::string& reg)
    {
        if (const std::string* allocated = allocatedReg(value)) {
            if (*allocated != reg) {
                out_ << "  mv " << *allocated << ", " << reg << "\n";
            }
            return;
        }
        storeStack(reg, offsetForValue(value));
    }

    void collectSavedRegs()
    {
        static constexpr const char* orderedRegs[] = {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
        std::unordered_set<std::string> used;
        for (const auto& [value, reg] : allocatedValueRegs_) {
            (void)value;
            used.insert(reg);
        }
        for (const char* reg : orderedRegs) {
            if (used.find(reg) != used.end()) {
                savedRegs_.push_back(reg);
            }
        }
    }

    std::unordered_set<int> computeLiveAcrossCalls(const std::unordered_map<int, analysis::LiveInterval>& intervals) const
    {
        std::unordered_set<int> liveAcrossCalls;
        int index = 0;
        for (const ir::BasicBlock& block : function_.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                if (inst.kind == ir::InstructionKind::Call) {
                    for (const auto& [value, interval] : intervals) {
                        if (interval.start < index && index < interval.end) {
                            liveAcrossCalls.insert(value);
                        }
                    }
                }
                ++index;
            }
            ++index;
        }
        return liveAcrossCalls;
    }

    void adjustStack(int amount)
    {
        if (fitsI12(amount)) {
            out_ << "  addi sp, sp, " << amount << "\n";
            return;
        }
        out_ << "  li t6, " << (amount < 0 ? -amount : amount) << "\n";
        if (amount < 0) {
            out_ << "  sub sp, sp, t6\n";
        } else {
            out_ << "  add sp, sp, t6\n";
        }
    }

    void loadStack(const std::string& reg, int offset)
    {
        if (fitsI12(offset)) {
            out_ << "  lw " << reg << ", " << offset << "(sp)\n";
            return;
        }
        out_ << "  li t6, " << offset << "\n";
        out_ << "  add t6, sp, t6\n";
        out_ << "  lw " << reg << ", 0(t6)\n";
    }

    void storeStack(const std::string& reg, int offset)
    {
        if (fitsI12(offset)) {
            out_ << "  sw " << reg << ", " << offset << "(sp)\n";
            return;
        }
        out_ << "  li t6, " << offset << "\n";
        out_ << "  add t6, sp, t6\n";
        out_ << "  sw " << reg << ", 0(t6)\n";
    }

    void emitInstruction(const ir::Instruction& inst)
    {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            emitCopyLike(inst.dst, inst.operands.empty() ? ir::Operand::imm(0) : inst.operands[0]);
            return;
        case ir::InstructionKind::Copy:
            emitCopyLike(inst.dst, inst.operands[0]);
            return;
        case ir::InstructionKind::LoadLocal:
            emitLoadLocal(inst);
            return;
        case ir::InstructionKind::StoreLocal:
            storeStack(readOperand(inst.operands[0], "t0"), offsetForLocal(inst.symbol));
            return;
        case ir::InstructionKind::LoadGlobal:
            emitLoadGlobal(inst);
            return;
        case ir::InstructionKind::StoreGlobal:
            {
            const std::string valueReg = readOperand(inst.operands[0], "t0");
            out_ << "  la t1, " << globalLabel(inst.symbol) << "\n";
            out_ << "  sw " << valueReg << ", 0(t1)\n";
            return;
            }
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

    void emitCopyLike(ir::Value dst, const ir::Operand& source)
    {
        const std::string dstReg = writeReg(dst, "t0");
        loadOperand(source, dstReg);
        storeValue(dst, dstReg);
    }

    void emitLoadLocal(const ir::Instruction& inst)
    {
        const std::string dstReg = writeReg(inst.dst, "t0");
        loadStack(dstReg, offsetForLocal(inst.symbol));
        storeValue(inst.dst, dstReg);
    }

    void emitLoadGlobal(const ir::Instruction& inst)
    {
        const std::string dstReg = writeReg(inst.dst, "t0");
        out_ << "  la " << dstReg << ", " << globalLabel(inst.symbol) << "\n";
        out_ << "  lw " << dstReg << ", 0(" << dstReg << ")\n";
        storeValue(inst.dst, dstReg);
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
        const std::string dstReg = writeReg(inst.dst, "t0");
        loadOperand(inst.operands[0], dstReg);
        switch (inst.unaryOp) {
        case ir::UnaryOpcode::Plus:
            break;
        case ir::UnaryOpcode::Minus:
            out_ << "  neg " << dstReg << ", " << dstReg << "\n";
            break;
        case ir::UnaryOpcode::Not:
            out_ << "  seqz " << dstReg << ", " << dstReg << "\n";
            break;
        }
        storeValue(inst.dst, dstReg);
    }

    void emitBinary(const ir::Instruction& inst)
    {
        if (emitBinaryWithImmediate(inst)) {
            return;
        }

        const std::string dstReg = writeReg(inst.dst, "t0");
        const std::string lhsReg = readOperand(inst.operands[0], "t0");
        const std::string rhsReg = readOperand(inst.operands[1], lhsReg == "t1" ? "t2" : "t1");
        switch (inst.binaryOp) {
        case ir::BinaryOpcode::Add:
            out_ << "  add " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Sub:
            out_ << "  sub " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Mul:
            out_ << "  mul " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Div:
            out_ << "  div " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Mod:
            out_ << "  rem " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Equal:
            out_ << "  sub " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            out_ << "  seqz " << dstReg << ", " << dstReg << "\n";
            break;
        case ir::BinaryOpcode::NotEqual:
            out_ << "  sub " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            out_ << "  snez " << dstReg << ", " << dstReg << "\n";
            break;
        case ir::BinaryOpcode::Less:
            out_ << "  slt " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            break;
        case ir::BinaryOpcode::Greater:
            out_ << "  slt " << dstReg << ", " << rhsReg << ", " << lhsReg << "\n";
            break;
        case ir::BinaryOpcode::LessEqual:
            out_ << "  slt " << dstReg << ", " << rhsReg << ", " << lhsReg << "\n";
            out_ << "  xori " << dstReg << ", " << dstReg << ", 1\n";
            break;
        case ir::BinaryOpcode::GreaterEqual:
            out_ << "  slt " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            out_ << "  xori " << dstReg << ", " << dstReg << ", 1\n";
            break;
        case ir::BinaryOpcode::LogicalAnd:
            out_ << "  snez t0, " << lhsReg << "\n";
            out_ << "  snez t1, " << rhsReg << "\n";
            out_ << "  and " << dstReg << ", t0, t1\n";
            break;
        case ir::BinaryOpcode::LogicalOr:
            out_ << "  or " << dstReg << ", " << lhsReg << ", " << rhsReg << "\n";
            out_ << "  snez " << dstReg << ", " << dstReg << "\n";
            break;
        }
        storeValue(inst.dst, dstReg);
    }

    bool emitDivModByConstant(const ir::Instruction& inst, std::int32_t divisor, bool isModulo)
    {
        if (divisor == 0) {
            return false;
        }

        const std::string dstReg = writeReg(inst.dst, "t0");
        loadOperand(inst.operands[0], "t2");

        auto finish = [&]() {
            storeValue(inst.dst, dstReg);
            return true;
        };

        if (divisor == 1 || divisor == -1) {
            if (isModulo) {
                out_ << "  li " << dstReg << ", 0\n";
            } else if (divisor == 1) {
                out_ << "  mv " << dstReg << ", t2\n";
            } else {
                out_ << "  neg " << dstReg << ", t2\n";
            }
            return finish();
        }

        if (isPositivePowerOfTwo(divisor)) {
            const int shift = log2PowerOfTwo(divisor);
            out_ << "  srai t1, t2, 31\n";
            out_ << "  srli t1, t1, " << (32 - shift) << "\n";
            out_ << "  add " << dstReg << ", t2, t1\n";
            out_ << "  srai " << dstReg << ", " << dstReg << ", " << shift << "\n";
            if (isModulo) {
                out_ << "  slli t1, " << dstReg << ", " << shift << "\n";
                out_ << "  sub " << dstReg << ", t2, t1\n";
            }
            return finish();
        }

        if (divisor < 0) {
            return false;
        }

        const DivMagic magic = computePositiveDivMagic(divisor);
        out_ << "  li t1, " << magic.multiplier << "\n";
        out_ << "  mulh " << dstReg << ", t2, t1\n";
        if (magic.addDividend) {
            out_ << "  add " << dstReg << ", " << dstReg << ", t2\n";
        }
        if (magic.shift > 0) {
            out_ << "  srai " << dstReg << ", " << dstReg << ", " << magic.shift << "\n";
        }
        out_ << "  srli t1, t2, 31\n";
        out_ << "  add " << dstReg << ", " << dstReg << ", t1\n";

        if (isModulo) {
            out_ << "  li t1, " << divisor << "\n";
            out_ << "  mul t1, " << dstReg << ", t1\n";
            out_ << "  sub " << dstReg << ", t2, t1\n";
        }
        return finish();
    }

    bool emitBinaryWithImmediate(const ir::Instruction& inst)
    {
        if (inst.operands.size() != 2) {
            return false;
        }

        const ir::Operand& lhs = inst.operands[0];
        const ir::Operand& rhs = inst.operands[1];
        const bool lhsImm = lhs.isImmediate;
        const bool rhsImm = rhs.isImmediate;
        if (!lhsImm && !rhsImm) {
            return false;
        }

        const std::string dstReg = writeReg(inst.dst, "t0");
        auto loadNonImmediate = [&](const ir::Operand& operand, const std::string& fallback) {
            return readOperand(operand, fallback);
        };
        auto finish = [&]() {
            storeValue(inst.dst, dstReg);
            return true;
        };

        if (rhsImm && !lhsImm) {
            const std::int32_t value = rhs.immediate;
            switch (inst.binaryOp) {
            case ir::BinaryOpcode::Add:
                if (fitsI12(value)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  addi " << dstReg << ", " << src << ", " << value << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Sub:
                {
                const long long negated = -static_cast<long long>(value);
                if (negated >= -2048 && negated <= 2047) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  addi " << dstReg << ", " << src << ", " << negated << "\n";
                    return finish();
                }
                return false;
                }
            case ir::BinaryOpcode::Mul:
                if (value == 0) {
                    out_ << "  li " << dstReg << ", 0\n";
                    return finish();
                }
                if (value == 1) {
                    loadOperand(lhs, dstReg);
                    return finish();
                }
                if (value == -1) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  neg " << dstReg << ", " << src << "\n";
                    return finish();
                }
                if (isPositivePowerOfTwo(value)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  slli " << dstReg << ", " << src << ", " << log2PowerOfTwo(value) << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Equal:
                if (value == 0) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  seqz " << dstReg << ", " << src << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::NotEqual:
                if (value == 0) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  snez " << dstReg << ", " << src << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Less:
                if (fitsI12(value)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  slti " << dstReg << ", " << src << ", " << value << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::LessEqual:
                if (value < 2147483647 && fitsI12(value + 1)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  slti " << dstReg << ", " << src << ", " << (value + 1) << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Greater:
                if (value < 2147483647 && fitsI12(value + 1)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  slti " << dstReg << ", " << src << ", " << (value + 1) << "\n";
                    out_ << "  xori " << dstReg << ", " << dstReg << ", 1\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::GreaterEqual:
                if (fitsI12(value)) {
                    const std::string src = loadNonImmediate(lhs, dstReg);
                    out_ << "  slti " << dstReg << ", " << src << ", " << value << "\n";
                    out_ << "  xori " << dstReg << ", " << dstReg << ", 1\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Div:
                return emitDivModByConstant(inst, value, false);
            case ir::BinaryOpcode::Mod:
                return emitDivModByConstant(inst, value, true);
            case ir::BinaryOpcode::LogicalAnd:
            case ir::BinaryOpcode::LogicalOr:
                return false;
            }
        }

        if (lhsImm && !rhsImm) {
            const std::int32_t value = lhs.immediate;
            switch (inst.binaryOp) {
            case ir::BinaryOpcode::Add:
                if (fitsI12(value)) {
                    const std::string src = loadNonImmediate(rhs, dstReg);
                    out_ << "  addi " << dstReg << ", " << src << ", " << value << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Mul:
                if (value == 0) {
                    out_ << "  li " << dstReg << ", 0\n";
                    return finish();
                }
                if (value == 1) {
                    loadOperand(rhs, dstReg);
                    return finish();
                }
                if (value == -1) {
                    const std::string src = loadNonImmediate(rhs, dstReg);
                    out_ << "  neg " << dstReg << ", " << src << "\n";
                    return finish();
                }
                if (isPositivePowerOfTwo(value)) {
                    const std::string src = loadNonImmediate(rhs, dstReg);
                    out_ << "  slli " << dstReg << ", " << src << ", " << log2PowerOfTwo(value) << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Equal:
                if (value == 0) {
                    const std::string src = loadNonImmediate(rhs, dstReg);
                    out_ << "  seqz " << dstReg << ", " << src << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::NotEqual:
                if (value == 0) {
                    const std::string src = loadNonImmediate(rhs, dstReg);
                    out_ << "  snez " << dstReg << ", " << src << "\n";
                    return finish();
                }
                return false;
            case ir::BinaryOpcode::Sub:
            case ir::BinaryOpcode::Div:
            case ir::BinaryOpcode::Mod:
            case ir::BinaryOpcode::Less:
            case ir::BinaryOpcode::LessEqual:
            case ir::BinaryOpcode::Greater:
            case ir::BinaryOpcode::GreaterEqual:
            case ir::BinaryOpcode::LogicalAnd:
            case ir::BinaryOpcode::LogicalOr:
                return false;
            }
        }

        return false;
    }

    void emitCall(const ir::Instruction& inst)
    {
        for (int i = static_cast<int>(inst.operands.size()) - 1; i >= 8; --i) {
            storeStack(readOperand(inst.operands[static_cast<std::size_t>(i)], "t0"), (i - 8) * 4);
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
            {
                const std::string conditionReg = readOperand(terminator.condition, "t0");
                out_ << "  beqz " << conditionReg << ", " << labelFor(terminator.falseBlock) << "\n";
                out_ << "  j " << labelFor(terminator.trueBlock) << "\n";
                break;
            }
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
    std::unordered_map<int, std::string> allocatedValueRegs_;
    std::unordered_map<std::string, int> savedRegOffsets_;
    std::vector<std::string> savedRegs_;
    int outgoingArgBytes_ = 0;
    int frameSize_ = 0;
    int raOffset_ = 0;
    bool savesRa_ = false;
};

} // namespace

void AsmPrinter::print(const ir::Module& module, std::ostream& out, std::ostream* statsOut) const
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

    const std::string beforePeephole = buffer.str();
    const std::string afterPeephole = peephole(beforePeephole);
    if (statsOut != nullptr) {
        printAssemblyStats(*statsOut, measureAssembly(beforePeephole), measureAssembly(afterPeephole));
    }
    out << afterPeephole;
}

} // namespace toyc::riscv
