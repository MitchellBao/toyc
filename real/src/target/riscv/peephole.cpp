#include "peephole.h"

#include <cstdlib>

#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::riscv {
namespace {

std::string trim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool isLabel(const std::string& line)
{
    return !line.empty() && line.back() == ':';
}

std::vector<std::string> splitOperands(const std::string& text)
{
    std::vector<std::string> operands;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        operands.push_back(trim(text.substr(begin, end - begin)));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return operands;
}

bool parseInstruction(const std::string& text, std::string& opcode, std::vector<std::string>& operands)
{
    const std::string line = trim(text);
    if (line.empty() || line[0] == '.' || isLabel(line)) {
        return false;
    }
    const std::size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) {
        opcode = line;
        operands.clear();
        return true;
    }
    opcode = line.substr(0, space);
    operands = splitOperands(line.substr(space + 1));
    return true;
}

std::string indentOf(const std::string& line)
{
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return line.substr(0, first);
}

std::string formatInstruction(const std::string& indent, const std::string& opcode, const std::vector<std::string>& operands)
{
    std::ostringstream out;
    out << indent << opcode;
    if (!operands.empty()) {
        out << ' ';
        for (std::size_t i = 0; i < operands.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << operands[i];
        }
    }
    return out.str();
}

bool isZeroRegister(const std::string& reg)
{
    return reg == "zero" || reg == "x0";
}

bool isMemorySlot(const std::string& operand)
{
    const std::size_t open = operand.find('(');
    return open != std::string::npos && operand.find(')', open + 1) == operand.size() - 1;
}

std::optional<std::string> invertedBranchOpcode(const std::string& opcode)
{
    if (opcode == "beqz") {
        return "bnez";
    }
    if (opcode == "bnez") {
        return "beqz";
    }
    if (opcode == "beq") {
        return "bne";
    }
    if (opcode == "bne") {
        return "beq";
    }
    if (opcode == "blt") {
        return "bge";
    }
    if (opcode == "bge") {
        return "blt";
    }
    return std::nullopt;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Register copy coalescing.
//
// Removes redundant register-to-register moves that the emitter produces when a
// register-resident local is loaded into a fresh value register (e.g.
// `mv a2, s2` followed by `add a4, a2, a3`, where `s2` still holds the value).
// It does two sound, conservative things per function:
//   1. Local copy propagation: within a basic block, forward the source of a
//      `mv` into later reads, until either register is redefined or the block
//      ends (copies never cross labels/branches/calls here).
//   2. Dead-move elimination: using real backward register liveness over a CFG
//      built from the assembly, delete a `mv rD, rS` whose destination is not
//      live afterwards.
// The transform never touches sp/ra/gp/tp/zero/fp and treats `call` as
// clobbering all caller-saved registers, so it cannot change program behaviour.
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& knownRegisters()
{
    static const std::unordered_set<std::string> regs = {
        "zero", "ra", "sp", "gp", "tp", "fp",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
    return regs;
}

// Registers whose defining move may be deleted (never the special/fixed ones).
bool isDeletableReg(const std::string& reg)
{
    static const std::unordered_set<std::string> fixed = {"zero", "ra", "sp", "gp", "tp", "fp", "s0"};
    return knownRegisters().count(reg) != 0 && fixed.count(reg) == 0;
}

const std::vector<std::string>& callerSavedRegs()
{
    static const std::vector<std::string> regs = {
        "ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    return regs;
}

const std::vector<std::string>& argRegs()
{
    static const std::vector<std::string> regs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    return regs;
}

// The base register inside a `off(reg)` memory operand, if any.
std::optional<std::string> memoryBaseReg(const std::string& operand)
{
    const std::size_t open = operand.find('(');
    const std::size_t close = operand.find(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return std::nullopt;
    }
    const std::string inner = trim(operand.substr(open + 1, close - open - 1));
    if (knownRegisters().count(inner) != 0) {
        return inner;
    }
    return std::nullopt;
}

bool isPlainReg(const std::string& operand)
{
    return knownRegisters().count(operand) != 0;
}

struct RegEffects {
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    bool understood = true; // false => treat as a full barrier (conservative)
};

// Read/write register sets for every opcode the backend emits. Anything not
// listed is treated as an unknown barrier so the pass stays conservative.
RegEffects effectsOf(const std::string& opcode, const std::vector<std::string>& ops)
{
    RegEffects e;
    auto addRead = [&](const std::string& r) {
        if (isPlainReg(r)) {
            e.reads.push_back(r);
        } else if (const auto base = memoryBaseReg(r)) {
            e.reads.push_back(*base);
        }
    };
    auto addWrite = [&](const std::string& r) {
        if (isPlainReg(r)) {
            e.writes.push_back(r);
        }
    };

    static const std::unordered_set<std::string> rdRs1Rs2 = {"add", "sub", "mul", "mulh", "div", "rem", "and", "or", "slt", "sltu"};
    static const std::unordered_set<std::string> rdRsImm = {"addi", "slti", "sltiu", "slli", "srai", "srli", "xori", "andi", "ori"};
    static const std::unordered_set<std::string> rdRs = {"mv", "neg", "seqz", "snez", "not"};
    static const std::unordered_set<std::string> branch2 = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};

    if (rdRs1Rs2.count(opcode) && ops.size() == 3) {
        addWrite(ops[0]); addRead(ops[1]); addRead(ops[2]);
    } else if (rdRsImm.count(opcode) && ops.size() == 3) {
        addWrite(ops[0]); addRead(ops[1]);
    } else if (rdRs.count(opcode) && ops.size() == 2) {
        addWrite(ops[0]); addRead(ops[1]);
    } else if (opcode == "li" && ops.size() == 2) {
        addWrite(ops[0]);
    } else if (opcode == "la" && ops.size() == 2) {
        addWrite(ops[0]);
    } else if (opcode == "lw" && ops.size() == 2) {
        addWrite(ops[0]); addRead(ops[1]);
    } else if (opcode == "sw" && ops.size() == 2) {
        addRead(ops[0]); addRead(ops[1]);
    } else if (branch2.count(opcode) && ops.size() == 3) {
        addRead(ops[0]); addRead(ops[1]);
    } else if ((opcode == "beqz" || opcode == "bnez") && ops.size() == 2) {
        addRead(ops[0]);
    } else if (opcode == "j" && ops.size() == 1) {
        // no register effects
    } else if (opcode == "call") {
        for (const std::string& r : argRegs()) {
            e.reads.push_back(r); // conservative: any arg reg may be used
        }
        for (const std::string& r : callerSavedRegs()) {
            e.writes.push_back(r); // caller-saved clobbered by the call
        }
    } else if (opcode == "ret") {
        e.reads.push_back("a0");
        e.reads.push_back("ra");
        e.reads.push_back("sp");
    } else {
        e.understood = false;
    }
    return e;
}

bool isControlTransfer(const std::string& opcode)
{
    static const std::unordered_set<std::string> ctrl = {
        "j", "ret", "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez"};
    return ctrl.count(opcode) != 0;
}

} // namespace

// Parsed view of one assembly line for the coalescing pass.
namespace {
struct AsmLine {
    std::string indent;
    std::string opcode;
    std::vector<std::string> operands;
    std::string label;      // non-empty if this line is a label
    bool isInstr = false;
    bool isLabel = false;
    bool deleted = false;
};

std::string renderAsmLine(const std::vector<std::string>& rawLines, const std::vector<AsmLine>& parsed, std::size_t i)
{
    const AsmLine& a = parsed[i];
    if (!a.isInstr) {
        return rawLines[i];
    }
    return formatInstruction(a.indent, a.opcode, a.operands);
}

std::string coalesceMoves(const std::string& assembly)
{
    std::vector<std::string> rawLines = splitLines(assembly);
    std::vector<AsmLine> parsed(rawLines.size());
    std::vector<int> instrIndices; // indices into parsed that are instructions
    for (std::size_t i = 0; i < rawLines.size(); ++i) {
        const std::string t = trim(rawLines[i]);
        parsed[i].indent = indentOf(rawLines[i]);
        if (t.empty()) {
            continue;
        }
        // Labels (including local `.L...:` labels) must be recognised before the
        // directive check, since they also start with '.'.
        if (isLabel(t)) {
            parsed[i].isLabel = true;
            parsed[i].label = t.substr(0, t.size() - 1);
            continue;
        }
        if (t[0] == '.') {
            continue;
        }
        std::string opcode;
        std::vector<std::string> operands;
        if (parseInstruction(t, opcode, operands)) {
            parsed[i].isInstr = true;
            parsed[i].opcode = opcode;
            parsed[i].operands = operands;
            instrIndices.push_back(static_cast<int>(i));
        }
    }

    if (instrIndices.empty()) {
        return assembly;
    }

    // Assign each instruction to a basic block. A new block starts at the first
    // instruction, at any instruction preceded by a label, or after a control
    // transfer. Labels map to the block that begins at/after them.
    const int n = static_cast<int>(instrIndices.size());
    std::vector<int> blockOf(n, 0);
    std::unordered_map<std::string, int> labelToBlock;
    int blockCount = 0;
    {
        bool startNew = true;
        for (int k = 0; k < n; ++k) {
            const int lineIdx = instrIndices[k];
            // Any label lines between the previous instruction and this one begin a block.
            const int prevLine = k > 0 ? instrIndices[k - 1] : -1;
            bool labelBetween = false;
            for (int li = prevLine + 1; li < lineIdx; ++li) {
                if (parsed[static_cast<std::size_t>(li)].isLabel) {
                    labelBetween = true;
                }
            }
            if (startNew || labelBetween) {
                ++blockCount;
            }
            blockOf[k] = blockCount - 1;
            // Map any labels immediately preceding this instruction to this block.
            for (int li = prevLine + 1; li < lineIdx; ++li) {
                if (parsed[static_cast<std::size_t>(li)].isLabel) {
                    labelToBlock[parsed[static_cast<std::size_t>(li)].label] = blockCount - 1;
                }
            }
            startNew = isControlTransfer(parsed[static_cast<std::size_t>(lineIdx)].opcode);
        }
    }
    if (blockCount == 0) {
        return assembly;
    }

    // First instruction index of each block, and block of each instruction slot.
    std::vector<std::vector<int>> blockInstrs(static_cast<std::size_t>(blockCount));
    for (int k = 0; k < n; ++k) {
        blockInstrs[static_cast<std::size_t>(blockOf[k])].push_back(k);
    }

    // Successors of each block.
    std::vector<std::vector<int>> succ(static_cast<std::size_t>(blockCount));
    for (int b = 0; b < blockCount; ++b) {
        const std::vector<int>& slots = blockInstrs[static_cast<std::size_t>(b)];
        if (slots.empty()) {
            continue;
        }
        const AsmLine& last = parsed[static_cast<std::size_t>(instrIndices[slots.back()])];
        const std::string& op = last.opcode;
        auto addFallthrough = [&]() {
            if (b + 1 < blockCount) {
                succ[static_cast<std::size_t>(b)].push_back(b + 1);
            }
        };
        auto addLabel = [&](const std::string& lbl) {
            const auto it = labelToBlock.find(lbl);
            if (it != labelToBlock.end()) {
                succ[static_cast<std::size_t>(b)].push_back(it->second);
            }
        };
        if (op == "ret") {
            // no successors
        } else if (op == "j" && last.operands.size() == 1) {
            addLabel(last.operands[0]);
        } else if ((op == "beqz" || op == "bnez") && last.operands.size() == 2) {
            addLabel(last.operands[1]);
            addFallthrough();
        } else if ((op == "beq" || op == "bne" || op == "blt" || op == "bge" || op == "bltu" || op == "bgeu") && last.operands.size() == 3) {
            addLabel(last.operands[2]);
            addFallthrough();
        } else {
            addFallthrough();
        }
    }

    // If any instruction is an unknown opcode, bail entirely (stay safe).
    for (int k = 0; k < n; ++k) {
        const AsmLine& a = parsed[static_cast<std::size_t>(instrIndices[k])];
        if (!effectsOf(a.opcode, a.operands).understood) {
            return assembly;
        }
    }

    // Step 1: local copy propagation within each block.
    for (int b = 0; b < blockCount; ++b) {
        std::unordered_map<std::string, std::string> copyOf; // dst -> src (value equal)
        for (int slot : blockInstrs[static_cast<std::size_t>(b)]) {
            AsmLine& a = parsed[static_cast<std::size_t>(instrIndices[slot])];
            const RegEffects e = effectsOf(a.opcode, a.operands);
            // Substitute reads (only plain-register operands; never the memory base
            // or label operands, and never a written operand position).
            std::unordered_set<std::string> writeSet(e.writes.begin(), e.writes.end());
            if (a.opcode != "call" && a.opcode != "ret") {
                for (std::string& operand : a.operands) {
                    if (isPlainReg(operand) && writeSet.count(operand) == 0) {
                        auto it = copyOf.find(operand);
                        if (it != copyOf.end()) {
                            operand = it->second;
                        }
                    }
                }
            }
            // Invalidate copies killed by this instruction's writes.
            for (const std::string& w : e.writes) {
                copyOf.erase(w);
                for (auto it = copyOf.begin(); it != copyOf.end();) {
                    if (it->second == w) {
                        it = copyOf.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            // Record a new copy for a surviving `mv dst, src`.
            if (a.opcode == "mv" && a.operands.size() == 2 && isPlainReg(a.operands[0]) && isPlainReg(a.operands[1]) && a.operands[0] != a.operands[1]) {
                copyOf[a.operands[0]] = a.operands[1];
            }
            if (a.opcode == "call") {
                copyOf.clear();
            }
        }
    }

    // Step 2: backward liveness, then delete dead moves.
    std::vector<std::unordered_set<std::string>> liveIn(static_cast<std::size_t>(blockCount));
    std::vector<std::unordered_set<std::string>> liveOut(static_cast<std::size_t>(blockCount));
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = blockCount - 1; b >= 0; --b) {
            std::unordered_set<std::string> out;
            for (int s : succ[static_cast<std::size_t>(b)]) {
                out.insert(liveIn[static_cast<std::size_t>(s)].begin(), liveIn[static_cast<std::size_t>(s)].end());
            }
            std::unordered_set<std::string> live = out;
            const std::vector<int>& slots = blockInstrs[static_cast<std::size_t>(b)];
            for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
                AsmLine& a = parsed[static_cast<std::size_t>(instrIndices[*it])];
                if (a.deleted) {
                    continue;
                }
                const RegEffects e = effectsOf(a.opcode, a.operands);
                for (const std::string& w : e.writes) {
                    live.erase(w);
                }
                for (const std::string& r : e.reads) {
                    live.insert(r);
                }
            }
            if (live != liveIn[static_cast<std::size_t>(b)]) {
                liveIn[static_cast<std::size_t>(b)] = std::move(live);
                changed = true;
            }
            if (out != liveOut[static_cast<std::size_t>(b)]) {
                liveOut[static_cast<std::size_t>(b)] = std::move(out);
                changed = true;
            }
        }
    }

    // Delete `mv rD, rS` whose destination is dead immediately after it.
    bool anyDeleted = false;
    for (int b = 0; b < blockCount; ++b) {
        const std::vector<int>& slots = blockInstrs[static_cast<std::size_t>(b)];
        std::unordered_set<std::string> live = liveOut[static_cast<std::size_t>(b)];
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            AsmLine& a = parsed[static_cast<std::size_t>(instrIndices[*it])];
            if (a.deleted) {
                continue;
            }
            const bool isMove = a.opcode == "mv" && a.operands.size() == 2
                && isPlainReg(a.operands[0]) && isPlainReg(a.operands[1]);
            // `live` currently equals the live-out set of this instruction.
            if (isMove && a.operands[0] != a.operands[1]
                && isDeletableReg(a.operands[0])
                && live.count(a.operands[0]) == 0) {
                a.deleted = true;
                anyDeleted = true;
                continue; // liveness unchanged: no reads added, dead write removed
            }
            const RegEffects e = effectsOf(a.opcode, a.operands);
            for (const std::string& w : e.writes) {
                live.erase(w);
            }
            for (const std::string& r : e.reads) {
                live.insert(r);
            }
        }
    }

    if (!anyDeleted) {
        // Copy propagation alone may have rewritten operands; re-render all lines.
    }

    std::ostringstream joined;
    for (std::size_t i = 0; i < rawLines.size(); ++i) {
        if (parsed[i].deleted) {
            continue;
        }
        joined << renderAsmLine(rawLines, parsed, i) << '\n';
    }
    return joined.str();
}
} // namespace

std::string peephole(const std::string& assembly)
{
    std::vector<std::string> lines = splitLines(assembly);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string text = trim(lines[i]);
        std::string opcode;
        std::vector<std::string> operands;
        const bool parsed = parseInstruction(text, opcode, operands);

        if (text.rfind("mv ", 0) == 0) {
            const std::size_t comma = text.find(',');
            if (comma != std::string::npos) {
                const std::string lhs = trim(text.substr(3, comma - 3));
                const std::string rhs = trim(text.substr(comma + 1));
                if (lhs == rhs) {
                    continue;
                }
            }
        }
        if (parsed && opcode == "addi" && operands.size() == 3 && operands[2] == "0") {
            if (operands[0] == operands[1]) {
                continue;
            }
            out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[1]}));
            continue;
        }
        if (parsed && opcode == "add" && operands.size() == 3) {
            if (isZeroRegister(operands[2])) {
                if (operands[0] == operands[1]) {
                    continue;
                }
                out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[1]}));
                continue;
            }
            if (isZeroRegister(operands[1])) {
                if (operands[0] == operands[2]) {
                    continue;
                }
                out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[2]}));
                continue;
            }
        }
        if (text.rfind("j ", 0) == 0 && i + 1 < lines.size()) {
            const std::string target = trim(text.substr(2));
            if (isLabel(trim(lines[i + 1])) && trim(lines[i + 1]) == target + ":") {
                continue;
            }
        }
        if (parsed && (opcode == "beqz" || opcode == "bnez" || opcode == "beq" || opcode == "bne" || opcode == "blt" || opcode == "bge")
            && operands.size() >= 2
            && i + 2 < lines.size()) {
            std::string jumpOpcode;
            std::vector<std::string> jumpOperands;
            const std::string falseLabel = operands.back();
            if (parseInstruction(trim(lines[i + 1]), jumpOpcode, jumpOperands)
                && jumpOpcode == "j"
                && jumpOperands.size() == 1
                && isLabel(trim(lines[i + 2]))
                && trim(lines[i + 2]) == falseLabel + ":") {
                if (const auto inverted = invertedBranchOpcode(opcode); inverted.has_value()) {
                    std::vector<std::string> invertedOperands = operands;
                    invertedOperands.back() = jumpOperands[0];
                    out.push_back(formatInstruction(indentOf(lines[i]), *inverted, invertedOperands));
                    ++i;
                    continue;
                }
            }
        }
        if (parsed && opcode == "sw" && operands.size() == 2 && isMemorySlot(operands[1]) && i + 1 < lines.size()) {
            std::string nextOpcode;
            std::vector<std::string> nextOperands;
            if (parseInstruction(trim(lines[i + 1]), nextOpcode, nextOperands)
                && nextOpcode == "lw"
                && nextOperands.size() == 2
                && nextOperands[1] == operands[1]) {
                out.push_back(lines[i]);
                if (nextOperands[0] != operands[0]) {
                    out.push_back(formatInstruction(indentOf(lines[i + 1]), "mv", {nextOperands[0], operands[0]}));
                }
                ++i;
                continue;
            }
        }
        out.push_back(lines[i]);
    }

    std::ostringstream joined;
    for (const std::string& line : out) {
        joined << line << '\n';
    }

    std::string result = joined.str();
    for (int iteration = 0; iteration < 3; ++iteration) {
        const std::string next = coalesceMoves(result);
        if (next == result) {
            break;
        }
        result = next;
    }
    return result;
}

} // namespace toyc::riscv
