#include "pass_manager.h"

#include <algorithm>
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
    case ir::InstructionKind::LoadGlobal:
    case ir::InstructionKind::StoreGlobal:
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
    default:
        return std::nullopt;
    }
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
    std::optional<ir::BinaryOpcode> cmpOp;
    std::optional<std::string> induction;
    std::optional<std::int32_t> bound;

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

    constants.clear();
    linear.clear();
    std::unordered_map<std::string, LinearValue> finalLocal;
    for (const ir::Instruction& inst : body.instructions) {
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
                linear[inst.dst.id] = LinearValue{inst.symbol, 0};
            }
            break;
        case ir::InstructionKind::Copy:
            if (inst.dst.id >= 0 && !inst.operands.empty()) {
                if (const auto value = linearOf(inst.operands[0], constants, linear); value.has_value()) {
                    linear[inst.dst.id] = *value;
                    if (value->base.empty()) {
                        constants[inst.dst.id] = value->offset;
                    }
                } else {
                    return false;
                }
            }
            break;
        case ir::InstructionKind::Binary:
            if (inst.dst.id >= 0 && inst.operands.size() == 2) {
                const auto lhs = linearOf(inst.operands[0], constants, linear);
                const auto rhs = linearOf(inst.operands[1], constants, linear);
                if (!lhs.has_value() || !rhs.has_value()) {
                    return false;
                }
                const auto folded = evalLinear(inst.binaryOp, *lhs, *rhs);
                if (!folded.has_value()) {
                    return false;
                }
                linear[inst.dst.id] = *folded;
                if (folded->base.empty()) {
                    constants[inst.dst.id] = folded->offset;
                }
            }
            break;
        case ir::InstructionKind::StoreLocal:
            if (inst.operands.empty()) {
                return false;
            }
            if (const auto value = linearOf(inst.operands[0], constants, linear); value.has_value()) {
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
    if (inductionFinal == finalLocal.end() || inductionFinal->second.base != *induction) {
        return false;
    }
    const std::int32_t step = inductionFinal->second.offset;
    const auto trips = tripCount(*cmpOp, *start, *bound, step);
    if (!trips.has_value()) {
        return false;
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
        if (value.base == symbol && value.offset != 0) {
            const std::int64_t total = static_cast<std::int64_t>(value.offset) * *trips;
            if (total < INT32_MIN || total > INT32_MAX) {
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
            add.operands = {ir::Operand::ref(load.dst), ir::Operand::imm(static_cast<std::int32_t>(total))};
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
