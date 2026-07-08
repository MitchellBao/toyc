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

ir::Value newValue(ir::Function& function)
{
    return ir::Value{function.nextValue++};
}

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

bool isSafeBodyInstruction(const ir::Instruction& inst, const std::unordered_set<std::string>& deadStoreGlobals)
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
        return !inst.symbol.empty() && deadStoreGlobals.find(inst.symbol) != deadStoreGlobals.end();
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
    const auto safeTotal = checkedInt64(total);
    if (!safeTotal.has_value()) {
        return std::nullopt;
    }
    return int32Value(*safeTotal);
}

ir::Operand appendLoadLocal(std::vector<ir::Instruction>& instructions, ir::Function& function, const std::string& symbol)
{
    ir::Instruction load;
    load.kind = ir::InstructionKind::LoadLocal;
    load.symbol = symbol;
    load.dst = newValue(function);
    instructions.push_back(load);
    return ir::Operand::ref(instructions.back().dst);
}

ir::Operand appendBinary(
    std::vector<ir::Instruction>& instructions,
    ir::Function& function,
    ir::BinaryOpcode op,
    ir::Operand lhs,
    ir::Operand rhs)
{
    ir::Instruction inst;
    inst.kind = ir::InstructionKind::Binary;
    inst.binaryOp = op;
    inst.dst = newValue(function);
    inst.operands = {lhs, rhs};
    instructions.push_back(inst);
    return ir::Operand::ref(instructions.back().dst);
}

ir::Operand appendMulByConst(std::vector<ir::Instruction>& instructions, ir::Function& function, ir::Operand value, std::int64_t factor)
{
    if (factor == 1) {
        return value;
    }
    if (factor == 0) {
        return ir::Operand::imm(0);
    }
    return appendBinary(instructions, function, ir::BinaryOpcode::Mul, value, ir::Operand::imm(static_cast<std::int32_t>(factor)));
}

ir::Operand appendAddOperand(std::vector<ir::Instruction>& instructions, ir::Function& function, std::optional<ir::Operand>& total, ir::Operand term)
{
    if (!total.has_value()) {
        total = term;
        return term;
    }
    total = appendBinary(instructions, function, ir::BinaryOpcode::Add, *total, term);
    return *total;
}

std::optional<ir::Operand> appendDynamicTripCount(
    std::vector<ir::Instruction>& instructions,
    ir::Function& function,
    ir::BinaryOpcode op,
    std::int32_t start,
    std::int32_t step,
    ir::Operand bound)
{
    std::optional<ir::Operand> numerator;
    std::int32_t denominator = 0;
    switch (op) {
    case ir::BinaryOpcode::Less:
    case ir::BinaryOpcode::LessEqual:
        if (step <= 0) {
            return std::nullopt;
        }
        numerator = start == 0 ? bound : appendBinary(instructions, function, ir::BinaryOpcode::Sub, bound, ir::Operand::imm(start));
        if (op == ir::BinaryOpcode::LessEqual) {
            numerator = appendBinary(instructions, function, ir::BinaryOpcode::Add, *numerator, ir::Operand::imm(1));
        }
        denominator = step;
        break;
    case ir::BinaryOpcode::Greater:
    case ir::BinaryOpcode::GreaterEqual:
        if (step >= 0) {
            return std::nullopt;
        }
        numerator = appendBinary(instructions, function, ir::BinaryOpcode::Sub, ir::Operand::imm(start), bound);
        if (op == ir::BinaryOpcode::GreaterEqual) {
            numerator = appendBinary(instructions, function, ir::BinaryOpcode::Add, *numerator, ir::Operand::imm(1));
        }
        denominator = -step;
        break;
    default:
        return std::nullopt;
    }
    if (denominator <= 0) {
        return std::nullopt;
    }
    const ir::Operand biased = appendBinary(
        instructions,
        function,
        ir::BinaryOpcode::Add,
        *numerator,
        ir::Operand::imm(denominator - 1));
    return appendBinary(instructions, function, ir::BinaryOpcode::Div, biased, ir::Operand::imm(denominator));
}

std::optional<ir::Operand> appendDynamicClosedFormSum(
    std::vector<ir::Instruction>& instructions,
    ir::Function& function,
    const Poly& increment,
    std::int32_t start,
    std::int32_t step,
    ir::Operand trips)
{
    std::optional<ir::Operand> total;
    if (increment.constant != 0) {
        appendAddOperand(instructions, function, total, appendMulByConst(instructions, function, trips, increment.constant));
    }

    std::optional<ir::Operand> sumI;
    if (increment.linear != 0 || increment.quadratic != 0) {
        const ir::Operand nMinusOne = appendBinary(instructions, function, ir::BinaryOpcode::Sub, trips, ir::Operand::imm(1));
        const ir::Operand product = appendBinary(instructions, function, ir::BinaryOpcode::Mul, trips, nMinusOne);
        const ir::Operand triangular = appendBinary(instructions, function, ir::BinaryOpcode::Div, product, ir::Operand::imm(2));
        std::optional<ir::Operand> sum;
        if (start != 0) {
            appendAddOperand(instructions, function, sum, appendMulByConst(instructions, function, trips, start));
        }
        if (step != 0) {
            appendAddOperand(instructions, function, sum, appendMulByConst(instructions, function, triangular, step));
        }
        sumI = sum.value_or(ir::Operand::imm(0));
    }
    if (increment.linear != 0) {
        appendAddOperand(instructions, function, total, appendMulByConst(instructions, function, *sumI, increment.linear));
    }
    if (!total.has_value()) {
        return std::nullopt;
    }
    return total;
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

bool tryInvariantAccumulationLoop(
    ir::Function& function,
    int header,
    int exitIndex,
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

    std::unordered_set<std::string> liveAfterLoop;
    for (int b = exitIndex; b < static_cast<int>(function.blocks.size()); ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                liveAfterLoop.insert(inst.symbol);
            }
        }
    }
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
    std::unordered_set<std::string> loadedGlobalsAfterLoop;
    for (int b = exitIndex; b < static_cast<int>(function.blocks.size()); ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::LoadGlobal && !inst.symbol.empty()) {
                loadedGlobalsAfterLoop.insert(inst.symbol);
            }
            if (inst.kind == ir::InstructionKind::Call) {
                return false;
            }
        }
    }
    std::unordered_set<std::string> deadStoreGlobals;
    for (const ir::Instruction& inst : body.instructions) {
        if (inst.kind == ir::InstructionKind::StoreGlobal
            && !inst.symbol.empty()
            && loadedGlobalsAfterLoop.find(inst.symbol) == loadedGlobalsAfterLoop.end()) {
            deadStoreGlobals.insert(inst.symbol);
        }
    }
    for (const ir::Instruction& inst : body.instructions) {
        if (!isSafeBodyInstruction(inst, deadStoreGlobals)) {
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
    std::optional<ir::BinaryOpcode> cmpOp;
    std::optional<std::string> induction;
    std::optional<std::int32_t> bound;
    std::optional<ir::Operand> dynamicBound;

    for (const ir::Instruction& inst : headerBlock.instructions) {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                constants[inst.dst.id] = inst.operands[0].immediate;
                linear[inst.dst.id] = LinearValue{"", inst.operands[0].immediate};
            }
            break;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id >= 0) {
                if (bodyStoredLocals.find(inst.symbol) == bodyStoredLocals.end()) {
                    if (const auto value = localConstants.find(inst.symbol); value != localConstants.end()) {
                        constants[inst.dst.id] = value->second;
                        linear[inst.dst.id] = LinearValue{"", value->second};
                        break;
                    }
                }
                loadedLocal[inst.dst.id] = inst.symbol;
                linear[inst.dst.id] = LinearValue{inst.symbol, 0};
            }
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id >= 0 && !inst.operands.empty()) {
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
                        dynamicBound = inst.operands[1];
                        cmpOp = inst.binaryOp;
                    } else if (!lhs->base.empty()
                        && !rhs->base.empty()
                        && rhs->base != lhs->base
                        && bodyStoredLocals.find(rhs->base) == bodyStoredLocals.end()) {
                        induction = lhs->base;
                        dynamicBound = inst.operands[1];
                        cmpOp = inst.binaryOp;
                    } else if (lhs->base.empty() && !rhs->base.empty()) {
                        induction = rhs->base;
                        bound = lhs->offset;
                        dynamicBound = inst.operands[0];
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

    if (!cmpOp.has_value() || !induction.has_value() || (!bound.has_value() && !dynamicBound.has_value())) {
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
        if (inst.kind == ir::InstructionKind::StoreGlobal
            && !inst.symbol.empty()
            && deadStoreGlobals.find(inst.symbol) != deadStoreGlobals.end()) {
            continue;
        }
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
    std::optional<int> trips;
    if (bound.has_value()) {
        trips = tripCount(*cmpOp, *start, *bound, step);
    }
    if (!trips.has_value()) {
        if (!dynamicBound.has_value()) {
            return false;
        }

        std::unordered_set<std::string> liveAfterLoop;
        for (int b = exitIndex; b < static_cast<int>(function.blocks.size()); ++b) {
            const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
            for (const ir::Instruction& inst : block.instructions) {
                if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                    liveAfterLoop.insert(inst.symbol);
                }
            }
        }

        std::vector<ir::Instruction> replacement;
        const auto dynamicTrips = appendDynamicTripCount(replacement, function, *cmpOp, *start, step, *dynamicBound);
        if (!dynamicTrips.has_value()) {
            return false;
        }
        bool changedAccumulator = false;
        for (const auto& [symbol, value] : finalLocal) {
            if (symbol == *induction || liveAfterLoop.find(symbol) == liveAfterLoop.end()) {
                continue;
            }
            if (value.base != symbol || isZero(value.poly)) {
                return false;
            }
            if (value.poly.quadratic != 0) {
                return false;
            }
            const ir::Operand current = appendLoadLocal(replacement, function, symbol);
            const auto total = appendDynamicClosedFormSum(replacement, function, value.poly, *start, step, *dynamicTrips);
            if (!total.has_value()) {
                return false;
            }
            const ir::Operand updated = appendBinary(replacement, function, ir::BinaryOpcode::Add, current, *total);

            ir::Instruction store;
            store.kind = ir::InstructionKind::StoreLocal;
            store.symbol = symbol;
            store.operands = {updated};
            replacement.push_back(store);
            changedAccumulator = true;
        }
        if (!changedAccumulator) {
            return false;
        }

        ir::Instruction inductionStore;
        inductionStore.kind = ir::InstructionKind::StoreLocal;
        inductionStore.symbol = *induction;
        const ir::Operand stepTrips = appendMulByConst(replacement, function, *dynamicTrips, step);
        inductionStore.operands = {appendBinary(replacement, function, ir::BinaryOpcode::Add, ir::Operand::imm(*start), stepTrips)};
        replacement.push_back(inductionStore);

        const int closedIndex = static_cast<int>(function.blocks.size());
        ir::BasicBlock closedBlock;
        closedBlock.label = ".while.closed";
        closedBlock.instructions = std::move(replacement);
        closedBlock.terminator.kind = ir::TerminatorKind::Jump;
        closedBlock.terminator.trueBlock = exitIndex;
        closedBlock.hasTerminator = true;
        function.blocks.push_back(std::move(closedBlock));

        ir::BasicBlock& mutableHeader = function.blocks[static_cast<std::size_t>(header)];
        mutableHeader.terminator.trueBlock = closedIndex;
        return true;
    }
    if (tryInvariantAccumulationLoop(function, header, exitIndex, body, *induction, *start, step, *trips)) {
        return true;
    }

    std::vector<ir::Instruction> replacement;
    bool changedAccumulator = false;
    std::unordered_set<std::string> liveAfterLoop;
    for (int b = exitIndex; b < static_cast<int>(function.blocks.size()); ++b) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(b)];
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                liveAfterLoop.insert(inst.symbol);
            }
        }
    }

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

class LoopSumPass final : public Pass {
public:
    std::string name() const override { return "loop-sum"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
                if (runOnLoop(function, i)) {
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
