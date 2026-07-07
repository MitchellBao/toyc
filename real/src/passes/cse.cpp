#include "pass_manager.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>

namespace toyc::passes {
namespace {

std::string operandKey(const ir::Operand& operand)
{
    if (operand.isImmediate) {
        return "#" + std::to_string(operand.immediate);
    }
    return "%" + std::to_string(operand.value.id);
}

bool isCommutative(ir::BinaryOpcode op)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
    case ir::BinaryOpcode::Mul:
    case ir::BinaryOpcode::Equal:
    case ir::BinaryOpcode::NotEqual:
    case ir::BinaryOpcode::LogicalAnd:
    case ir::BinaryOpcode::LogicalOr:
        return true;
    case ir::BinaryOpcode::Sub:
    case ir::BinaryOpcode::Div:
    case ir::BinaryOpcode::Mod:
    case ir::BinaryOpcode::Less:
    case ir::BinaryOpcode::LessEqual:
    case ir::BinaryOpcode::Greater:
    case ir::BinaryOpcode::GreaterEqual:
        return false;
    }
    return false;
}

std::string memoryVersionedSymbol(const std::string& symbol, const std::unordered_map<std::string, int>& versions)
{
    const auto found = versions.find(symbol);
    const int version = found == versions.end() ? 0 : found->second;
    return symbol + "@" + std::to_string(version);
}

std::string instructionKey(const ir::Instruction& inst, const std::unordered_map<std::string, int>& localVersions, int globalVersion)
{
    if (inst.hasSideEffect || inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::StoreLocal) {
        return {};
    }

    std::ostringstream key;
    key << static_cast<int>(inst.kind);

    if (inst.kind == ir::InstructionKind::Binary) {
        key << ":bin:" << static_cast<int>(inst.binaryOp);
        if (inst.operands.size() != 2) {
            return {};
        }
        std::string lhs = operandKey(inst.operands[0]);
        std::string rhs = operandKey(inst.operands[1]);
        if (isCommutative(inst.binaryOp) && rhs < lhs) {
            std::swap(lhs, rhs);
        }
        key << ':' << lhs << ':' << rhs;
        return key.str();
    }

    if (inst.kind == ir::InstructionKind::Unary) {
        key << ":un:" << static_cast<int>(inst.unaryOp);
    } else if (inst.kind == ir::InstructionKind::LoadLocal) {
        key << ":local:" << memoryVersionedSymbol(inst.symbol, localVersions);
    } else if (inst.kind == ir::InstructionKind::LoadGlobal) {
        key << ":global:" << inst.symbol << '@' << globalVersion;
    } else {
        key << ':' << static_cast<int>(inst.binaryOp) << ':' << static_cast<int>(inst.unaryOp) << ':' << inst.symbol;
    }

    for (const ir::Operand& operand : inst.operands) {
        key << ':' << operandKey(operand);
    }
    return key.str();
}

void killDefinitionsForValue(std::unordered_map<std::string, ir::Value>& seen, ir::Value value)
{
    for (auto iter = seen.begin(); iter != seen.end();) {
        if (iter->second == value) {
            iter = seen.erase(iter);
        } else {
            ++iter;
        }
    }
}

void advanceLocalVersion(const ir::Instruction& inst, std::unordered_map<std::string, int>& localVersions)
{
    if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
        ++localVersions[inst.symbol];
    }
}

class CsePass final : public Pass {
public:
    std::string name() const override { return "local-cse"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<std::string, ir::Value> seen;
                std::unordered_map<std::string, int> localVersions;
                int globalVersion = 0;
                for (ir::Instruction& inst : block.instructions) {
                    if (inst.kind == ir::InstructionKind::Call || inst.hasSideEffect) {
                        seen.clear();
                        ++globalVersion;
                    } else if (inst.kind == ir::InstructionKind::StoreGlobal) {
                        seen.clear();
                        ++globalVersion;
                    }
                    advanceLocalVersion(inst, localVersions);

                    const std::string key = instructionKey(inst, localVersions, globalVersion);
                    if (key.empty() || inst.dst.id < 0) {
                        continue;
                    }
                    const auto found = seen.find(key);
                    if (found != seen.end()) {
                        inst.kind = ir::InstructionKind::Copy;
                        inst.operands = {ir::Operand::ref(found->second)};
                        inst.symbol.clear();
                        killDefinitionsForValue(seen, inst.dst);
                        changed = true;
                    } else {
                        seen.emplace(key, inst.dst);
                    }
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createCsePass()
{
    return std::make_unique<CsePass>();
}

} // namespace toyc::passes
