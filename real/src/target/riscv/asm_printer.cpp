#include "asm_printer.h"

#include "analysis/liveness.h"
#include "frame.h"
#include "peephole.h"
#include "regalloc.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <optional>
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

bool isCalleeSavedReg(const std::string& reg)
{
    return reg.size() >= 2 && reg[0] == 's' && reg != "sp";
}

struct AssemblyStats {
    std::size_t lines = 0;
    std::size_t lw = 0;
    std::size_t sw = 0;
    std::size_t mv = 0;
    std::size_t j = 0;
    std::size_t addiZero = 0;
};

struct BranchCompare {
    ir::BinaryOpcode op = ir::BinaryOpcode::Equal;
    ir::Operand lhs;
    ir::Operand rhs;
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
        bool hasSelfCall = false;
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
                    hasSelfCall = hasSelfCall || inst.symbol == function_.name;
                    maxCallArgs = std::max(maxCallArgs, static_cast<int>(inst.operands.size()));
                }
            }
        }

        allocateLocalRegs(locals, hasCall, hasEntryBackedge());
        std::unordered_set<std::string> reservedRegs;
        for (const auto& [local, reg] : allocatedLocalRegs_) {
            (void)local;
            reservedRegs.insert(reg);
        }
        collectSkippedLocalUpdateDefs();
        const auto intervals = analysis::computeLocalIntervals(function_);
        allocatedValueRegs_ = RegisterAllocator().allocate(intervals, computeLiveAcrossCalls(intervals), !hasCall, reservedRegs, hasSelfCall ? 1 : -1);
        collectSavedRegs();

        outgoingArgBytes_ = std::max(0, maxCallArgs - 8) * 4;
        savesRa_ = hasCall;
        int nextOffset = outgoingArgBytes_;
        for (const std::string& local : locals) {
            if (allocatedLocalRegs_.find(local) != allocatedLocalRegs_.end()) {
                continue;
            }
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
            const std::string& param = function_.params[static_cast<std::size_t>(i)];
            if (i < 8) {
                storeLocal(param, "a" + std::to_string(i));
            } else {
                const int callerOffset = frameSize_ + (i - 8) * 4;
                loadStack("t0", callerOffset);
                storeLocal(param, "t0");
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

    const std::string* allocatedLocalReg(const std::string& symbol) const
    {
        const auto found = allocatedLocalRegs_.find(symbol);
        if (found == allocatedLocalRegs_.end()) {
            return nullptr;
        }
        return &found->second;
    }

    void loadLocal(const std::string& symbol, const std::string& reg)
    {
        if (const std::string* allocated = allocatedLocalReg(symbol)) {
            if (*allocated != reg) {
                out_ << "  mv " << reg << ", " << *allocated << "\n";
            }
            return;
        }
        loadStack(reg, offsetForLocal(symbol));
    }

    void storeLocal(const std::string& symbol, const std::string& reg)
    {
        if (const std::string* allocated = allocatedLocalReg(symbol)) {
            if (*allocated != reg) {
                out_ << "  mv " << *allocated << ", " << reg << "\n";
            }
            return;
        }
        storeStack(reg, offsetForLocal(symbol));
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
            if (isCalleeSavedReg(reg)) {
                used.insert(reg);
            }
        }
        for (const auto& [local, reg] : allocatedLocalRegs_) {
            (void)local;
            if (isCalleeSavedReg(reg)) {
                used.insert(reg);
            }
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

    bool hasEntryBackedge() const
    {
        for (int i = 1; i < static_cast<int>(function_.blocks.size()); ++i) {
            const ir::Terminator& term = function_.blocks[static_cast<std::size_t>(i)].terminator;
            if (term.kind == ir::TerminatorKind::Jump && term.trueBlock == 0) {
                return true;
            }
            if (term.kind == ir::TerminatorKind::Branch && (term.trueBlock == 0 || term.falseBlock == 0)) {
                return true;
            }
        }
        return false;
    }

    void allocateLocalRegs(const std::unordered_set<std::string>& locals, bool hasCall, bool hasLoop)
    {
        // Only skip when there is no loop and the function takes parameters (the
        // params are cheap to keep on the stack there). Functions that contain
        // calls are fine: locals go into callee-saved registers, which are saved
        // in the prologue / restored in the epilogue and therefore survive calls.
        if (!hasLoop && !function_.params.empty()) {
            return;
        }

        std::unordered_map<std::string, int> weights;
        for (const ir::BasicBlock& block : function_.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                if ((inst.kind == ir::InstructionKind::LoadLocal || inst.kind == ir::InstructionKind::StoreLocal) && !inst.symbol.empty()) {
                    weights[inst.symbol] += 8;
                }
            }
        }
        for (const std::string& param : function_.params) {
            if (locals.find(param) != locals.end()) {
                ++weights[param];
            }
        }

        std::vector<std::pair<std::string, int>> ordered;
        ordered.reserve(weights.size());
        for (const auto& [symbol, weight] : weights) {
            if (weight > 0) {
                ordered.emplace_back(symbol, weight);
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        });

        // With no calls, values live in caller-saved temps and never need
        // callee-saved registers, so hot locals may use the whole callee-saved
        // bank. With calls, leave s7-s11 for values that must survive a call.
        static constexpr const char* regsNoCall[] = {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
        static constexpr const char* regsWithCall[] = {"s1", "s2", "s3", "s4", "s5", "s6"};
        const char* const* regs = hasCall ? regsWithCall : regsNoCall;
        const std::size_t regCount = hasCall ? std::size(regsWithCall) : std::size(regsNoCall);
        const std::size_t count = std::min<std::size_t>(ordered.size(), regCount);
        for (std::size_t i = 0; i < count; ++i) {
            allocatedLocalRegs_[ordered[i].first] = regs[i];
        }
    }

    bool emitLocalAddUpdate(const ir::Instruction& inst)
    {
        if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol.empty() || inst.operands.empty()) {
            return false;
        }
        const std::string* targetReg = allocatedLocalReg(inst.symbol);
        if (targetReg == nullptr || inst.operands[0].isImmediate) {
            return false;
        }

        const ir::Value value = inst.operands[0].value;
        for (const ir::BasicBlock& block : function_.blocks) {
            for (std::size_t defIndex = 0; defIndex < block.instructions.size(); ++defIndex) {
                const ir::Instruction& def = block.instructions[defIndex];
                if (def.dst != value || def.kind != ir::InstructionKind::Binary || def.operands.size() != 2) {
                    continue;
                }
                const auto storeIndex = findInstructionIndex(block, inst);
                if (!storeIndex.has_value() || defIndex >= *storeIndex) {
                    return false;
                }
                const bool lhsSelf = !def.operands[0].isImmediate
                    && def.operands[0].value.id >= 0
                    && isCurrentLoadOfLocal(block, def.operands[0].value, inst.symbol, defIndex, *storeIndex);
                const bool rhsSelf = !def.operands[1].isImmediate
                    && def.operands[1].value.id >= 0
                    && isCurrentLoadOfLocal(block, def.operands[1].value, inst.symbol, defIndex, *storeIndex);
                if (def.binaryOp == ir::BinaryOpcode::Add && lhsSelf) {
                    if (def.operands[1].isImmediate && fitsI12(def.operands[1].immediate)) {
                        out_ << "  addi " << *targetReg << ", " << *targetReg << ", " << def.operands[1].immediate << "\n";
                        return true;
                    }
                    const std::string rhs = readOperand(def.operands[1], "t0");
                    out_ << "  add " << *targetReg << ", " << *targetReg << ", " << rhs << "\n";
                    return true;
                }
                if (def.binaryOp == ir::BinaryOpcode::Add && rhsSelf) {
                    if (def.operands[0].isImmediate && fitsI12(def.operands[0].immediate)) {
                        out_ << "  addi " << *targetReg << ", " << *targetReg << ", " << def.operands[0].immediate << "\n";
                        return true;
                    }
                    const std::string lhs = readOperand(def.operands[0], "t0");
                    out_ << "  add " << *targetReg << ", " << *targetReg << ", " << lhs << "\n";
                    return true;
                }
                if (def.binaryOp == ir::BinaryOpcode::Sub && lhsSelf) {
                    if (def.operands[1].isImmediate) {
                        const long long negated = -static_cast<long long>(def.operands[1].immediate);
                        if (negated >= -2048 && negated <= 2047) {
                            out_ << "  addi " << *targetReg << ", " << *targetReg << ", " << negated << "\n";
                            return true;
                        }
                    }
                    const std::string rhs = readOperand(def.operands[1], "t0");
                    out_ << "  sub " << *targetReg << ", " << *targetReg << ", " << rhs << "\n";
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    std::optional<std::size_t> findInstructionIndex(const ir::BasicBlock& block, const ir::Instruction& needle) const
    {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            if (&block.instructions[i] == &needle) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool isCurrentLoadOfLocal(
        const ir::BasicBlock& block,
        ir::Value value,
        const std::string& symbol,
        std::size_t defIndex,
        std::size_t storeIndex) const
    {
        std::optional<std::size_t> loadIndex;
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const ir::Instruction& inst = block.instructions[i];
            if (inst.dst == value && inst.kind == ir::InstructionKind::LoadLocal && inst.symbol == symbol) {
                loadIndex = i;
                break;
            }
        }
        if (!loadIndex.has_value() || *loadIndex >= defIndex) {
            return false;
        }
        for (std::size_t i = *loadIndex + 1; i < storeIndex; ++i) {
            const ir::Instruction& inst = block.instructions[i];
            if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol == symbol) {
                return false;
            }
        }
        return true;
    }

    void collectSkippedLocalUpdateDefs()
    {
        std::unordered_map<int, int> useCounts;
        for (const ir::BasicBlock& block : function_.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                for (const ir::Operand& operand : inst.operands) {
                    if (!operand.isImmediate && operand.value.id >= 0) {
                        ++useCounts[operand.value.id];
                    }
                }
            }
            if (!block.terminator.condition.isImmediate && block.terminator.condition.value.id >= 0) {
                ++useCounts[block.terminator.condition.value.id];
            }
            if (!block.terminator.returnValue.isImmediate && block.terminator.returnValue.value.id >= 0) {
                ++useCounts[block.terminator.returnValue.value.id];
            }
        }

        for (const ir::BasicBlock& block : function_.blocks) {
            for (std::size_t storeIndex = 0; storeIndex < block.instructions.size(); ++storeIndex) {
                const ir::Instruction& store = block.instructions[storeIndex];
                if (store.kind != ir::InstructionKind::StoreLocal
                    || allocatedLocalReg(store.symbol) == nullptr
                    || store.operands.empty()
                    || store.operands[0].isImmediate) {
                    continue;
                }
                const ir::Value result = store.operands[0].value;
                if (useCounts[result.id] != 1) {
                    continue;
                }
                for (std::size_t defIndex = 0; defIndex < storeIndex; ++defIndex) {
                    const ir::Instruction& def = block.instructions[defIndex];
                    if (def.dst != result || def.kind != ir::InstructionKind::Binary || def.operands.size() != 2) {
                        continue;
                    }
                    // Only skip defs that emitLocalAddUpdate will actually emit
                    // in place: `L = L + X` (either operand is the current load of
                    // L) or `L = L - X` (L on the left). Skipping any other op
                    // (e.g. `L = L * 2`) would drop the computation entirely,
                    // since emitLocalAddUpdate refuses it and the generic store
                    // path then reads a never-defined value.
                    auto selfLoad = [&](const ir::Operand& operand) {
                        return !operand.isImmediate
                            && operand.value.id >= 0
                            && useCounts[operand.value.id] == 1
                            && isCurrentLoadOfLocal(block, operand.value, store.symbol, defIndex, storeIndex);
                    };
                    const bool lhsSelf = selfLoad(def.operands[0]);
                    const bool rhsSelf = selfLoad(def.operands[1]);
                    int selfOperand = -1;
                    if (def.binaryOp == ir::BinaryOpcode::Add && lhsSelf) {
                        selfOperand = 0;
                    } else if (def.binaryOp == ir::BinaryOpcode::Add && rhsSelf) {
                        selfOperand = 1;
                    } else if (def.binaryOp == ir::BinaryOpcode::Sub && lhsSelf) {
                        selfOperand = 0;
                    }
                    if (selfOperand >= 0) {
                        skippedValueDefs_.insert(def.operands[static_cast<std::size_t>(selfOperand)].value.id);
                        skippedValueDefs_.insert(def.dst.id);
                    }
                    break;
                }
            }
        }

        collectBranchCompareDefs(useCounts);
    }

    bool isBranchCompareOpcode(ir::BinaryOpcode op) const
    {
        switch (op) {
        case ir::BinaryOpcode::Equal:
        case ir::BinaryOpcode::NotEqual:
        case ir::BinaryOpcode::Less:
        case ir::BinaryOpcode::LessEqual:
        case ir::BinaryOpcode::Greater:
        case ir::BinaryOpcode::GreaterEqual:
            return true;
        case ir::BinaryOpcode::Add:
        case ir::BinaryOpcode::Sub:
        case ir::BinaryOpcode::Mul:
        case ir::BinaryOpcode::Div:
        case ir::BinaryOpcode::Mod:
        case ir::BinaryOpcode::LogicalAnd:
        case ir::BinaryOpcode::LogicalOr:
            return false;
        }
        return false;
    }

    void collectBranchCompareDefs(const std::unordered_map<int, int>& useCounts)
    {
        for (const ir::BasicBlock& block : function_.blocks) {
            if (block.terminator.kind != ir::TerminatorKind::Branch
                || block.terminator.condition.isImmediate
                || block.terminator.condition.value.id < 0) {
                continue;
            }

            const ir::Value condition = block.terminator.condition.value;
            const auto useCount = useCounts.find(condition.id);
            if (useCount == useCounts.end() || useCount->second != 1) {
                continue;
            }

            for (const ir::Instruction& inst : block.instructions) {
                if (inst.dst != condition
                    || inst.kind != ir::InstructionKind::Binary
                    || inst.operands.size() != 2
                    || !isBranchCompareOpcode(inst.binaryOp)) {
                    continue;
                }
                branchCompareDefs_[condition.id] = BranchCompare{inst.binaryOp, inst.operands[0], inst.operands[1]};
                skippedValueDefs_.insert(condition.id);
                break;
            }
        }
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
        if (inst.dst.id >= 0 && skippedValueDefs_.find(inst.dst.id) != skippedValueDefs_.end()) {
            return;
        }
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
            if (!emitLocalAddUpdate(inst)) {
                storeLocal(inst.symbol, readOperand(inst.operands[0], "t0"));
            }
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
        loadLocal(inst.symbol, dstReg);
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

        const std::int32_t absDivisor = divisor < 0 ? -divisor : divisor;
        const DivMagic magic = computePositiveDivMagic(absDivisor);
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
        if (divisor < 0) {
            out_ << "  neg " << dstReg << ", " << dstReg << "\n";
        }

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
                if (!terminator.condition.isImmediate && terminator.condition.value.id >= 0) {
                    const auto found = branchCompareDefs_.find(terminator.condition.value.id);
                    if (found != branchCompareDefs_.end()) {
                        emitBranchCompare(found->second, terminator.falseBlock, terminator.trueBlock);
                        break;
                    }
                }
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

    void emitBranchCompare(const BranchCompare& compare, int falseBlock, int trueBlock)
    {
        const std::string lhsReg = readOperand(compare.lhs, "t0");
        const std::string rhsReg = readOperand(compare.rhs, lhsReg == "t1" ? "t2" : "t1");
        switch (compare.op) {
        case ir::BinaryOpcode::Equal:
            out_ << "  bne " << lhsReg << ", " << rhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::NotEqual:
            out_ << "  beq " << lhsReg << ", " << rhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::Less:
            out_ << "  bge " << lhsReg << ", " << rhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::LessEqual:
            out_ << "  blt " << rhsReg << ", " << lhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::Greater:
            out_ << "  bge " << rhsReg << ", " << lhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::GreaterEqual:
            out_ << "  blt " << lhsReg << ", " << rhsReg << ", " << labelFor(falseBlock) << "\n";
            break;
        case ir::BinaryOpcode::Add:
        case ir::BinaryOpcode::Sub:
        case ir::BinaryOpcode::Mul:
        case ir::BinaryOpcode::Div:
        case ir::BinaryOpcode::Mod:
        case ir::BinaryOpcode::LogicalAnd:
        case ir::BinaryOpcode::LogicalOr:
            throw std::runtime_error("non-compare opcode in branch compare");
        }
        out_ << "  j " << labelFor(trueBlock) << "\n";
    }

    const ir::Function& function_;
    const std::unordered_map<std::string, std::string>& globals_;
    std::ostream& out_;
    std::unordered_map<std::string, int> localOffsets_;
    std::unordered_map<int, int> valueOffsets_;
    std::unordered_map<int, std::string> allocatedValueRegs_;
    std::unordered_map<std::string, std::string> allocatedLocalRegs_;
    std::unordered_set<int> skippedValueDefs_;
    std::unordered_map<int, BranchCompare> branchCompareDefs_;
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
