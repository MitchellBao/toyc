#include "ir_codegen.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
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

struct ValueInterval {
    int value = -1;
    int start = 0;
    int end = 0;
    int weight = 0;
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

std::string trim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

bool isLabelLine(const std::string& line)
{
    return !line.empty() && line.back() == ':';
}

bool parseRegisterPair(const std::string& line, const std::string& opcode, std::string& lhs, std::string& rhs)
{
    const std::string prefix = opcode + " ";
    if (!startsWith(line, prefix)) {
        return false;
    }
    const std::size_t comma = line.find(',', prefix.size());
    if (comma == std::string::npos) {
        return false;
    }
    lhs = trim(line.substr(prefix.size(), comma - prefix.size()));
    rhs = trim(line.substr(comma + 1));
    return !lhs.empty() && !rhs.empty();
}

bool parseJump(const std::string& line, std::string& label)
{
    if (!startsWith(line, "j ")) {
        return false;
    }
    label = trim(line.substr(2));
    return !label.empty();
}

bool parseLi(const std::string& line, std::string& reg, std::string& imm)
{
    return parseRegisterPair(line, "li", reg, imm);
}

bool parseThreeRegister(const std::string& line, const std::string& opcode, std::string& dst, std::string& lhs, std::string& rhs)
{
    const std::string prefix = opcode + " ";
    if (!startsWith(line, prefix)) {
        return false;
    }
    const std::size_t firstComma = line.find(',', prefix.size());
    if (firstComma == std::string::npos) {
        return false;
    }
    const std::size_t secondComma = line.find(',', firstComma + 1);
    if (secondComma == std::string::npos) {
        return false;
    }
    dst = trim(line.substr(prefix.size(), firstComma - prefix.size()));
    lhs = trim(line.substr(firstComma + 1, secondComma - firstComma - 1));
    rhs = trim(line.substr(secondComma + 1));
    return !dst.empty() && !lhs.empty() && !rhs.empty();
}

bool parseImmediateBinary(const std::string& line, const std::string& opcode, std::string& dst, std::string& src, std::string& imm)
{
    const std::string prefix = opcode + " ";
    if (!startsWith(line, prefix)) {
        return false;
    }
    const std::size_t firstComma = line.find(',', prefix.size());
    if (firstComma == std::string::npos) {
        return false;
    }
    const std::size_t secondComma = line.find(',', firstComma + 1);
    if (secondComma == std::string::npos) {
        return false;
    }
    dst = trim(line.substr(prefix.size(), firstComma - prefix.size()));
    src = trim(line.substr(firstComma + 1, secondComma - firstComma - 1));
    imm = trim(line.substr(secondComma + 1));
    return !dst.empty() && !src.empty() && !imm.empty();
}

bool parseStoreWord(const std::string& line, std::string& src, std::string& address)
{
    return parseRegisterPair(line, "sw", src, address);
}

bool parseBranchZero(const std::string& line, std::string& opcode, std::string& reg, std::string& label)
{
    if (!startsWith(line, "beqz ") && !startsWith(line, "bnez ")) {
        return false;
    }
    const std::size_t space = line.find(' ');
    const std::size_t comma = line.find(',', space + 1);
    if (space == std::string::npos || comma == std::string::npos) {
        return false;
    }
    opcode = line.substr(0, space);
    reg = trim(line.substr(space + 1, comma - space - 1));
    label = trim(line.substr(comma + 1));
    return !reg.empty() && !label.empty();
}

bool parseRegisterBranch(const std::string& line, std::string& opcode, std::string& lhs, std::string& rhs, std::string& label)
{
    if (!startsWith(line, "blt ") && !startsWith(line, "bge ") && !startsWith(line, "beq ") && !startsWith(line, "bne ")) {
        return false;
    }
    const std::size_t space = line.find(' ');
    const std::size_t firstComma = line.find(',', space + 1);
    if (space == std::string::npos || firstComma == std::string::npos) {
        return false;
    }
    const std::size_t secondComma = line.find(',', firstComma + 1);
    if (secondComma == std::string::npos) {
        return false;
    }
    opcode = line.substr(0, space);
    lhs = trim(line.substr(space + 1, firstComma - space - 1));
    rhs = trim(line.substr(firstComma + 1, secondComma - firstComma - 1));
    label = trim(line.substr(secondComma + 1));
    return !lhs.empty() && !rhs.empty() && !label.empty();
}

std::string invertRegisterBranch(const std::string& opcode)
{
    if (opcode == "blt") {
        return "bge";
    }
    if (opcode == "bge") {
        return "blt";
    }
    if (opcode == "beq") {
        return "bne";
    }
    if (opcode == "bne") {
        return "beq";
    }
    return {};
}

bool isSavedRegister(const std::string& reg)
{
    if (reg.size() < 2 || reg[0] != 's') {
        return false;
    }
    if (reg == "s0") {
        return false;
    }
    for (std::size_t i = 1; i < reg.size(); ++i) {
        if (reg[i] < '0' || reg[i] > '9') {
            return false;
        }
    }
    const int index = std::stoi(reg.substr(1));
    return index >= 1 && index <= 11;
}

bool isFoldableSourceRegister(const std::string& reg)
{
    return isSavedRegister(reg) || reg == "zero";
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (const std::string& line : lines) {
        output << line << '\n';
    }
    return output.str();
}

std::string peepholeRiscV(const std::string& assembly)
{
    const std::vector<std::string> input = splitLines(assembly);
    std::vector<std::string> kept;
    kept.reserve(input.size());

    bool unreachable = false;
    std::string previousLiReg;
    std::string previousLiImm;
    bool hasPreviousLi = false;

    for (const std::string& rawLine : input) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            kept.push_back(rawLine);
            continue;
        }

        if (isLabelLine(line) || startsWith(line, ".") || line == ".text" || line == ".data") {
            unreachable = false;
            hasPreviousLi = false;
            kept.push_back(rawLine);
            continue;
        }

        if (unreachable) {
            continue;
        }

        std::string dst;
        std::string src;
        if (parseRegisterPair(line, "mv", dst, src) && dst == src) {
            continue;
        }

        std::string liReg;
        std::string liImm;
        if (parseLi(line, liReg, liImm)) {
            if (hasPreviousLi && previousLiReg == liReg && previousLiImm == liImm) {
                continue;
            }
            previousLiReg = liReg;
            previousLiImm = liImm;
            hasPreviousLi = true;
        } else {
            hasPreviousLi = false;
        }

        kept.push_back(rawLine);

        if (startsWith(line, "j ") || line == "ret") {
            unreachable = true;
        }
    }

    std::vector<std::string> output;
    output.reserve(kept.size());
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const std::string line = trim(kept[i]);

        std::string moveDst;
        std::string moveSrc;
        std::string secondMoveDst;
        std::string secondMoveSrc;
        std::string opDst;
        std::string opLhs;
        std::string opRhs;
        bool foldedRegisterBinary = false;
        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 3 < kept.size()
            && moveDst == "t0" && isFoldableSourceRegister(moveSrc)
            && parseRegisterPair(trim(kept[i + 1]), "mv", secondMoveDst, secondMoveSrc)
            && secondMoveDst == "a0" && isFoldableSourceRegister(secondMoveSrc)) {
            static const char* ops[] = {"add", "sub", "mul", "div", "rem", "and", "or"};
            for (const char* op : ops) {
                std::string resultDst;
                std::string resultSrc;
                if (parseThreeRegister(trim(kept[i + 2]), op, opDst, opLhs, opRhs)
                    && opDst == "a0" && opLhs == "t0" && opRhs == "a0"
                    && parseRegisterPair(trim(kept[i + 3]), "mv", resultDst, resultSrc)
                    && resultSrc == "a0" && isSavedRegister(resultDst)) {
                    output.push_back("  " + std::string(op) + " " + resultDst + ", " + moveSrc + ", " + secondMoveSrc);
                    i += 3;
                    foldedRegisterBinary = true;
                    break;
                }
            }
            if (foldedRegisterBinary) {
                continue;
            }
        }

        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 6 < kept.size()
            && moveDst == "t0" && isSavedRegister(moveSrc)
            && parseRegisterPair(trim(kept[i + 1]), "mv", secondMoveDst, secondMoveSrc)
            && secondMoveDst == "a0" && isFoldableSourceRegister(secondMoveSrc)
            && parseThreeRegister(trim(kept[i + 2]), "add", opDst, opLhs, opRhs)
            && opDst == "a0" && opLhs == "t0" && opRhs == "a0") {
            std::string middleMoveDst;
            std::string middleMoveSrc;
            std::string thirdMoveDst;
            std::string thirdMoveSrc;
            std::string resultDst;
            std::string resultSrc;
            if (parseRegisterPair(trim(kept[i + 3]), "mv", middleMoveDst, middleMoveSrc)
                && middleMoveDst == "t0" && middleMoveSrc == "a0"
                && parseRegisterPair(trim(kept[i + 4]), "mv", thirdMoveDst, thirdMoveSrc)
                && thirdMoveDst == "a0" && isFoldableSourceRegister(thirdMoveSrc)
                && parseThreeRegister(trim(kept[i + 5]), "add", opDst, opLhs, opRhs)
                && opDst == "a0" && opLhs == "t0" && opRhs == "a0"
                && parseRegisterPair(trim(kept[i + 6]), "mv", resultDst, resultSrc)
                && resultSrc == "a0" && isSavedRegister(resultDst)) {
                output.push_back("  add " + resultDst + ", " + moveSrc + ", " + secondMoveSrc);
                output.push_back("  add " + resultDst + ", " + resultDst + ", " + thirdMoveSrc);
                i += 6;
                continue;
            }
        }

        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 2 < kept.size()
            && moveDst == "t0" && isFoldableSourceRegister(moveSrc)
            && parseImmediateBinary(trim(kept[i + 1]), "addi", opDst, opLhs, opRhs)
            && opDst == "a0" && opLhs == "t0"
            && parseRegisterPair(trim(kept[i + 2]), "mv", secondMoveDst, secondMoveSrc)
            && secondMoveSrc == "a0" && isSavedRegister(secondMoveDst)) {
            output.push_back("  addi " + secondMoveDst + ", " + moveSrc + ", " + opRhs);
            i += 2;
            continue;
        }

        if (parseLi(line, moveDst, moveSrc) && i + 1 < kept.size()
            && moveDst == "a0"
            && parseRegisterPair(trim(kept[i + 1]), "mv", secondMoveDst, secondMoveSrc)
            && secondMoveSrc == "a0" && isSavedRegister(secondMoveDst)) {
            output.push_back("  li " + secondMoveDst + ", " + moveSrc);
            ++i;
            continue;
        }

        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 1 < kept.size()
            && moveDst == "a0" && isFoldableSourceRegister(moveSrc)) {
            std::string storeSrc;
            std::string storeAddress;
            if (parseStoreWord(trim(kept[i + 1]), storeSrc, storeAddress) && storeSrc == "a0") {
                output.push_back("  sw " + moveSrc + ", " + storeAddress);
                ++i;
                continue;
            }
        }

        std::string immReg;
        std::string immValue;
        std::string branchOpcode;
        std::string branchReg;
        std::string branchTarget;
        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 3 < kept.size()
            && moveDst == "t0" && isFoldableSourceRegister(moveSrc)
            && parseLi(trim(kept[i + 1]), immReg, immValue) && immReg == "a0"
            && parseThreeRegister(trim(kept[i + 2]), "slt", opDst, opLhs, opRhs)
            && opDst == "a0" && opLhs == "t0" && opRhs == "a0"
            && parseBranchZero(trim(kept[i + 3]), branchOpcode, branchReg, branchTarget)
            && branchReg == "a0") {
            output.push_back("  li a0, " + immValue);
            output.push_back("  " + std::string(branchOpcode == "beqz" ? "bge" : "blt") + " " + moveSrc + ", a0, " + branchTarget);
            i += 3;
            continue;
        }

        if (parseRegisterPair(line, "mv", moveDst, moveSrc) && i + 2 < kept.size()
            && moveDst == "t0" && isFoldableSourceRegister(moveSrc)
            && parseImmediateBinary(trim(kept[i + 1]), "slti", opDst, opLhs, opRhs)
            && opDst == "a0" && opLhs == "t0"
            && parseBranchZero(trim(kept[i + 2]), branchOpcode, branchReg, branchTarget)
            && branchReg == "a0") {
            output.push_back("  li a0, " + opRhs);
            output.push_back("  " + std::string(branchOpcode == "beqz" ? "bge" : "blt") + " " + moveSrc + ", a0, " + branchTarget);
            i += 2;
            continue;
        }

        std::string jumpTarget;
        std::string registerBranchOpcode;
        std::string registerBranchLhs;
        std::string registerBranchRhs;
        std::string registerBranchTarget;
        if (parseRegisterBranch(line, registerBranchOpcode, registerBranchLhs, registerBranchRhs, registerBranchTarget) && i + 2 < kept.size()
            && parseJump(trim(kept[i + 1]), jumpTarget)
            && trim(kept[i + 2]) == registerBranchTarget + ":") {
            const std::string inverted = invertRegisterBranch(registerBranchOpcode);
            if (!inverted.empty()) {
                output.push_back("  " + inverted + " " + registerBranchLhs + ", " + registerBranchRhs + ", " + jumpTarget);
                ++i;
                continue;
            }
        }

        if (parseBranchZero(line, branchOpcode, branchReg, branchTarget) && i + 2 < kept.size()
            && parseJump(trim(kept[i + 1]), jumpTarget)
            && trim(kept[i + 2]) == branchTarget + ":") {
            const std::string inverted = branchOpcode == "bnez" ? "beqz" : "bnez";
            output.push_back("  " + inverted + " " + branchReg + ", " + jumpTarget);
            ++i;
            continue;
        }

        std::string label;
        if (parseJump(line, label) && i + 1 < kept.size()) {
            const std::string next = trim(kept[i + 1]);
            if (next == label + ":") {
                continue;
            }
        }
        output.push_back(kept[i]);
    }

    return joinLines(output);
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
        std::vector<ValueInterval> valueIntervals(static_cast<std::size_t>(std::max(function_.nextValue, 0)));
        for (int value = 0; value < function_.nextValue; ++value) {
            valueIntervals[static_cast<std::size_t>(value)] = ValueInterval{value, std::numeric_limits<int>::max(), -1, 0};
        }

        auto touchLocal = [&](const std::string& symbol, int weight) {
            if (symbol.empty()) {
                return;
            }
            locals.insert(symbol);
            localUseCounts[symbol] += weight;
        };

        for (const std::string& param : function_.params) {
            touchLocal(param, 0);
        }

        int maxValue = -1;
        std::unordered_map<int, int> valueDefinitionBlock;
        int position = 0;
        for (std::size_t blockIndex = 0; blockIndex < function_.blocks.size(); ++blockIndex) {
            const auto& block = function_.blocks[blockIndex];
            for (const auto& inst : block.instructions) {
                const int instPosition = position++;
                maxValue = std::max(maxValue, inst.dst.id);
                if (inst.dst.id >= 0) {
                    valueDefinitionBlock[inst.dst.id] = static_cast<int>(blockIndex);
                    defineValue(inst.dst, instPosition, valueIntervals);
                }
                for (const auto& operand : inst.operands) {
                    countValueUse(operand);
                    countRawValueUse(operand);
                    touchValueUse(operand, instPosition, 1, valueIntervals);
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
                const int terminatorPosition = position++;
                countValueUse(block.terminator.condition);
                countValueUse(block.terminator.returnValue);
                countRawValueUse(block.terminator.condition);
                countRawValueUse(block.terminator.returnValue);
                touchValueUse(block.terminator.condition, terminatorPosition, 2, valueIntervals);
                touchValueUse(block.terminator.returnValue, terminatorPosition, 2, valueIntervals);
            }
        }

        for (std::size_t blockIndex = 0; blockIndex < function_.blocks.size(); ++blockIndex) {
            const ir::BasicBlock& block = function_.blocks[blockIndex];
            if (!hasBackedge(block, blockIndex)) {
                continue;
            }
            for (const ir::Instruction& inst : block.instructions) {
                for (const ir::Operand& operand : inst.operands) {
                    countLoopCarriedValueUse(operand, valueDefinitionBlock, blockIndex);
                    boostLoopCarriedValue(operand, valueDefinitionBlock, blockIndex, valueIntervals);
                }
            }
            if (block.hasTerminator) {
                countLoopCarriedValueUse(block.terminator.condition, valueDefinitionBlock, blockIndex);
                countLoopCarriedValueUse(block.terminator.returnValue, valueDefinitionBlock, blockIndex);
                boostLoopCarriedValue(block.terminator.condition, valueDefinitionBlock, blockIndex, valueIntervals);
                boostLoopCarriedValue(block.terminator.returnValue, valueDefinitionBlock, blockIndex, valueIntervals);
            }
        }

        std::vector<std::string> orderedLocals(locals.begin(), locals.end());
        std::sort(orderedLocals.begin(), orderedLocals.end(), [&](const std::string& lhs, const std::string& rhs) {
            if (localUseCounts[lhs] != localUseCounts[rhs]) {
                return localUseCounts[lhs] > localUseCounts[rhs];
            }
            return lhs < rhs;
        });

        int nextSavedReg = 0;
        for (const std::string& local : orderedLocals) {
            if (nextSavedReg >= 11) {
                break;
            }
            if (localUseCounts[local] < 4) {
                continue;
            }
            locals_[local] = LocalSlot{true, nextSavedReg, 0};
            usedSavedRegs_.push_back(nextSavedReg);
            ++nextSavedReg;
        }

        const int regBudget = nextSavedReg;
        allocateValueRegisters(valueIntervals, regBudget);

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
            if (valueRegs_.contains(value)) {
                continue;
            }
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
            const bool directBranch = canEmitDirectBranch(block);
            const std::size_t instructionCount = directBranch && !block.instructions.empty()
                ? block.instructions.size() - 1
                : block.instructions.size();
            for (std::size_t instIndex = 0; instIndex < instructionCount; ++instIndex) {
                emitInstruction(block.instructions[instIndex]);
            }
            if (block.hasTerminator) {
                if (directBranch) {
                    emitDirectBranch(block.instructions.back(), block.terminator);
                } else {
                    emitTerminator(block.terminator);
                }
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
        validateInstruction(inst);
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            materializeConst(inst.dst, inst.operands.at(0).immediate);
            break;
        case ir::InstructionKind::Copy:
            materializeCopy(inst.dst, inst.operands.at(0));
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
            clobberRegisterAliases("t0");
            out_ << "  la t0, g_" << sanitizeLabel(inst.symbol) << "\n";
            clobberRegisterAliases("a0");
            out_ << "  lw a0, 0(t0)\n";
            storeValue(inst.dst, "a0");
            break;
        case ir::InstructionKind::StoreGlobal:
            loadOperand(inst.operands.at(0), "a0");
            clobberRegisterAliases("t0");
            out_ << "  la t0, g_" << sanitizeLabel(inst.symbol) << "\n";
            out_ << "  sw a0, 0(t0)\n";
            break;
        case ir::InstructionKind::LoadLocal:
            materializeLocal(inst.dst, inst.symbol);
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

    void validateInstruction(const ir::Instruction& inst) const
    {
        const bool needsOne = inst.kind == ir::InstructionKind::Const
            || inst.kind == ir::InstructionKind::Copy
            || inst.kind == ir::InstructionKind::Unary
            || inst.kind == ir::InstructionKind::StoreGlobal
            || inst.kind == ir::InstructionKind::StoreLocal;
        if (needsOne && inst.operands.empty()) {
            throw IrCodegenError(std::string("IR instruction missing operand: ")
                + ir::instructionKindName(inst.kind)
                + " "
                + inst.symbol);
        }
        if (inst.kind == ir::InstructionKind::Binary && inst.operands.size() < 2) {
            throw IrCodegenError("IR binary instruction missing operand");
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
            clobberRegisterAliases("t0");
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
        const int stackArgBytes = std::max(0, argCount - 8) * 4;
        const int reserveBytes = alignTo(stackArgBytes, 16);
        if (reserveBytes > 0) {
            out_ << "  addi sp, sp, -" << reserveBytes << "\n";
        }

        for (int i = 8; i < argCount; ++i) {
            loadOperand(inst.operands[static_cast<std::size_t>(i)], "t0");
            out_ << "  sw t0, " << (i - 8) * 4 << "(sp)\n";
        }

        for (int i = std::min(8, argCount) - 1; i >= 0; --i) {
            loadOperand(inst.operands[static_cast<std::size_t>(i)], "a" + std::to_string(i));
        }

        clobberCallerSavedRegisterAliases();
        out_ << "  call " << inst.symbol << "\n";
        if (reserveBytes > 0) {
            out_ << "  addi sp, sp, " << reserveBytes << "\n";
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

    bool canEmitDirectBranch(const ir::BasicBlock& block) const
    {
        if (!block.hasTerminator || block.terminator.kind != ir::TerminatorKind::Branch || block.terminator.condition.isImmediate || block.instructions.empty()) {
            return false;
        }
        const ir::Instruction& inst = block.instructions.back();
        if (inst.kind != ir::InstructionKind::Binary || inst.dst.id != block.terminator.condition.value.id || rawValueUseCount(inst.dst) != 1) {
            return false;
        }
        switch (inst.binaryOp) {
        case ir::BinaryOpcode::Equal:
        case ir::BinaryOpcode::NotEqual:
        case ir::BinaryOpcode::Less:
        case ir::BinaryOpcode::LessEqual:
        case ir::BinaryOpcode::Greater:
        case ir::BinaryOpcode::GreaterEqual:
            return inst.operands.size() >= 2;
        default:
            return false;
        }
    }

    void emitDirectBranch(const ir::Instruction& inst, const ir::Terminator& terminator)
    {
        loadOperand(inst.operands.at(0), "t0");
        loadOperand(inst.operands.at(1), "a0");
        switch (inst.binaryOp) {
        case ir::BinaryOpcode::Equal:
            out_ << "  beq t0, a0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::BinaryOpcode::NotEqual:
            out_ << "  bne t0, a0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::BinaryOpcode::Less:
            out_ << "  blt t0, a0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::BinaryOpcode::LessEqual:
            out_ << "  bge a0, t0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::BinaryOpcode::Greater:
            out_ << "  blt a0, t0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        case ir::BinaryOpcode::GreaterEqual:
            out_ << "  bge t0, a0, " << blockLabel(terminator.trueBlock) << "\n";
            break;
        default:
            throw IrCodegenError("invalid direct branch opcode");
        }
        out_ << "  j " << blockLabel(terminator.falseBlock) << "\n";
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
        const auto valueReg = valueRegs_.find(value.id);
        if (valueReg != valueRegs_.end()) {
            const std::string source = savedRegName(valueReg->second);
            if (source != reg) {
                clobberRegisterAliases(reg);
                out_ << "  mv " << reg << ", " << source << "\n";
            }
            return;
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
        const auto valueReg = valueRegs_.find(value.id);
        if (valueReg != valueRegs_.end()) {
            valueAliases_.erase(value.id);
            out_ << "  mv " << savedRegName(valueReg->second) << ", " << reg << "\n";
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
            clobberRegisterAliases(reg);
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

    bool hasValueRegister(ir::Value value) const
    {
        return value.id >= 0 && valueRegs_.find(value.id) != valueRegs_.end();
    }

    std::string valueRegisterName(ir::Value value) const
    {
        const auto found = valueRegs_.find(value.id);
        if (found == valueRegs_.end()) {
            throw IrCodegenError("IR value has no allocated register");
        }
        return savedRegName(found->second);
    }

    void materializeConst(ir::Value value, std::int32_t immediate)
    {
        if (hasValueRegister(value) && valueUseCount(value) > 0) {
            valueAliases_.erase(value.id);
            out_ << "  li " << valueRegisterName(value) << ", " << immediate << "\n";
            return;
        }
        aliasImmediate(value, immediate);
    }

    void materializeCopy(ir::Value value, const ir::Operand& operand)
    {
        if (hasValueRegister(value) && valueUseCount(value) > 0) {
            valueAliases_.erase(value.id);
            loadOperand(operand, valueRegisterName(value));
            return;
        }
        aliasOperand(value, operand);
    }

    void materializeLocal(ir::Value value, const std::string& symbol)
    {
        if (hasValueRegister(value) && valueUseCount(value) > 0) {
            valueAliases_.erase(value.id);
            loadLocal(symbol, valueRegisterName(value));
            return;
        }
        aliasLocal(value, symbol);
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

    void clobberCallerSavedRegisterAliases()
    {
        static const char* regs[] = {
            "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
            "t0", "t1", "t2", "t3", "t4", "t5", "t6",
        };
        for (const char* reg : regs) {
            clobberRegisterAliases(reg);
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

    void countValueUse(const ir::Operand& operand, int weight = 1)
    {
        if (!operand.isImmediate && operand.value.id >= 0) {
            valueUseCounts_[operand.value.id] += weight;
        }
    }

    void countRawValueUse(const ir::Operand& operand)
    {
        if (!operand.isImmediate && operand.value.id >= 0) {
            ++rawValueUseCounts_[operand.value.id];
        }
    }

    void defineValue(ir::Value value, int position, std::vector<ValueInterval>& intervals)
    {
        if (value.id < 0 || value.id >= static_cast<int>(intervals.size())) {
            return;
        }
        ValueInterval& interval = intervals[static_cast<std::size_t>(value.id)];
        interval.start = std::min(interval.start, position);
        interval.end = std::max(interval.end, position);
    }

    void touchValueUse(const ir::Operand& operand, int position, int weight, std::vector<ValueInterval>& intervals)
    {
        if (operand.isImmediate || operand.value.id < 0 || operand.value.id >= static_cast<int>(intervals.size())) {
            return;
        }
        ValueInterval& interval = intervals[static_cast<std::size_t>(operand.value.id)];
        interval.start = std::min(interval.start, position);
        interval.end = std::max(interval.end, position);
        interval.weight += weight;
    }

    bool hasBackedge(const ir::BasicBlock& block, std::size_t blockIndex) const
    {
        if (!block.hasTerminator) {
            return false;
        }
        switch (block.terminator.kind) {
        case ir::TerminatorKind::Jump:
            return block.terminator.trueBlock <= static_cast<int>(blockIndex);
        case ir::TerminatorKind::Branch:
            return block.terminator.trueBlock <= static_cast<int>(blockIndex)
                || block.terminator.falseBlock <= static_cast<int>(blockIndex);
        case ir::TerminatorKind::Return:
            return false;
        }
        return false;
    }

    void countLoopCarriedValueUse(const ir::Operand& operand, const std::unordered_map<int, int>& valueDefinitionBlock, std::size_t blockIndex)
    {
        if (operand.isImmediate || operand.value.id < 0) {
            return;
        }
        const auto found = valueDefinitionBlock.find(operand.value.id);
        if (found == valueDefinitionBlock.end()) {
            return;
        }
        if (found->second < static_cast<int>(blockIndex)) {
            countValueUse(operand, 8);
        }
    }

    void boostLoopCarriedValue(const ir::Operand& operand, const std::unordered_map<int, int>& valueDefinitionBlock, std::size_t blockIndex, std::vector<ValueInterval>& intervals)
    {
        if (operand.isImmediate || operand.value.id < 0 || operand.value.id >= static_cast<int>(intervals.size())) {
            return;
        }
        const auto found = valueDefinitionBlock.find(operand.value.id);
        if (found == valueDefinitionBlock.end()) {
            return;
        }
        if (found->second < static_cast<int>(blockIndex)) {
            intervals[static_cast<std::size_t>(operand.value.id)].weight += 12;
        }
    }

    void allocateValueRegisters(std::vector<ValueInterval>& intervals, int firstRegister)
    {
        if (firstRegister >= 11) {
            return;
        }
        std::vector<ValueInterval> candidates;
        for (const ValueInterval& interval : intervals) {
            const int rawUses = rawValueUseCounts_.contains(interval.value) ? rawValueUseCounts_[interval.value] : 0;
            if (interval.value < 0 || interval.end < interval.start || (interval.weight < 3 && rawUses < 2)) {
                continue;
            }
            candidates.push_back(interval);
        }
        std::sort(candidates.begin(), candidates.end(), [](const ValueInterval& lhs, const ValueInterval& rhs) {
            if (lhs.start != rhs.start) {
                return lhs.start < rhs.start;
            }
            if (lhs.end != rhs.end) {
                return lhs.end < rhs.end;
            }
            return lhs.value < rhs.value;
        });

        std::vector<int> freeRegs;
        for (int reg = 10; reg >= firstRegister; --reg) {
            freeRegs.push_back(reg);
        }
        struct ActiveInterval {
            int value = -1;
            int end = 0;
            int reg = -1;
            int weight = 0;
        };
        std::vector<ActiveInterval> active;

        auto expireOld = [&](int start) {
            std::vector<ActiveInterval> kept;
            for (const ActiveInterval& current : active) {
                if (current.end < start) {
                    freeRegs.push_back(current.reg);
                } else {
                    kept.push_back(current);
                }
            }
            active = std::move(kept);
        };

        for (const ValueInterval& interval : candidates) {
            expireOld(interval.start);
            if (!freeRegs.empty()) {
                const int reg = freeRegs.back();
                freeRegs.pop_back();
                valueRegs_[interval.value] = reg;
                active.push_back(ActiveInterval{interval.value, interval.end, reg, interval.weight});
                continue;
            }

            auto spill = std::max_element(active.begin(), active.end(), [](const ActiveInterval& lhs, const ActiveInterval& rhs) {
                if (lhs.end != rhs.end) {
                    return lhs.end < rhs.end;
                }
                return lhs.weight < rhs.weight;
            });
            if (spill != active.end() && spill->end > interval.end && spill->weight <= interval.weight) {
                const int reg = spill->reg;
                valueRegs_.erase(spill->value);
                *spill = ActiveInterval{interval.value, interval.end, reg, interval.weight};
                valueRegs_[interval.value] = reg;
            }
        }

        std::vector<int> regs;
        regs.reserve(valueRegs_.size());
        for (const auto& [_, reg] : valueRegs_) {
            regs.push_back(reg);
        }
        std::sort(regs.begin(), regs.end());
        regs.erase(std::unique(regs.begin(), regs.end()), regs.end());
        for (int reg : regs) {
            usedSavedRegs_.push_back(reg);
        }
    }

    int valueUseCount(ir::Value value) const
    {
        const auto found = valueUseCounts_.find(value.id);
        return found == valueUseCounts_.end() ? 0 : found->second;
    }

    int rawValueUseCount(ir::Value value) const
    {
        const auto found = rawValueUseCounts_.find(value.id);
        return found == rawValueUseCounts_.end() ? 0 : found->second;
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
    std::unordered_map<int, int> rawValueUseCounts_;
    std::unordered_map<int, int> remainingValueUses_;
    std::unordered_map<int, int> valueRegs_;
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
    for (const auto& function : module.functions) {
        if (function.blocks.empty() || function.nextValue < 0) {
            return false;
        }

        auto validValue = [&](ir::Value value) {
            return value.id >= 0 && value.id < function.nextValue;
        };

        auto validOperand = [&](const ir::Operand& operand) {
            return operand.isImmediate || validValue(operand.value);
        };

        auto validBlock = [&](int block) {
            return block >= 0 && block < static_cast<int>(function.blocks.size());
        };

        for (const auto& block : function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.dst.id >= function.nextValue) {
                    return false;
                }
                for (const auto& operand : inst.operands) {
                    if (!validOperand(operand)) {
                        return false;
                    }
                }
            }

            if (!block.hasTerminator) {
                continue;
            }
            switch (block.terminator.kind) {
            case ir::TerminatorKind::Jump:
                if (!validBlock(block.terminator.trueBlock)) {
                    return false;
                }
                break;
            case ir::TerminatorKind::Branch:
                if (!validOperand(block.terminator.condition)
                    || !validBlock(block.terminator.trueBlock)
                    || !validBlock(block.terminator.falseBlock)) {
                    return false;
                }
                break;
            case ir::TerminatorKind::Return:
                if (block.terminator.hasReturnValue && !validOperand(block.terminator.returnValue)) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

void IrRiscVCodeGenerator::generate(const ir::Module& module, std::ostream& out) const
{
    std::ostringstream buffer;
    buffer << ".data\n";
    for (const auto& global : module.globals) {
        buffer << ".globl g_" << sanitizeLabel(global.name) << "\n";
        buffer << "g_" << sanitizeLabel(global.name) << ":\n";
        buffer << "  .word " << global.init << "\n";
    }

    buffer << "\n.text\n";
    for (const auto& function : module.functions) {
        FunctionEmitter(function, buffer).emit();
    }

    out << peepholeRiscV(buffer.str());
}

} // namespace toyc
