#include "pass_manager.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

struct LinearValue {
    std::string base;
    std::int32_t offset = 0;
};

struct Poly {
    std::int64_t quadratic = 0;
    std::int64_t linear = 0;
    std::int64_t constant = 0;
};

struct PolyValue {
    std::string base;
    Poly poly;
};

std::optional<std::int32_t> evalBinary(ir::BinaryOpcode op, std::int32_t lhs, std::int32_t rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
        return lhs + rhs;
    case ir::BinaryOpcode::Sub:
        return lhs - rhs;
    case ir::BinaryOpcode::Mul:
        return lhs * rhs;
    case ir::BinaryOpcode::Div:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs / rhs};
    case ir::BinaryOpcode::Mod:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs % rhs};
    case ir::BinaryOpcode::Equal:
        return lhs == rhs ? 1 : 0;
    case ir::BinaryOpcode::NotEqual:
        return lhs != rhs ? 1 : 0;
    case ir::BinaryOpcode::Less:
        return lhs < rhs ? 1 : 0;
    case ir::BinaryOpcode::LessEqual:
        return lhs <= rhs ? 1 : 0;
    case ir::BinaryOpcode::Greater:
        return lhs > rhs ? 1 : 0;
    case ir::BinaryOpcode::GreaterEqual:
        return lhs >= rhs ? 1 : 0;
    case ir::BinaryOpcode::LogicalAnd:
        return (lhs != 0 && rhs != 0) ? 1 : 0;
    case ir::BinaryOpcode::LogicalOr:
        return (lhs != 0 || rhs != 0) ? 1 : 0;
    }
    return std::nullopt;
}

std::int32_t evalUnary(ir::UnaryOpcode op, std::int32_t value)
{
    switch (op) {
    case ir::UnaryOpcode::Plus:
        return value;
    case ir::UnaryOpcode::Minus:
        return -value;
    case ir::UnaryOpcode::Not:
        return value == 0 ? 1 : 0;
    }
    return value;
}

bool isZero(const Poly& poly)
{
    return poly.quadratic == 0 && poly.linear == 0 && poly.constant == 0;
}

bool isConstant(const Poly& poly)
{
    return poly.quadratic == 0 && poly.linear == 0;
}

std::optional<std::int32_t> int32Value(std::int64_t value)
{
    if (value < INT32_MIN || value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t wrapInt32(std::int64_t value)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::int32_t wrapInt32Wide(__int128 value)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::optional<std::int64_t> checkedInt64(__int128 value)
{
    if (value < LLONG_MIN || value > LLONG_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

std::optional<std::int64_t> checkedAdd(std::int64_t lhs, std::int64_t rhs)
{
    return checkedInt64(static_cast<__int128>(lhs) + rhs);
}

std::optional<std::int64_t> checkedSub(std::int64_t lhs, std::int64_t rhs)
{
    return checkedInt64(static_cast<__int128>(lhs) - rhs);
}

std::optional<std::int64_t> checkedMul(std::int64_t lhs, std::int64_t rhs)
{
    return checkedInt64(static_cast<__int128>(lhs) * rhs);
}

std::optional<Poly> addPoly(const Poly& lhs, const Poly& rhs)
{
    const auto quadratic = checkedAdd(lhs.quadratic, rhs.quadratic);
    const auto linear = checkedAdd(lhs.linear, rhs.linear);
    const auto constant = checkedAdd(lhs.constant, rhs.constant);
    if (!quadratic.has_value() || !linear.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    return Poly{*quadratic, *linear, *constant};
}

std::optional<Poly> subPoly(const Poly& lhs, const Poly& rhs)
{
    const auto quadratic = checkedSub(lhs.quadratic, rhs.quadratic);
    const auto linear = checkedSub(lhs.linear, rhs.linear);
    const auto constant = checkedSub(lhs.constant, rhs.constant);
    if (!quadratic.has_value() || !linear.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    return Poly{*quadratic, *linear, *constant};
}

std::optional<Poly> mulPolyByConst(const Poly& poly, std::int64_t value)
{
    const auto quadratic = checkedMul(poly.quadratic, value);
    const auto linear = checkedMul(poly.linear, value);
    const auto constant = checkedMul(poly.constant, value);
    if (!quadratic.has_value() || !linear.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    return Poly{*quadratic, *linear, *constant};
}

std::optional<Poly> mulPoly(const Poly& lhs, const Poly& rhs)
{
    if (isConstant(lhs)) {
        return mulPolyByConst(rhs, lhs.constant);
    }
    if (isConstant(rhs)) {
        return mulPolyByConst(lhs, rhs.constant);
    }
    if (lhs.quadratic != 0 || rhs.quadratic != 0) {
        return std::nullopt;
    }

    // (a*i + c) * (b*i + d) = ab*i^2 + (ad + bc)*i + cd.
    const auto quadratic = checkedMul(lhs.linear, rhs.linear);
    const auto leftLinear = checkedMul(lhs.linear, rhs.constant);
    const auto rightLinear = checkedMul(rhs.linear, lhs.constant);
    const auto constant = checkedMul(lhs.constant, rhs.constant);
    if (!quadratic.has_value() || !leftLinear.has_value() || !rightLinear.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    const auto linear = checkedAdd(*leftLinear, *rightLinear);
    if (!linear.has_value()) {
        return std::nullopt;
    }
    return Poly{*quadratic, *linear, *constant};
}

std::optional<PolyValue> polyOf(
    const ir::Operand& operand,
    const std::unordered_map<int, std::int32_t>& constants,
    const std::unordered_map<int, PolyValue>& values)
{
    if (operand.isImmediate) {
        return PolyValue{"", Poly{0, 0, operand.immediate}};
    }
    if (const auto found = constants.find(operand.value.id); found != constants.end()) {
        return PolyValue{"", Poly{0, 0, found->second}};
    }
    if (const auto found = values.find(operand.value.id); found != values.end()) {
        return found->second;
    }
    return std::nullopt;
}

std::optional<PolyValue> evalPoly(ir::BinaryOpcode op, const PolyValue& lhs, const PolyValue& rhs)
{
    auto combineBases = [](const std::string& lhsBase, const std::string& rhsBase) -> std::optional<std::string> {
        if (lhsBase.empty()) {
            return rhsBase;
        }
        if (rhsBase.empty() || rhsBase == lhsBase) {
            return lhsBase;
        }
        return std::nullopt;
    };

    switch (op) {
    case ir::BinaryOpcode::Add:
        if (const auto base = combineBases(lhs.base, rhs.base); base.has_value()) {
            if (const auto poly = addPoly(lhs.poly, rhs.poly); poly.has_value()) {
                return PolyValue{*base, *poly};
            }
        }
        return std::nullopt;
    case ir::BinaryOpcode::Sub:
        if (!rhs.base.empty()) {
            if (lhs.base != rhs.base) {
                return std::nullopt;
            }
            if (const auto poly = subPoly(lhs.poly, rhs.poly); poly.has_value()) {
                return PolyValue{"", *poly};
            }
            return std::nullopt;
        }
        if (const auto poly = subPoly(lhs.poly, rhs.poly); poly.has_value()) {
            return PolyValue{lhs.base, *poly};
        }
        return std::nullopt;
    case ir::BinaryOpcode::Mul:
        if (!lhs.base.empty() || !rhs.base.empty()) {
            if (!lhs.base.empty() && isConstant(rhs.poly) && rhs.poly.constant == 1 && rhs.base.empty()) {
                return lhs;
            }
            if (!rhs.base.empty() && isConstant(lhs.poly) && lhs.poly.constant == 1 && lhs.base.empty()) {
                return rhs;
            }
            return std::nullopt;
        }
        if (const auto poly = mulPoly(lhs.poly, rhs.poly); poly.has_value()) {
            return PolyValue{"", *poly};
        }
        return std::nullopt;
    default:
        if (lhs.base.empty() && rhs.base.empty() && isConstant(lhs.poly) && isConstant(rhs.poly)) {
            const auto lhsValue = int32Value(lhs.poly.constant);
            const auto rhsValue = int32Value(rhs.poly.constant);
            if (!lhsValue.has_value() || !rhsValue.has_value()) {
                return std::nullopt;
            }
            if (const auto value = evalBinary(op, *lhsValue, *rhsValue); value.has_value()) {
                return PolyValue{"", Poly{0, 0, *value}};
            }
        }
        return std::nullopt;
    }
}

std::optional<std::int32_t> constOf(const ir::Operand& operand, const std::unordered_map<int, std::int32_t>& constants)
{
    if (operand.isImmediate) {
        return operand.immediate;
    }
    const auto found = constants.find(operand.value.id);
    if (found == constants.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<LinearValue> linearOf(
    const ir::Operand& operand,
    const std::unordered_map<int, std::int32_t>& constants,
    const std::unordered_map<int, LinearValue>& linear)
{
    if (operand.isImmediate) {
        return LinearValue{"", operand.immediate};
    }
    if (const auto constIt = constants.find(operand.value.id); constIt != constants.end()) {
        return LinearValue{"", constIt->second};
    }
    if (const auto linearIt = linear.find(operand.value.id); linearIt != linear.end()) {
        return linearIt->second;
    }
    return std::nullopt;
}

bool sameBase(const LinearValue& lhs, const LinearValue& rhs)
{
    return lhs.base == rhs.base;
}

std::optional<LinearValue> evalLinear(
    ir::BinaryOpcode op,
    const LinearValue& lhs,
    const LinearValue& rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
        if (lhs.base.empty()) {
            return LinearValue{rhs.base, rhs.offset + lhs.offset};
        }
        if (rhs.base.empty()) {
            return LinearValue{lhs.base, lhs.offset + rhs.offset};
        }
        if (sameBase(lhs, rhs)) {
            return std::nullopt;
        }
        return std::nullopt;
    case ir::BinaryOpcode::Sub:
        if (rhs.base.empty()) {
            return LinearValue{lhs.base, lhs.offset - rhs.offset};
        }
        if (lhs.base.empty() || !sameBase(lhs, rhs)) {
            return std::nullopt;
        }
        return LinearValue{"", lhs.offset - rhs.offset};
    default:
        if (lhs.base.empty() && rhs.base.empty()) {
            if (const auto value = evalBinary(op, lhs.offset, rhs.offset); value.has_value()) {
                return LinearValue{"", *value};
            }
        }
        return std::nullopt;
    }
}

bool isWhileCond(const ir::BasicBlock& block)
{
    return block.label.find(".while.cond") == 0;
}

bool isSafeBodyInstruction(const ir::Instruction& inst)
{
    switch (inst.kind) {
    case ir::InstructionKind::Const:
    case ir::InstructionKind::Copy:
    case ir::InstructionKind::LoadLocal:
    case ir::InstructionKind::StoreLocal:
    case ir::InstructionKind::Unary:
    case ir::InstructionKind::Binary:
        return true;
    case ir::InstructionKind::StoreGlobal:
    case ir::InstructionKind::LoadGlobal:
    case ir::InstructionKind::Call:
        return false;
    }
    return false;
}

std::optional<int> ceilDivPositive(std::int64_t numerator, std::int64_t denominator)
{
    if (denominator <= 0) {
        return std::nullopt;
    }
    if (numerator <= 0) {
        return 0;
    }
    const std::int64_t value = (numerator + denominator - 1) / denominator;
    if (value > 1000000000LL) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<int> tripCount(ir::BinaryOpcode op, std::int32_t start, std::int32_t bound, std::int32_t step)
{
    if (step == 0) {
        return std::nullopt;
    }
    switch (op) {
    case ir::BinaryOpcode::Less:
        if (step <= 0) {
            return std::nullopt;
        }
        return ceilDivPositive(static_cast<std::int64_t>(bound) - start, step);
    case ir::BinaryOpcode::LessEqual:
        if (step <= 0) {
            return std::nullopt;
        }
        return ceilDivPositive(static_cast<std::int64_t>(bound) - start + 1, step);
    case ir::BinaryOpcode::Greater:
        if (step >= 0) {
            return std::nullopt;
        }
        return ceilDivPositive(static_cast<std::int64_t>(start) - bound, -static_cast<std::int64_t>(step));
    case ir::BinaryOpcode::GreaterEqual:
        if (step >= 0) {
            return std::nullopt;
        }
        return ceilDivPositive(static_cast<std::int64_t>(start) - bound + 1, -static_cast<std::int64_t>(step));
    case ir::BinaryOpcode::NotEqual: {
        const std::int64_t diff = static_cast<std::int64_t>(bound) - start;
        if (diff == 0) {
            return 0;
        }
        if ((diff > 0) != (step > 0) || diff % step != 0) {
            return std::nullopt;
        }
        const std::int64_t value = diff / step;
        if (value <= 0 || value > 1000000000LL) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }
    default:
        return std::nullopt;
    }
}

std::optional<std::int64_t> sumInduction(std::int64_t start, std::int64_t step, std::int64_t trips)
{
    const __int128 total =
        static_cast<__int128>(trips) * start
        + static_cast<__int128>(step) * trips * (trips - 1) / 2;
    return checkedInt64(total);
}

std::optional<std::int64_t> sumInductionSquared(std::int64_t start, std::int64_t step, std::int64_t trips)
{
    const __int128 tri = static_cast<__int128>(trips) * (trips - 1) / 2;
    const __int128 sqSum = static_cast<__int128>(trips) * (trips - 1) * (2 * static_cast<__int128>(trips) - 1) / 6;
    const __int128 total =
        static_cast<__int128>(trips) * start * start
        + 2 * static_cast<__int128>(start) * step * tri
        + static_cast<__int128>(step) * step * sqSum;
    return checkedInt64(total);
}

std::optional<std::int32_t> closedFormSum(
    const Poly& increment,
    std::int32_t start,
    std::int32_t step,
    int trips)
{
    const auto sumI = sumInduction(start, step, trips);
    const auto sumI2 = sumInductionSquared(start, step, trips);
    if (!sumI.has_value() || !sumI2.has_value()) {
        return std::nullopt;
    }

    const __int128 total =
        static_cast<__int128>(increment.quadratic) * *sumI2
        + static_cast<__int128>(increment.linear) * *sumI
        + static_cast<__int128>(increment.constant) * trips;
    if (total < LLONG_MIN || total > LLONG_MAX) {
        return std::nullopt;
    }
    return wrapInt32(static_cast<std::int64_t>(total));
}

std::optional<std::int32_t> closedFormSumNoWrap(
    const Poly& increment,
    std::int32_t start,
    std::int32_t step,
    int trips)
{
    const auto sumI = sumInduction(start, step, trips);
    const auto sumI2 = sumInductionSquared(start, step, trips);
    if (!sumI.has_value() || !sumI2.has_value()) {
        return std::nullopt;
    }

    const __int128 total =
        static_cast<__int128>(increment.quadratic) * *sumI2
        + static_cast<__int128>(increment.linear) * *sumI
        + static_cast<__int128>(increment.constant) * trips;
    if (total < INT32_MIN || total > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(total);
}

std::optional<ir::Operand> resolveCopy(
    ir::Operand operand,
    const std::unordered_map<int, ir::Operand>& copies)
{
    for (int depth = 0; depth < 8 && !operand.isImmediate && operand.value.id >= 0; ++depth) {
        const auto found = copies.find(operand.value.id);
        if (found == copies.end()) {
            break;
        }
        operand = found->second;
    }
    return operand;
}

std::string reloadLocalOf(
    ir::Operand operand,
    const std::unordered_map<int, ir::Operand>& copies,
    const std::unordered_map<int, std::string>& loadedLocal)
{
    const auto resolved = resolveCopy(operand, copies);
    if (!resolved.has_value() || resolved->isImmediate || resolved->value.id < 0) {
        return "";
    }
    const auto found = loadedLocal.find(resolved->value.id);
    if (found == loadedLocal.end()) {
        return "";
    }
    return found->second;
}

struct BinaryDef {
    ir::BinaryOpcode op = ir::BinaryOpcode::Add;
    ir::Operand lhs;
    ir::Operand rhs;
};

struct InvariantOperand {
    ir::Operand operand;
    std::string reloadLocal;
};

bool isLoadOfLocal(
    ir::Operand operand,
    const std::string& symbol,
    const std::unordered_map<int, ir::Operand>& copies,
    const std::unordered_map<int, std::string>& loadedLocal)
{
    const auto resolved = resolveCopy(operand, copies);
    if (!resolved.has_value() || resolved->isImmediate || resolved->value.id < 0) {
        return false;
    }
    const auto found = loadedLocal.find(resolved->value.id);
    return found != loadedLocal.end() && found->second == symbol;
}

std::optional<std::int32_t> constOperandValue(
    ir::Operand operand,
    const std::unordered_map<int, ir::Operand>& copies,
    const std::unordered_map<int, std::int32_t>& constants)
{
    const auto resolved = resolveCopy(operand, copies);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    if (resolved->isImmediate) {
        return resolved->immediate;
    }
    const auto found = constants.find(resolved->value.id);
    if (found == constants.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<InvariantOperand> invariantOperand(
    ir::Operand operand,
    const std::unordered_map<int, ir::Operand>& copies,
    const std::unordered_set<int>& definedInBody,
    const std::unordered_map<int, std::string>& loadedLocal,
    const std::unordered_set<std::string>& storedLocals)
{
    const auto resolved = resolveCopy(operand, copies);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    if (resolved->isImmediate || resolved->value.id < 0) {
        return InvariantOperand{*resolved, ""};
    }
    if (definedInBody.find(resolved->value.id) != definedInBody.end()) {
        const auto loaded = loadedLocal.find(resolved->value.id);
        if (loaded != loadedLocal.end() && storedLocals.find(loaded->second) == storedLocals.end()) {
            return InvariantOperand{ir::Operand{}, loaded->second};
        }
        return std::nullopt;
    }
    return InvariantOperand{*resolved, ""};
}

std::optional<std::int32_t> inductionStepInBody(const ir::BasicBlock& body, const std::string& induction)
{
    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, ir::Operand> copies;
    std::unordered_map<int, std::string> loadedLocal;
    std::unordered_map<int, BinaryDef> binaryDefs;
    std::optional<std::int32_t> step;

    for (const ir::Instruction& inst : body.instructions) {
        if (inst.kind == ir::InstructionKind::StoreGlobal) {
            continue;
        }
        if (inst.kind == ir::InstructionKind::Const
            && inst.dst.id >= 0
            && !inst.operands.empty()
            && inst.operands[0].isImmediate) {
            constants[inst.dst.id] = inst.operands[0].immediate;
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0) {
            loadedLocal[inst.dst.id] = inst.symbol;
            constants.erase(inst.dst.id);
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
            copies[inst.dst.id] = resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]);
            constants.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
            binaryDefs[inst.dst.id] = BinaryDef{
                inst.binaryOp,
                resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]),
                resolveCopy(inst.operands[1], copies).value_or(inst.operands[1]),
            };
            constants.erase(inst.dst.id);
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol != induction || inst.operands.empty()) {
            continue;
        }

        const ir::Operand stored = resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]);
        if (stored.isImmediate || stored.value.id < 0) {
            return std::nullopt;
        }
        const auto def = binaryDefs.find(stored.value.id);
        if (def == binaryDefs.end()) {
            return std::nullopt;
        }

        const bool lhsSelf = isLoadOfLocal(def->second.lhs, induction, copies, loadedLocal);
        const bool rhsSelf = isLoadOfLocal(def->second.rhs, induction, copies, loadedLocal);
        std::optional<std::int32_t> candidate;
        if (def->second.op == ir::BinaryOpcode::Add && lhsSelf) {
            candidate = constOperandValue(def->second.rhs, copies, constants);
        } else if (def->second.op == ir::BinaryOpcode::Add && rhsSelf) {
            candidate = constOperandValue(def->second.lhs, copies, constants);
        } else if (def->second.op == ir::BinaryOpcode::Sub && lhsSelf) {
            if (const auto value = constOperandValue(def->second.rhs, copies, constants); value.has_value()) {
                candidate = -*value;
            }
        }
        if (!candidate.has_value()) {
            return std::nullopt;
        }
        if (step.has_value() && *step != *candidate) {
            return std::nullopt;
        }
        step = *candidate;
    }

    return step;
}

// Locals whose value is observed outside this loop. The loop occupies exactly
// two blocks (header + body); a local is live after the loop if it is loaded by
// any other block. Scanning by block set (not by index order) is required for
// nested loops, where an inner loop's accumulator is consumed by the enclosing
// loop whose blocks may have lower indices than the inner exit.
std::unordered_set<std::string> computeLiveAfterLoop(
    const ir::Function& function,
    int header,
    int bodyIndex)
{
    std::unordered_set<std::string> liveAfterLoop;
    for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
        if (b == header || b == bodyIndex) {
            continue;
        }
        for (const ir::Instruction& inst : function.blocks[static_cast<std::size_t>(b)].instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                liveAfterLoop.insert(inst.symbol);
            }
        }
    }
    return liveAfterLoop;
}

bool tryInvariantAccumulationLoop(
    ir::Function& function,
    int header,
    int exitIndex,
    int bodyIndex,
    ir::BasicBlock& body,
    const std::string& induction,
    std::int32_t start,
    std::int32_t step,
    int trips)
{
    std::unordered_set<int> definedInBody;
    std::unordered_set<std::string> storedLocals;
    for (const ir::Instruction& inst : body.instructions) {
        if (inst.dst.id >= 0) {
            definedInBody.insert(inst.dst.id);
        }
        if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
            storedLocals.insert(inst.symbol);
        }
    }

    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, ir::Operand> copies;
    std::unordered_map<int, std::string> loadedLocal;
    std::unordered_map<int, BinaryDef> binaryDefs;
    std::optional<std::string> accumulator;
    std::optional<InvariantOperand> increment;
    bool sawInductionStore = false;

    for (const ir::Instruction& inst : body.instructions) {
        if (inst.kind == ir::InstructionKind::StoreGlobal) {
            continue;
        }
        if (inst.kind == ir::InstructionKind::Const
            && inst.dst.id >= 0
            && !inst.operands.empty()
            && inst.operands[0].isImmediate) {
            constants[inst.dst.id] = inst.operands[0].immediate;
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0) {
            loadedLocal[inst.dst.id] = inst.symbol;
            constants.erase(inst.dst.id);
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
            copies[inst.dst.id] = resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]);
            constants.erase(inst.dst.id);
            continue;
        }
        if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
            binaryDefs[inst.dst.id] = BinaryDef{
                inst.binaryOp,
                resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]),
                resolveCopy(inst.operands[1], copies).value_or(inst.operands[1]),
            };
            constants.erase(inst.dst.id);
            copies.erase(inst.dst.id);
            continue;
        }
        if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol.empty() || inst.operands.empty()) {
            return false;
        }

        const ir::Operand stored = resolveCopy(inst.operands[0], copies).value_or(inst.operands[0]);
        if (stored.isImmediate || stored.value.id < 0) {
            return false;
        }
        const auto def = binaryDefs.find(stored.value.id);
        if (def == binaryDefs.end()) {
            return false;
        }

        if (inst.symbol == induction) {
            const bool lhsSelf = isLoadOfLocal(def->second.lhs, induction, copies, loadedLocal);
            const bool rhsSelf = isLoadOfLocal(def->second.rhs, induction, copies, loadedLocal);
            std::optional<std::int32_t> delta;
            if (def->second.op == ir::BinaryOpcode::Add && lhsSelf) {
                delta = constOperandValue(def->second.rhs, copies, constants);
            } else if (def->second.op == ir::BinaryOpcode::Add && rhsSelf) {
                delta = constOperandValue(def->second.lhs, copies, constants);
            } else if (def->second.op == ir::BinaryOpcode::Sub && lhsSelf) {
                if (const auto value = constOperandValue(def->second.rhs, copies, constants); value.has_value()) {
                    delta = -*value;
                }
            }
            if (!delta.has_value() || *delta != step) {
                return false;
            }
            sawInductionStore = true;
            continue;
        }

        if (accumulator.has_value()) {
            return false;
        }
        if (def->second.op != ir::BinaryOpcode::Add) {
            return false;
        }
        const bool lhsAcc = isLoadOfLocal(def->second.lhs, inst.symbol, copies, loadedLocal);
        const bool rhsAcc = isLoadOfLocal(def->second.rhs, inst.symbol, copies, loadedLocal);
        if (lhsAcc == rhsAcc) {
            return false;
        }
        const auto candidate = invariantOperand(
            lhsAcc ? def->second.rhs : def->second.lhs,
            copies,
            definedInBody,
            loadedLocal,
            storedLocals);
        if (!candidate.has_value()) {
            return false;
        }
        accumulator = inst.symbol;
        increment = *candidate;
    }

    if (!sawInductionStore || !accumulator.has_value() || !increment.has_value()) {
        return false;
    }

    const std::unordered_set<std::string> liveAfterLoop = computeLiveAfterLoop(function, header, bodyIndex);
    if (liveAfterLoop.find(*accumulator) == liveAfterLoop.end()) {
        return false;
    }

    std::vector<ir::Instruction> replacement;
    ir::Instruction load;
    load.kind = ir::InstructionKind::LoadLocal;
    load.symbol = *accumulator;
    load.dst = ir::Value{function.nextValue++};
    replacement.push_back(load);

    ir::Operand scaled = increment->operand;
    if (!increment->reloadLocal.empty()) {
        ir::Instruction incrementLoad;
        incrementLoad.kind = ir::InstructionKind::LoadLocal;
        incrementLoad.symbol = increment->reloadLocal;
        incrementLoad.dst = ir::Value{function.nextValue++};
        replacement.push_back(incrementLoad);
        scaled = ir::Operand::ref(incrementLoad.dst);
    }
    if (trips != 1) {
        ir::Instruction mul;
        mul.kind = ir::InstructionKind::Binary;
        mul.binaryOp = ir::BinaryOpcode::Mul;
        mul.dst = ir::Value{function.nextValue++};
        mul.operands = {scaled, ir::Operand::imm(trips)};
        replacement.push_back(mul);
        scaled = ir::Operand::ref(mul.dst);
    }

    ir::Instruction add;
    add.kind = ir::InstructionKind::Binary;
    add.binaryOp = ir::BinaryOpcode::Add;
    add.dst = ir::Value{function.nextValue++};
    add.operands = {ir::Operand::ref(load.dst), scaled};
    replacement.push_back(add);

    ir::Instruction store;
    store.kind = ir::InstructionKind::StoreLocal;
    store.symbol = *accumulator;
    store.operands = {ir::Operand::ref(add.dst)};
    replacement.push_back(store);

    ir::Instruction inductionStore;
    inductionStore.kind = ir::InstructionKind::StoreLocal;
    inductionStore.symbol = induction;
    inductionStore.operands = {ir::Operand::imm(static_cast<std::int32_t>(start + static_cast<std::int64_t>(step) * trips))};
    replacement.push_back(inductionStore);

    ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
    mutableHeader.instructions = std::move(replacement);
    mutableHeader.terminator = {};
    mutableHeader.terminator.kind = ir::TerminatorKind::Jump;
    mutableHeader.terminator.trueBlock = exitIndex;
    mutableHeader.hasTerminator = true;
    return true;
}

std::optional<std::int32_t> initialLocalConst(const ir::Function& function, int header, const std::string& symbol)
{
    std::optional<std::int32_t> value;
    std::unordered_map<int, std::int32_t> constants;
    for (int b = 0; b < header; ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            for (const ir::Operand& operand : inst.operands) {
                (void)operand;
            }
            if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
            } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                if (const auto copied = constOf(inst.operands[0], constants); copied.has_value()) {
                    constants[inst.dst.id] = *copied;
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol == symbol && !inst.operands.empty()) {
                value = constOf(inst.operands[0], constants);
            } else if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol == symbol) {
                value.reset();
            } else if (inst.dst.id >= 0) {
                constants.erase(inst.dst.id);
            }
            if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::LoadGlobal) {
                constants.clear();
            }
        }
    }
    return value;
}

std::unordered_map<std::string, std::int32_t> initialLocalConstants(const ir::Function& function, int header)
{
    std::unordered_map<std::string, std::int32_t> values;
    std::unordered_map<std::string, int> storeCounts;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                ++storeCounts[inst.symbol];
            }
        }
    }

    std::unordered_map<int, std::int32_t> constants;
    for (int b = 0; b < header; ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
            } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                if (const auto copied = constOf(inst.operands[0], constants); copied.has_value()) {
                    constants[inst.dst.id] = *copied;
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty() && !inst.operands.empty()) {
                if (const auto value = constOf(inst.operands[0], constants); value.has_value()) {
                    values[inst.symbol] = *value;
                } else {
                    values.erase(inst.symbol);
                }
            } else if (inst.dst.id >= 0) {
                constants.erase(inst.dst.id);
            }
            if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::LoadGlobal) {
                constants.clear();
            }
        }
    }
    for (auto iter = values.begin(); iter != values.end();) {
        const auto found = storeCounts.find(iter->first);
        if (found == storeCounts.end() || found->second != 1) {
            iter = values.erase(iter);
        } else {
            ++iter;
        }
    }
    return values;
}

std::unordered_map<int, std::int32_t> valueConstantsBeforeLoop(
    const ir::Function& function,
    int header,
    const std::unordered_map<std::string, std::int32_t>& localConstants)
{
    std::unordered_map<int, std::int32_t> constants;
    for (int b = 0; b < header; ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
            } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                if (const auto value = constOf(inst.operands[0], constants); value.has_value()) {
                    constants[inst.dst.id] = *value;
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.kind == ir::InstructionKind::Unary && inst.dst.id >= 0 && inst.operands.size() == 1) {
                if (const auto value = constOf(inst.operands[0], constants); value.has_value()) {
                    constants[inst.dst.id] = evalUnary(inst.unaryOp, *value);
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
                const auto lhs = constOf(inst.operands[0], constants);
                const auto rhs = constOf(inst.operands[1], constants);
                if (lhs.has_value() && rhs.has_value()) {
                    if (const auto value = evalBinary(inst.binaryOp, *lhs, *rhs); value.has_value()) {
                        constants[inst.dst.id] = *value;
                    } else {
                        constants.erase(inst.dst.id);
                    }
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0) {
                if (const auto value = localConstants.find(inst.symbol); value != localConstants.end()) {
                    constants[inst.dst.id] = value->second;
                } else {
                    constants.erase(inst.dst.id);
                }
            } else if (inst.dst.id >= 0) {
                constants.erase(inst.dst.id);
            }
        }
    }
    return constants;
}

bool runOnLoop(ir::Function& function, int header)
{
    if (header < 0 || header >= static_cast<int>(function.blocks.size())) {
        return false;
    }
    const ir::BasicBlock& headerBlock = function.blocks[static_cast<std::size_t>(header)];
    if (!isWhileCond(headerBlock) || headerBlock.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    const int bodyIndex = headerBlock.terminator.trueBlock;
    const int exitIndex = headerBlock.terminator.falseBlock;
    if (bodyIndex < 0 || bodyIndex >= static_cast<int>(function.blocks.size()) || exitIndex < 0) {
        return false;
    }
    ir::BasicBlock& body = function.blocks[static_cast<std::size_t>(bodyIndex)];
    if (body.terminator.kind != ir::TerminatorKind::Jump || body.terminator.trueBlock != header) {
        return false;
    }
    for (const ir::Instruction& inst : body.instructions) {
        if (!isSafeBodyInstruction(inst)) {
            return false;
        }
    }
    std::unordered_set<std::string> bodyStoredLocals;
    for (const ir::Instruction& inst : body.instructions) {
        if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
            bodyStoredLocals.insert(inst.symbol);
        }
    }
    const std::unordered_map<std::string, std::int32_t> localConstants = initialLocalConstants(function, header);

    std::unordered_map<int, std::string> loadedLocal;
    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, LinearValue> linear;
    std::unordered_map<int, ir::Operand> headerCopies;
    std::optional<ir::BinaryOpcode> cmpOp;
    std::optional<std::string> induction;
    std::optional<std::int32_t> bound;

    for (const ir::Instruction& inst : headerBlock.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
                linear[inst.dst.id] = LinearValue{"", inst.operands[0].immediate};
                headerCopies.erase(inst.dst.id);
            }
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id >= 0) {
                headerCopies.erase(inst.dst.id);
                if (bodyStoredLocals.find(inst.symbol) == bodyStoredLocals.end()) {
                    if (const auto value = localConstants.find(inst.symbol); value != localConstants.end()) {
                        constants[inst.dst.id] = value->second;
                        linear[inst.dst.id] = LinearValue{"", value->second};
                        break;
                    }
                }
                loadedLocal[inst.dst.id] = inst.symbol;
                linear[inst.dst.id] = LinearValue{inst.symbol, 0};
                constants.erase(inst.dst.id);
            }
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id >= 0 && !inst.operands.empty()) {
                headerCopies[inst.dst.id] = resolveCopy(inst.operands[0], headerCopies).value_or(inst.operands[0]);
                if (const auto value = constOf(inst.operands[0], constants); value.has_value()) {
                    constants[inst.dst.id] = *value;
                }
                if (const auto value = linearOf(inst.operands[0], constants, linear); value.has_value()) {
                    linear[inst.dst.id] = *value;
                }
            }
            break;
        case ir::InstructionKind::Binary:
            if (inst.dst.id >= 0 && inst.operands.size() == 2) {
                headerCopies.erase(inst.dst.id);
                const bool feedsBranch = !headerBlock.terminator.condition.isImmediate
                    && headerBlock.terminator.condition.value == inst.dst;
                if (feedsBranch) {
                    const auto lhs = linearOf(inst.operands[0], constants, linear);
                    const auto rhs = linearOf(inst.operands[1], constants, linear);
                    if (!lhs.has_value() || !rhs.has_value()) {
                        return false;
                    }
                    if (!lhs->base.empty() && rhs->base.empty()) {
                        induction = lhs->base;
                        bound = rhs->offset;
                        cmpOp = inst.binaryOp;
                    } else if (lhs->base.empty() && !rhs->base.empty()) {
                        induction = rhs->base;
                        bound = lhs->offset;
                        switch (inst.binaryOp) {
                        case ir::BinaryOpcode::Greater:
                            cmpOp = ir::BinaryOpcode::Less;
                            break;
                        case ir::BinaryOpcode::GreaterEqual:
                            cmpOp = ir::BinaryOpcode::LessEqual;
                            break;
                        case ir::BinaryOpcode::Less:
                            cmpOp = ir::BinaryOpcode::Greater;
                            break;
                        case ir::BinaryOpcode::LessEqual:
                            cmpOp = ir::BinaryOpcode::GreaterEqual;
                            break;
                        case ir::BinaryOpcode::NotEqual:
                            cmpOp = ir::BinaryOpcode::NotEqual;
                            break;
                        default:
                            return false;
                        }
                    } else {
                        return false;
                    }
                }
                if (const auto lhs = linearOf(inst.operands[0], constants, linear), rhs = linearOf(inst.operands[1], constants, linear);
                    lhs.has_value() && rhs.has_value()) {
                    if (const auto folded = evalLinear(inst.binaryOp, *lhs, *rhs); folded.has_value()) {
                        linear[inst.dst.id] = *folded;
                        if (folded->base.empty()) {
                            constants[inst.dst.id] = folded->offset;
                        }
                    }
                }
            }
            break;
        default:
            return false;
        }
    }

    if (!cmpOp.has_value() || !induction.has_value() || !bound.has_value()) {
        return false;
    }
    const auto start = initialLocalConst(function, header, *induction);
    if (!start.has_value()) {
        return false;
    }
    if (bound.has_value()) {
        const auto bodyStep = inductionStepInBody(body, *induction);
        if (bodyStep.has_value()) {
            if (const auto fixedTrips = tripCount(*cmpOp, *start, *bound, *bodyStep); fixedTrips.has_value()) {
                if (tryInvariantAccumulationLoop(
                        function,
                        header,
                        exitIndex,
                        bodyIndex,
                        body,
                        *induction,
                        *start,
                        *bodyStep,
                        *fixedTrips)) {
                    return true;
                }
            }
        }
    }

    constants = valueConstantsBeforeLoop(function, header, localConstants);
    std::unordered_map<int, PolyValue> polyValues;
    for (const auto& [valueId, value] : constants) {
        polyValues[valueId] = PolyValue{"", Poly{0, 0, value}};
    }
    std::unordered_map<std::string, PolyValue> currentLocal;
    std::unordered_map<std::string, PolyValue> finalLocal;
    for (const ir::Instruction& inst : body.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
                polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, inst.operands[0].immediate}};
            }
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id >= 0) {
                if (const auto current = currentLocal.find(inst.symbol); current != currentLocal.end()) {
                    polyValues[inst.dst.id] = current->second;
                    if (current->second.base.empty() && isConstant(current->second.poly)) {
                        if (const auto constant = int32Value(current->second.poly.constant); constant.has_value()) {
                            constants[inst.dst.id] = *constant;
                        }
                    }
                    break;
                }
                if (bodyStoredLocals.find(inst.symbol) == bodyStoredLocals.end()) {
                    if (const auto value = localConstants.find(inst.symbol); value != localConstants.end()) {
                        constants[inst.dst.id] = value->second;
                        polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, value->second}};
                        break;
                    }
                }
                if (inst.symbol == *induction) {
                    polyValues[inst.dst.id] = PolyValue{"", Poly{0, 1, 0}};
                } else {
                    polyValues[inst.dst.id] = PolyValue{inst.symbol, Poly{0, 0, 0}};
                }
            }
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id >= 0 && !inst.operands.empty()) {
                if (const auto value = polyOf(inst.operands[0], constants, polyValues); value.has_value()) {
                    polyValues[inst.dst.id] = *value;
                    if (value->base.empty() && isConstant(value->poly)) {
                        if (const auto constant = int32Value(value->poly.constant); constant.has_value()) {
                            constants[inst.dst.id] = *constant;
                        }
                    }
                } else {
                    return false;
                }
            }
            break;
        case ir::InstructionKind::Binary:
            if (inst.dst.id >= 0 && inst.operands.size() == 2) {
                const auto lhs = polyOf(inst.operands[0], constants, polyValues);
                const auto rhs = polyOf(inst.operands[1], constants, polyValues);
                if (!lhs.has_value() || !rhs.has_value()) {
                    return false;
                }
                const auto folded = evalPoly(inst.binaryOp, *lhs, *rhs);
                if (!folded.has_value()) {
                    return false;
                }
                polyValues[inst.dst.id] = *folded;
                if (folded->base.empty() && isConstant(folded->poly)) {
                    if (const auto constant = int32Value(folded->poly.constant); constant.has_value()) {
                        constants[inst.dst.id] = *constant;
                    }
                }
            }
            break;
        case ir::InstructionKind::StoreLocal:
            if (inst.operands.empty()) {
                return false;
            }
            if (const auto value = polyOf(inst.operands[0], constants, polyValues); value.has_value()) {
                currentLocal[inst.symbol] = *value;
                finalLocal[inst.symbol] = *value;
            } else {
                return false;
            }
            break;
        default:
            return false;
        }
    }

    const auto inductionFinal = finalLocal.find(*induction);
    if (inductionFinal == finalLocal.end()
        || !inductionFinal->second.base.empty()
        || inductionFinal->second.poly.quadratic != 0
        || inductionFinal->second.poly.linear != 1) {
        return false;
    }
    const auto stepValue = int32Value(inductionFinal->second.poly.constant);
    if (!stepValue.has_value()) {
        return false;
    }
    const std::int32_t step = *stepValue;
    const std::optional<int> trips = tripCount(*cmpOp, *start, *bound, step);
    if (!trips.has_value()) {
        return false;
    }
    if (tryInvariantAccumulationLoop(function, header, exitIndex, bodyIndex, body, *induction, *start, step, *trips)) {
        return true;
    }

    std::vector<ir::Instruction> replacement;
    bool changedAccumulator = false;
    const std::unordered_set<std::string> liveAfterLoop = computeLiveAfterLoop(function, header, bodyIndex);

    for (const auto& [symbol, value] : finalLocal) {
        if (symbol == *induction) {
            continue;
        }
        if (liveAfterLoop.find(symbol) == liveAfterLoop.end()) {
            continue;
        }
        if (value.base == symbol && !isZero(value.poly)) {
            const auto total = closedFormSum(value.poly, *start, step, *trips);
            if (!total.has_value()) {
                return false;
            }
            ir::Instruction load;
            load.kind = ir::InstructionKind::LoadLocal;
            load.symbol = symbol;
            load.dst = ir::Value{function.nextValue++};
            replacement.push_back(load);

            ir::Instruction add;
            add.kind = ir::InstructionKind::Binary;
            add.binaryOp = ir::BinaryOpcode::Add;
            add.dst = ir::Value{function.nextValue++};
            add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(*total)};
            replacement.push_back(add);

            ir::Instruction store;
            store.kind = ir::InstructionKind::StoreLocal;
            store.symbol = symbol;
            store.operands = {ir::Operand::ref(add.dst)};
            replacement.push_back(store);
            changedAccumulator = true;
        } else {
            return false;
        }
    }
    if (!changedAccumulator) {
        return false;
    }

    ir::Instruction inductionStore;
    inductionStore.kind = ir::InstructionKind::StoreLocal;
    inductionStore.symbol = *induction;
    inductionStore.operands = {ir::Operand::imm(static_cast<std::int32_t>(*start + static_cast<std::int64_t>(step) * *trips))};
    replacement.push_back(inductionStore);

    ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
    mutableHeader.instructions = std::move(replacement);
    mutableHeader.terminator = {};
    mutableHeader.terminator.kind = ir::TerminatorKind::Jump;
    mutableHeader.terminator.trueBlock = exitIndex;
    mutableHeader.hasTerminator = true;
    return true;
}

// A comparison `local <op> const` recovered from the block that computes a
// branch condition. `local` is on the left after normalization.
struct CompareInfo {
    std::string local;
    std::int32_t bound = 0;
    ir::BinaryOpcode op = ir::BinaryOpcode::Less;
};

std::optional<CompareInfo> analyzeCompare(const ir::BasicBlock& block, const ir::Operand& cond)
{
    if (cond.isImmediate || cond.value.id < 0) {
        return std::nullopt;
    }
    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, std::string> loadLocalOf;
    struct Bin { ir::BinaryOpcode op; ir::Operand a; ir::Operand b; };
    std::unordered_map<int, Bin> bins;
    for (const ir::Instruction& inst : block.instructions) {
        if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
            constants[inst.dst.id] = inst.operands[0].immediate;
        } else if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0) {
            loadLocalOf[inst.dst.id] = inst.symbol;
        } else if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
            bins[inst.dst.id] = Bin{inst.binaryOp, inst.operands[0], inst.operands[1]};
        }
    }
    const auto it = bins.find(cond.value.id);
    if (it == bins.end()) {
        return std::nullopt;
    }
    auto asConst = [&](const ir::Operand& o) -> std::optional<std::int32_t> {
        if (o.isImmediate) return o.immediate;
        const auto c = constants.find(o.value.id);
        return c != constants.end() ? std::optional<std::int32_t>(c->second) : std::nullopt;
    };
    auto asLocal = [&](const ir::Operand& o) -> std::optional<std::string> {
        if (o.isImmediate || o.value.id < 0) return std::nullopt;
        const auto l = loadLocalOf.find(o.value.id);
        return l != loadLocalOf.end() ? std::optional<std::string>(l->second) : std::nullopt;
    };
    if (const auto localL = asLocal(it->second.a), r = std::optional<std::string>{}; localL.has_value()) {
        if (const auto rc = asConst(it->second.b); rc.has_value()) {
            return CompareInfo{*localL, *rc, it->second.op};
        }
    }
    const auto lc = asConst(it->second.a);
    const auto rl = asLocal(it->second.b);
    if (lc.has_value() && rl.has_value()) {
        ir::BinaryOpcode sw;
        switch (it->second.op) {
        case ir::BinaryOpcode::Less: sw = ir::BinaryOpcode::Greater; break;
        case ir::BinaryOpcode::LessEqual: sw = ir::BinaryOpcode::GreaterEqual; break;
        case ir::BinaryOpcode::Greater: sw = ir::BinaryOpcode::Less; break;
        case ir::BinaryOpcode::GreaterEqual: sw = ir::BinaryOpcode::LessEqual; break;
        default: return std::nullopt;
        }
        return CompareInfo{*rl, *lc, sw};
    }
    return std::nullopt;
}

// Increment poly of a single-accumulation branch block: `acc = acc + poly(ind)`
// and nothing else observable. Returns Poly{0} for a branch that leaves acc
// untouched (e.g. an empty else). The accumulator is modelled as base "__acc__".
std::optional<Poly> branchAccIncrement(const ir::BasicBlock& block, const std::string& induction, const std::string& acc)
{
    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, PolyValue> polyValues;
    std::optional<Poly> increment;
    bool sawAccStore = false;
    for (const ir::Instruction& inst : block.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
                polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, inst.operands[0].immediate}};
            } else {
                return std::nullopt;
            }
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id < 0) return std::nullopt;
            if (inst.symbol == acc) {
                polyValues[inst.dst.id] = PolyValue{"__acc__", Poly{0, 0, 0}};
            } else if (inst.symbol == induction) {
                polyValues[inst.dst.id] = PolyValue{"", Poly{0, 1, 0}};
            } else {
                return std::nullopt;
            }
            break;
        case ir::InstructionKind::Copy: {
            if (inst.dst.id < 0 || inst.operands.empty()) return std::nullopt;
            const auto v = polyOf(inst.operands[0], constants, polyValues);
            if (!v.has_value()) return std::nullopt;
            polyValues[inst.dst.id] = *v;
            break;
        }
        case ir::InstructionKind::Binary: {
            if (inst.dst.id < 0 || inst.operands.size() != 2) return std::nullopt;
            const auto a = polyOf(inst.operands[0], constants, polyValues);
            const auto b = polyOf(inst.operands[1], constants, polyValues);
            if (!a.has_value() || !b.has_value()) return std::nullopt;
            const auto r = evalPoly(inst.binaryOp, *a, *b);
            if (!r.has_value()) return std::nullopt;
            polyValues[inst.dst.id] = *r;
            break;
        }
        case ir::InstructionKind::StoreLocal: {
            if (inst.symbol != acc || inst.operands.empty()) return std::nullopt;
            if (sawAccStore) return std::nullopt;
            const auto v = polyOf(inst.operands[0], constants, polyValues);
            if (!v.has_value() || v->base != "__acc__") return std::nullopt;
            increment = v->poly;
            sawAccStore = true;
            break;
        }
        default:
            return std::nullopt;
        }
    }
    if (!sawAccStore) {
        return Poly{0, 0, 0};
    }
    return increment;
}

// Close a counted loop whose body is a single if/else diamond, each branch a
// pure accumulation `acc += poly(i)`, and the if-condition is `i <op> K` (K a
// compile-time constant) that splits the iteration range. Backend-safe: emits
// the same closed-form store loop-sum already produces.
bool tryConditionalAccumulationLoop(ir::Function& function, int header)
{
    const int n = static_cast<int>(function.blocks.size());
    if (header < 0 || header >= n) {
        return false;
    }
    const ir::BasicBlock& H = function.blocks[static_cast<std::size_t>(header)];
    if (!isWhileCond(H) || H.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    const int ifIdx = H.terminator.trueBlock;
    const int exitIdx = H.terminator.falseBlock;
    if (ifIdx < 0 || ifIdx >= n || exitIdx < 0) {
        return false;
    }
    const ir::BasicBlock& IF = function.blocks[static_cast<std::size_t>(ifIdx)];
    if (IF.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    for (const ir::Instruction& inst : IF.instructions) {
        if (inst.kind == ir::InstructionKind::StoreLocal || inst.kind == ir::InstructionKind::StoreGlobal
            || inst.kind == ir::InstructionKind::LoadGlobal || inst.kind == ir::InstructionKind::Call) {
            return false;
        }
    }
    const int thenIdx = IF.terminator.trueBlock;
    const int elseIdx = IF.terminator.falseBlock;
    if (thenIdx < 0 || elseIdx < 0 || thenIdx >= n || elseIdx >= n || thenIdx == elseIdx) {
        return false;
    }
    const ir::BasicBlock& THEN = function.blocks[static_cast<std::size_t>(thenIdx)];
    const ir::BasicBlock& ELSE = function.blocks[static_cast<std::size_t>(elseIdx)];
    if (THEN.terminator.kind != ir::TerminatorKind::Jump || ELSE.terminator.kind != ir::TerminatorKind::Jump) {
        return false;
    }
    const int latchIdx = THEN.terminator.trueBlock;
    if (ELSE.terminator.trueBlock != latchIdx || latchIdx < 0 || latchIdx >= n) {
        return false;
    }
    const ir::BasicBlock& LATCH = function.blocks[static_cast<std::size_t>(latchIdx)];
    if (LATCH.terminator.kind != ir::TerminatorKind::Jump || LATCH.terminator.trueBlock != header) {
        return false;
    }

    const auto hdr = analyzeCompare(H, H.terminator.condition);
    if (!hdr.has_value()) {
        return false;
    }
    const std::string induction = hdr->local;
    const auto step = inductionStepInBody(LATCH, induction);
    if (!step.has_value() || *step <= 0) {
        return false;
    }
    for (const ir::Instruction& inst : LATCH.instructions) {
        if (inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::LoadGlobal || inst.kind == ir::InstructionKind::Call) {
            return false;
        }
        if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol != induction) {
            return false;
        }
    }
    const auto ifc = analyzeCompare(IF, IF.terminator.condition);
    if (!ifc.has_value() || ifc->local != induction) {
        return false;
    }
    const auto start = initialLocalConst(function, header, induction);
    if (!start.has_value()) {
        return false;
    }
    const auto total = tripCount(hdr->op, *start, hdr->bound, *step);
    if (!total.has_value()) {
        return false;
    }

    // Accumulator symbol: the single local stored across the two branches.
    std::string acc;
    for (const ir::BasicBlock* b : {&THEN, &ELSE}) {
        for (const ir::Instruction& inst : b->instructions) {
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                if (!acc.empty() && acc != inst.symbol) {
                    return false;
                }
                acc = inst.symbol;
            }
        }
    }
    if (acc.empty() || acc == induction) {
        return false;
    }
    const auto thenInc = branchAccIncrement(THEN, induction, acc);
    const auto elseInc = branchAccIncrement(ELSE, induction, acc);
    if (!thenInc.has_value() || !elseInc.has_value()) {
        return false;
    }

    const int totalTrips = *total;
    auto clampTrips = [&](std::optional<int> t) {
        int v = t.value_or(0);
        if (v < 0) v = 0;
        if (v > totalTrips) v = totalTrips;
        return v;
    };
    int trueTrips = 0;
    bool trueIsLow = true;
    switch (ifc->op) {
    case ir::BinaryOpcode::Less:
        trueTrips = clampTrips(tripCount(ir::BinaryOpcode::Less, *start, ifc->bound, *step));
        trueIsLow = true;
        break;
    case ir::BinaryOpcode::LessEqual:
        trueTrips = clampTrips(tripCount(ir::BinaryOpcode::LessEqual, *start, ifc->bound, *step));
        trueIsLow = true;
        break;
    case ir::BinaryOpcode::Greater:
        trueTrips = totalTrips - clampTrips(tripCount(ir::BinaryOpcode::LessEqual, *start, ifc->bound, *step));
        trueIsLow = false;
        break;
    case ir::BinaryOpcode::GreaterEqual:
        trueTrips = totalTrips - clampTrips(tripCount(ir::BinaryOpcode::Less, *start, ifc->bound, *step));
        trueIsLow = false;
        break;
    default:
        return false;
    }
    if (trueTrips < 0 || trueTrips > totalTrips) {
        return false;
    }

    // Sum each branch over its contiguous sub-range of iterations.
    std::optional<std::int32_t> sumThen;
    std::optional<std::int32_t> sumElse;
    if (trueIsLow) {
        const int lowT = trueTrips;
        const int highT = totalTrips - trueTrips;
        sumThen = closedFormSum(*thenInc, *start, *step, lowT);
        sumElse = closedFormSum(*elseInc, static_cast<std::int32_t>(*start + static_cast<std::int64_t>(*step) * lowT), *step, highT);
    } else {
        const int falseT = totalTrips - trueTrips;
        const int highT = trueTrips;
        sumElse = closedFormSum(*elseInc, *start, *step, falseT);
        sumThen = closedFormSum(*thenInc, static_cast<std::int32_t>(*start + static_cast<std::int64_t>(*step) * falseT), *step, highT);
    }
    if (!sumThen.has_value() || !sumElse.has_value()) {
        return false;
    }
    const std::int32_t incTotal = wrapInt32(static_cast<std::int64_t>(*sumThen) + *sumElse);

    // Emit `acc = acc + incTotal; induction = final;` in the header, jump to exit.
    std::vector<ir::Instruction> replacement;
    ir::Instruction load;
    load.kind = ir::InstructionKind::LoadLocal;
    load.symbol = acc;
    load.dst = ir::Value{function.nextValue++};
    replacement.push_back(load);

    ir::Instruction add;
    add.kind = ir::InstructionKind::Binary;
    add.binaryOp = ir::BinaryOpcode::Add;
    add.dst = ir::Value{function.nextValue++};
    add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(incTotal)};
    replacement.push_back(add);

    ir::Instruction store;
    store.kind = ir::InstructionKind::StoreLocal;
    store.symbol = acc;
    store.operands = {ir::Operand::ref(add.dst)};
    replacement.push_back(store);

    ir::Instruction indStore;
    indStore.kind = ir::InstructionKind::StoreLocal;
    indStore.symbol = induction;
    indStore.operands = {ir::Operand::imm(static_cast<std::int32_t>(*start + static_cast<std::int64_t>(*step) * totalTrips))};
    replacement.push_back(indStore);

    ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
    mutableHeader.instructions = std::move(replacement);
    mutableHeader.terminator = {};
    mutableHeader.terminator.kind = ir::TerminatorKind::Jump;
    mutableHeader.terminator.trueBlock = exitIdx;
    mutableHeader.hasTerminator = true;
    return true;
}

struct ModuloTerm {
    Poly poly;
    std::int32_t modulus = 1;
};

struct ModuloAccumulation {
    std::string accumulator;
    ModuloTerm term;
};

struct ModuloReducedAccumulation {
    std::string accumulator;
    Poly increment;
    std::int32_t modulus = 1;
};

std::optional<ModuloTerm> moduloOf(
    const ir::Operand& operand,
    const std::unordered_map<int, ModuloTerm>& moduloValues)
{
    if (operand.isImmediate || operand.value.id < 0) {
        return std::nullopt;
    }
    const auto found = moduloValues.find(operand.value.id);
    if (found == moduloValues.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<std::string> pureLocalBaseOf(
    const ir::Operand& operand,
    const std::unordered_map<int, std::int32_t>& constants,
    const std::unordered_map<int, PolyValue>& polyValues)
{
    const auto value = polyOf(operand, constants, polyValues);
    if (!value.has_value() || value->base.empty() || !isZero(value->poly)) {
        return std::nullopt;
    }
    return value->base;
}

std::int32_t evalPolyInt32(const Poly& poly, std::int64_t x)
{
    const __int128 value =
        static_cast<__int128>(poly.quadratic) * x * x
        + static_cast<__int128>(poly.linear) * x
        + static_cast<__int128>(poly.constant);
    return wrapInt32Wide(value);
}

std::optional<std::int32_t> closedFormModuloSum(
    const ModuloTerm& term,
    std::int32_t start,
    std::int32_t step,
    int trips)
{
    if (term.modulus <= 0 || term.modulus > 256 || trips < 0) {
        return std::nullopt;
    }

    const int period = term.modulus;
    std::vector<std::int32_t> prefix(static_cast<std::size_t>(period) + 1, 0);
    std::int64_t cycleSum = 0;
    for (int t = 0; t < period; ++t) {
        const std::int64_t x = static_cast<std::int64_t>(start) + static_cast<std::int64_t>(step) * t;
        const std::int32_t value = evalPolyInt32(term.poly, x);
        const std::int32_t delta = value % term.modulus;
        cycleSum += delta;
        prefix[static_cast<std::size_t>(t + 1)] = wrapInt32(static_cast<std::int64_t>(prefix[static_cast<std::size_t>(t)]) + delta);
    }

    const std::int64_t fullCycles = trips / period;
    const int remainder = trips % period;
    const __int128 total =
        static_cast<__int128>(fullCycles) * cycleSum
        + prefix[static_cast<std::size_t>(remainder)];
    return wrapInt32Wide(total);
}

bool tryModuloAccumulationLoop(ir::Function& function, int header)
{
    if (header < 0 || header >= static_cast<int>(function.blocks.size())) {
        return false;
    }
    const ir::BasicBlock& headerBlock = function.blocks[static_cast<std::size_t>(header)];
    if (!isWhileCond(headerBlock) || headerBlock.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    const int bodyIndex = headerBlock.terminator.trueBlock;
    const int exitIndex = headerBlock.terminator.falseBlock;
    if (bodyIndex < 0 || bodyIndex >= static_cast<int>(function.blocks.size()) || exitIndex < 0) {
        return false;
    }
    ir::BasicBlock& body = function.blocks[static_cast<std::size_t>(bodyIndex)];
    if (body.terminator.kind != ir::TerminatorKind::Jump || body.terminator.trueBlock != header) {
        return false;
    }
    for (const ir::Instruction& inst : body.instructions) {
        if (!isSafeBodyInstruction(inst)) {
            return false;
        }
    }

    const auto compare = analyzeCompare(headerBlock, headerBlock.terminator.condition);
    if (!compare.has_value()) {
        return false;
    }
    const std::string induction = compare->local;
    const auto start = initialLocalConst(function, header, induction);
    const auto step = inductionStepInBody(body, induction);
    if (!start.has_value() || !step.has_value() || *step <= 0) {
        return false;
    }
    const auto trips = tripCount(compare->op, *start, compare->bound, *step);
    if (!trips.has_value()) {
        return false;
    }

    std::unordered_set<std::string> bodyStoredLocals;
    for (const ir::Instruction& inst : body.instructions) {
        if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
            bodyStoredLocals.insert(inst.symbol);
        }
    }
    const std::unordered_map<std::string, std::int32_t> localConstants = initialLocalConstants(function, header);
    std::unordered_map<int, std::int32_t> constants = valueConstantsBeforeLoop(function, header, localConstants);
    std::unordered_map<int, PolyValue> polyValues;
    for (const auto& [valueId, value] : constants) {
        polyValues[valueId] = PolyValue{"", Poly{0, 0, value}};
    }
    std::unordered_map<int, ModuloTerm> moduloValues;
    std::unordered_map<int, ModuloAccumulation> accumulationValues;
    std::unordered_map<int, ModuloReducedAccumulation> reducedAccumulationValues;
    std::unordered_map<std::string, ModuloTerm> accumulations;
    std::unordered_map<std::string, ModuloReducedAccumulation> reducedAccumulations;
    bool sawInductionStore = false;

    auto rememberConstant = [&](int valueId, const PolyValue& value) {
        if (value.base.empty() && isConstant(value.poly)) {
            if (const auto constant = int32Value(value.poly.constant); constant.has_value()) {
                constants[valueId] = *constant;
            }
        }
    };

    for (const ir::Instruction& inst : body.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id < 0 || inst.operands.empty() || !inst.operands[0].isImmediate) {
                return false;
            }
            constants[inst.dst.id] = inst.operands[0].immediate;
            polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, inst.operands[0].immediate}};
            moduloValues.erase(inst.dst.id);
            accumulationValues.erase(inst.dst.id);
            reducedAccumulationValues.erase(inst.dst.id);
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id < 0) {
                return false;
            }
            moduloValues.erase(inst.dst.id);
            accumulationValues.erase(inst.dst.id);
            reducedAccumulationValues.erase(inst.dst.id);
            if (bodyStoredLocals.find(inst.symbol) == bodyStoredLocals.end()) {
                if (const auto value = localConstants.find(inst.symbol); value != localConstants.end()) {
                    constants[inst.dst.id] = value->second;
                    polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, value->second}};
                    break;
                }
            }
            constants.erase(inst.dst.id);
            if (inst.symbol == induction) {
                polyValues[inst.dst.id] = PolyValue{"", Poly{0, 1, 0}};
            } else {
                polyValues[inst.dst.id] = PolyValue{inst.symbol, Poly{0, 0, 0}};
            }
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id < 0 || inst.operands.empty()) {
                return false;
            }
            constants.erase(inst.dst.id);
            polyValues.erase(inst.dst.id);
            moduloValues.erase(inst.dst.id);
            accumulationValues.erase(inst.dst.id);
            reducedAccumulationValues.erase(inst.dst.id);
            if (const auto value = polyOf(inst.operands[0], constants, polyValues); value.has_value()) {
                polyValues[inst.dst.id] = *value;
                rememberConstant(inst.dst.id, *value);
            } else if (const auto value = moduloOf(inst.operands[0], moduloValues); value.has_value()) {
                moduloValues[inst.dst.id] = *value;
            } else if (!inst.operands[0].isImmediate) {
                const auto found = accumulationValues.find(inst.operands[0].value.id);
                if (found != accumulationValues.end()) {
                    accumulationValues[inst.dst.id] = found->second;
                } else {
                    const auto reducedFound = reducedAccumulationValues.find(inst.operands[0].value.id);
                    if (reducedFound != reducedAccumulationValues.end()) {
                        reducedAccumulationValues[inst.dst.id] = reducedFound->second;
                    } else {
                        return false;
                    }
                }
            } else {
                return false;
            }
            break;
        case ir::InstructionKind::Binary:
            if (inst.dst.id < 0 || inst.operands.size() != 2) {
                return false;
            }
            constants.erase(inst.dst.id);
            polyValues.erase(inst.dst.id);
            moduloValues.erase(inst.dst.id);
            accumulationValues.erase(inst.dst.id);
            reducedAccumulationValues.erase(inst.dst.id);
            if (inst.binaryOp == ir::BinaryOpcode::Mod) {
                const auto lhs = polyOf(inst.operands[0], constants, polyValues);
                const auto rhs = constOf(inst.operands[1], constants);
                if (!lhs.has_value() || !rhs.has_value() || *rhs <= 0) {
                    return false;
                }
                if (lhs->base.empty()) {
                    if (*rhs > 256) {
                        return false;
                    }
                    moduloValues[inst.dst.id] = ModuloTerm{lhs->poly, *rhs};
                } else {
                    if (lhs->base == induction || isZero(lhs->poly)) {
                        return false;
                    }
                    reducedAccumulationValues[inst.dst.id] = ModuloReducedAccumulation{lhs->base, lhs->poly, *rhs};
                }
                break;
            }
            if (inst.binaryOp == ir::BinaryOpcode::Add) {
                const auto lhsMod = moduloOf(inst.operands[0], moduloValues);
                const auto rhsMod = moduloOf(inst.operands[1], moduloValues);
                const auto lhsAcc = pureLocalBaseOf(inst.operands[0], constants, polyValues);
                const auto rhsAcc = pureLocalBaseOf(inst.operands[1], constants, polyValues);
                if (lhsAcc.has_value() && rhsMod.has_value() && *lhsAcc != induction) {
                    accumulationValues[inst.dst.id] = ModuloAccumulation{*lhsAcc, *rhsMod};
                    break;
                }
                if (rhsAcc.has_value() && lhsMod.has_value() && *rhsAcc != induction) {
                    accumulationValues[inst.dst.id] = ModuloAccumulation{*rhsAcc, *lhsMod};
                    break;
                }
            }
            if (const auto lhs = polyOf(inst.operands[0], constants, polyValues), rhs = polyOf(inst.operands[1], constants, polyValues);
                lhs.has_value() && rhs.has_value()) {
                if (const auto folded = evalPoly(inst.binaryOp, *lhs, *rhs); folded.has_value()) {
                    polyValues[inst.dst.id] = *folded;
                    rememberConstant(inst.dst.id, *folded);
                    break;
                }
            }
            return false;
        case ir::InstructionKind::StoreLocal:
            if (inst.operands.empty()) {
                return false;
            }
            if (inst.symbol == induction) {
                sawInductionStore = true;
                break;
            }
            if (inst.operands[0].isImmediate || inst.operands[0].value.id < 0) {
                return false;
            }
            if (const auto found = accumulationValues.find(inst.operands[0].value.id);
                found != accumulationValues.end() && found->second.accumulator == inst.symbol) {
                if (accumulations.find(inst.symbol) != accumulations.end()) {
                    return false;
                }
                if (reducedAccumulations.find(inst.symbol) != reducedAccumulations.end()) {
                    return false;
                }
                accumulations.emplace(inst.symbol, found->second.term);
                break;
            }
            if (const auto found = reducedAccumulationValues.find(inst.operands[0].value.id);
                found != reducedAccumulationValues.end() && found->second.accumulator == inst.symbol) {
                if (accumulations.find(inst.symbol) != accumulations.end()
                    || reducedAccumulations.find(inst.symbol) != reducedAccumulations.end()) {
                    return false;
                }
                reducedAccumulations.emplace(inst.symbol, found->second);
                break;
            }
            return false;
        default:
            return false;
        }
    }

    if (!sawInductionStore || (accumulations.empty() && reducedAccumulations.empty())) {
        return false;
    }
    const std::unordered_set<std::string> liveAfterLoop = computeLiveAfterLoop(function, header, bodyIndex);
    for (const auto& [symbol, term] : accumulations) {
        (void)term;
        if (liveAfterLoop.find(symbol) == liveAfterLoop.end()) {
            return false;
        }
    }
    for (const auto& [symbol, accumulation] : reducedAccumulations) {
        (void)accumulation;
        if (liveAfterLoop.find(symbol) == liveAfterLoop.end()) {
            return false;
        }
    }

    std::vector<ir::Instruction> replacement;
    for (const auto& [symbol, term] : accumulations) {
        const auto total = closedFormModuloSum(term, *start, *step, *trips);
        if (!total.has_value()) {
            return false;
        }

        ir::Instruction load;
        load.kind = ir::InstructionKind::LoadLocal;
        load.symbol = symbol;
        load.dst = ir::Value{function.nextValue++};
        replacement.push_back(load);

        ir::Instruction add;
        add.kind = ir::InstructionKind::Binary;
        add.binaryOp = ir::BinaryOpcode::Add;
        add.dst = ir::Value{function.nextValue++};
        add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(*total)};
        replacement.push_back(add);

        ir::Instruction store;
        store.kind = ir::InstructionKind::StoreLocal;
        store.symbol = symbol;
        store.operands = {ir::Operand::ref(add.dst)};
        replacement.push_back(store);
    }
    for (const auto& [symbol, accumulation] : reducedAccumulations) {
        const auto total = closedFormSumNoWrap(accumulation.increment, *start, *step, *trips);
        if (!total.has_value()) {
            return false;
        }

        ir::Instruction load;
        load.kind = ir::InstructionKind::LoadLocal;
        load.symbol = symbol;
        load.dst = ir::Value{function.nextValue++};
        replacement.push_back(load);

        ir::Instruction add;
        add.kind = ir::InstructionKind::Binary;
        add.binaryOp = ir::BinaryOpcode::Add;
        add.dst = ir::Value{function.nextValue++};
        add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(*total)};
        replacement.push_back(add);

        ir::Instruction mod;
        mod.kind = ir::InstructionKind::Binary;
        mod.binaryOp = ir::BinaryOpcode::Mod;
        mod.dst = ir::Value{function.nextValue++};
        mod.operands = {ir::Operand::ref(add.dst), ir::Operand::imm(accumulation.modulus)};
        replacement.push_back(mod);

        ir::Instruction store;
        store.kind = ir::InstructionKind::StoreLocal;
        store.symbol = symbol;
        store.operands = {ir::Operand::ref(mod.dst)};
        replacement.push_back(store);
    }

    ir::Instruction inductionStore;
    inductionStore.kind = ir::InstructionKind::StoreLocal;
    inductionStore.symbol = induction;
    inductionStore.operands = {ir::Operand::imm(static_cast<std::int32_t>(*start + static_cast<std::int64_t>(*step) * *trips))};
    replacement.push_back(inductionStore);

    ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
    mutableHeader.instructions = std::move(replacement);
    mutableHeader.terminator = {};
    mutableHeader.terminator.kind = ir::TerminatorKind::Jump;
    mutableHeader.terminator.trueBlock = exitIndex;
    mutableHeader.hasTerminator = true;
    return true;
}

struct PeriodicCondition {
    ModuloTerm term;
    ir::BinaryOpcode op = ir::BinaryOpcode::Equal;
    std::int32_t rhs = 0;
};

std::optional<ir::BinaryOpcode> reverseComparison(ir::BinaryOpcode op)
{
    switch (op) {
    case ir::BinaryOpcode::Equal:
        return ir::BinaryOpcode::Equal;
    case ir::BinaryOpcode::NotEqual:
        return ir::BinaryOpcode::NotEqual;
    case ir::BinaryOpcode::Less:
        return ir::BinaryOpcode::Greater;
    case ir::BinaryOpcode::LessEqual:
        return ir::BinaryOpcode::GreaterEqual;
    case ir::BinaryOpcode::Greater:
        return ir::BinaryOpcode::Less;
    case ir::BinaryOpcode::GreaterEqual:
        return ir::BinaryOpcode::LessEqual;
    default:
        return std::nullopt;
    }
}

bool evalComparison(ir::BinaryOpcode op, std::int32_t lhs, std::int32_t rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Equal:
        return lhs == rhs;
    case ir::BinaryOpcode::NotEqual:
        return lhs != rhs;
    case ir::BinaryOpcode::Less:
        return lhs < rhs;
    case ir::BinaryOpcode::LessEqual:
        return lhs <= rhs;
    case ir::BinaryOpcode::Greater:
        return lhs > rhs;
    case ir::BinaryOpcode::GreaterEqual:
        return lhs >= rhs;
    default:
        return false;
    }
}

std::optional<PeriodicCondition> analyzePeriodicCondition(
    const ir::BasicBlock& block,
    const ir::Operand& cond,
    const std::string& induction)
{
    if (cond.isImmediate || cond.value.id < 0) {
        return std::nullopt;
    }

    std::unordered_map<int, std::int32_t> constants;
    std::unordered_map<int, PolyValue> polyValues;
    std::unordered_map<int, ModuloTerm> moduloValues;
    struct CompareDef { ir::BinaryOpcode op; ir::Operand lhs; ir::Operand rhs; };
    std::unordered_map<int, CompareDef> compares;

    for (const ir::Instruction& inst : block.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
                polyValues[inst.dst.id] = PolyValue{"", Poly{0, 0, inst.operands[0].immediate}};
            }
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id < 0) {
                return std::nullopt;
            }
            if (inst.symbol != induction) {
                return std::nullopt;
            }
            constants.erase(inst.dst.id);
            polyValues[inst.dst.id] = PolyValue{"", Poly{0, 1, 0}};
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id < 0 || inst.operands.empty()) {
                return std::nullopt;
            }
            constants.erase(inst.dst.id);
            polyValues.erase(inst.dst.id);
            moduloValues.erase(inst.dst.id);
            if (const auto value = polyOf(inst.operands[0], constants, polyValues); value.has_value()) {
                polyValues[inst.dst.id] = *value;
                if (value->base.empty() && isConstant(value->poly)) {
                    if (const auto constant = int32Value(value->poly.constant); constant.has_value()) {
                        constants[inst.dst.id] = *constant;
                    }
                }
            } else if (const auto value = moduloOf(inst.operands[0], moduloValues); value.has_value()) {
                moduloValues[inst.dst.id] = *value;
            } else {
                return std::nullopt;
            }
            break;
        case ir::InstructionKind::Binary:
            if (inst.dst.id < 0 || inst.operands.size() != 2) {
                return std::nullopt;
            }
            constants.erase(inst.dst.id);
            polyValues.erase(inst.dst.id);
            moduloValues.erase(inst.dst.id);
            if (inst.binaryOp == ir::BinaryOpcode::Mod) {
                const auto lhs = polyOf(inst.operands[0], constants, polyValues);
                const auto rhs = constOf(inst.operands[1], constants);
                if (!lhs.has_value() || !lhs->base.empty() || !rhs.has_value() || *rhs <= 0 || *rhs > 256) {
                    return std::nullopt;
                }
                moduloValues[inst.dst.id] = ModuloTerm{lhs->poly, *rhs};
                break;
            }
            if (inst.binaryOp == ir::BinaryOpcode::Equal
                || inst.binaryOp == ir::BinaryOpcode::NotEqual
                || inst.binaryOp == ir::BinaryOpcode::Less
                || inst.binaryOp == ir::BinaryOpcode::LessEqual
                || inst.binaryOp == ir::BinaryOpcode::Greater
                || inst.binaryOp == ir::BinaryOpcode::GreaterEqual) {
                compares[inst.dst.id] = CompareDef{inst.binaryOp, inst.operands[0], inst.operands[1]};
                break;
            }
            if (const auto lhs = polyOf(inst.operands[0], constants, polyValues), rhs = polyOf(inst.operands[1], constants, polyValues);
                lhs.has_value() && rhs.has_value()) {
                if (const auto folded = evalPoly(inst.binaryOp, *lhs, *rhs); folded.has_value()) {
                    polyValues[inst.dst.id] = *folded;
                    if (folded->base.empty() && isConstant(folded->poly)) {
                        if (const auto constant = int32Value(folded->poly.constant); constant.has_value()) {
                            constants[inst.dst.id] = *constant;
                        }
                    }
                    break;
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    const auto found = compares.find(cond.value.id);
    if (found == compares.end()) {
        return std::nullopt;
    }
    const auto lhsMod = moduloOf(found->second.lhs, moduloValues);
    const auto rhsConst = constOf(found->second.rhs, constants);
    if (lhsMod.has_value() && rhsConst.has_value()) {
        return PeriodicCondition{*lhsMod, found->second.op, *rhsConst};
    }
    const auto lhsConst = constOf(found->second.lhs, constants);
    const auto rhsMod = moduloOf(found->second.rhs, moduloValues);
    if (lhsConst.has_value() && rhsMod.has_value()) {
        const auto reversed = reverseComparison(found->second.op);
        if (!reversed.has_value()) {
            return std::nullopt;
        }
        return PeriodicCondition{*rhsMod, *reversed, *lhsConst};
    }
    return std::nullopt;
}

std::optional<std::int32_t> closedFormPeriodicConditionSum(
    const PeriodicCondition& condition,
    const Poly& thenIncrement,
    const Poly& elseIncrement,
    std::int32_t start,
    std::int32_t step,
    int trips)
{
    if (condition.term.modulus <= 0 || condition.term.modulus > 256 || step <= 0 || trips < 0) {
        return std::nullopt;
    }

    __int128 total = 0;
    const int period = condition.term.modulus;
    for (int residue = 0; residue < period; ++residue) {
        if (residue >= trips) {
            break;
        }
        const int count = ((trips - 1 - residue) / period) + 1;
        const std::int32_t first = static_cast<std::int32_t>(
            static_cast<std::int64_t>(start) + static_cast<std::int64_t>(step) * residue);
        const std::int32_t condValue = evalPolyInt32(condition.term.poly, first) % condition.term.modulus;
        const Poly& selected = evalComparison(condition.op, condValue, condition.rhs) ? thenIncrement : elseIncrement;
        const auto partial = closedFormSum(selected, first, step * period, count);
        if (!partial.has_value()) {
            return std::nullopt;
        }
        total += *partial;
    }
    return wrapInt32Wide(total);
}

bool tryPeriodicConditionalAccumulationLoop(ir::Function& function, int header)
{
    const int n = static_cast<int>(function.blocks.size());
    if (header < 0 || header >= n) {
        return false;
    }
    const ir::BasicBlock& H = function.blocks[static_cast<std::size_t>(header)];
    if (!isWhileCond(H) || H.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    const int ifIdx = H.terminator.trueBlock;
    const int exitIdx = H.terminator.falseBlock;
    if (ifIdx < 0 || ifIdx >= n || exitIdx < 0) {
        return false;
    }
    const ir::BasicBlock& IF = function.blocks[static_cast<std::size_t>(ifIdx)];
    if (IF.terminator.kind != ir::TerminatorKind::Branch) {
        return false;
    }
    for (const ir::Instruction& inst : IF.instructions) {
        if (inst.kind == ir::InstructionKind::StoreLocal || inst.kind == ir::InstructionKind::StoreGlobal
            || inst.kind == ir::InstructionKind::LoadGlobal || inst.kind == ir::InstructionKind::Call) {
            return false;
        }
    }

    const int thenIdx = IF.terminator.trueBlock;
    const int elseIdx = IF.terminator.falseBlock;
    if (thenIdx < 0 || elseIdx < 0 || thenIdx >= n || elseIdx >= n || thenIdx == elseIdx) {
        return false;
    }
    const ir::BasicBlock& THEN = function.blocks[static_cast<std::size_t>(thenIdx)];
    const ir::BasicBlock& ELSE = function.blocks[static_cast<std::size_t>(elseIdx)];
    if (THEN.terminator.kind != ir::TerminatorKind::Jump || ELSE.terminator.kind != ir::TerminatorKind::Jump) {
        return false;
    }
    const int latchIdx = THEN.terminator.trueBlock;
    if (ELSE.terminator.trueBlock != latchIdx || latchIdx < 0 || latchIdx >= n) {
        return false;
    }
    const ir::BasicBlock& LATCH = function.blocks[static_cast<std::size_t>(latchIdx)];
    if (LATCH.terminator.kind != ir::TerminatorKind::Jump || LATCH.terminator.trueBlock != header) {
        return false;
    }

    const auto hdr = analyzeCompare(H, H.terminator.condition);
    if (!hdr.has_value()) {
        return false;
    }
    const std::string induction = hdr->local;
    const auto step = inductionStepInBody(LATCH, induction);
    if (!step.has_value() || *step <= 0) {
        return false;
    }
    for (const ir::Instruction& inst : LATCH.instructions) {
        if (inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::LoadGlobal || inst.kind == ir::InstructionKind::Call) {
            return false;
        }
        if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol != induction) {
            return false;
        }
    }
    const auto start = initialLocalConst(function, header, induction);
    if (!start.has_value()) {
        return false;
    }
    const auto totalTrips = tripCount(hdr->op, *start, hdr->bound, *step);
    if (!totalTrips.has_value()) {
        return false;
    }
    const auto periodic = analyzePeriodicCondition(IF, IF.terminator.condition, induction);
    if (!periodic.has_value()) {
        return false;
    }

    std::string acc;
    for (const ir::BasicBlock* b : {&THEN, &ELSE}) {
        for (const ir::Instruction& inst : b->instructions) {
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                if (!acc.empty() && acc != inst.symbol) {
                    return false;
                }
                acc = inst.symbol;
            }
        }
    }
    if (acc.empty() || acc == induction) {
        return false;
    }
    const auto thenInc = branchAccIncrement(THEN, induction, acc);
    const auto elseInc = branchAccIncrement(ELSE, induction, acc);
    if (!thenInc.has_value() || !elseInc.has_value()) {
        return false;
    }
    const auto incTotal = closedFormPeriodicConditionSum(*periodic, *thenInc, *elseInc, *start, *step, *totalTrips);
    if (!incTotal.has_value()) {
        return false;
    }

    std::vector<ir::Instruction> replacement;
    ir::Instruction load;
    load.kind = ir::InstructionKind::LoadLocal;
    load.symbol = acc;
    load.dst = ir::Value{function.nextValue++};
    replacement.push_back(load);

    ir::Instruction add;
    add.kind = ir::InstructionKind::Binary;
    add.binaryOp = ir::BinaryOpcode::Add;
    add.dst = ir::Value{function.nextValue++};
    add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(*incTotal)};
    replacement.push_back(add);

    ir::Instruction store;
    store.kind = ir::InstructionKind::StoreLocal;
    store.symbol = acc;
    store.operands = {ir::Operand::ref(add.dst)};
    replacement.push_back(store);

    ir::Instruction indStore;
    indStore.kind = ir::InstructionKind::StoreLocal;
    indStore.symbol = induction;
    indStore.operands = {ir::Operand::imm(static_cast<std::int32_t>(*start + static_cast<std::int64_t>(*step) * *totalTrips))};
    replacement.push_back(indStore);

    ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
    mutableHeader.instructions = std::move(replacement);
    mutableHeader.terminator = {};
    mutableHeader.terminator.kind = ir::TerminatorKind::Jump;
    mutableHeader.terminator.trueBlock = exitIdx;
    mutableHeader.hasTerminator = true;
    return true;
}

class LoopSumPass final : public Pass {
public:
    std::string name() const override { return "loop-sum"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
                if (runOnLoop(function, i)
                    || tryModuloAccumulationLoop(function, i)
                    || tryPeriodicConditionalAccumulationLoop(function, i)
                    || tryConditionalAccumulationLoop(function, i)) {
                    changed = true;
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createLoopSumPass()
{
    return std::make_unique<LoopSumPass>();
}

} // namespace toyc::passes
