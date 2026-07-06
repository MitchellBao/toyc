#include "codegen.h"
#include "ast_optimizer.h"
#include "ir_builder.h"
#include "pass.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

class CodegenError final : public std::runtime_error {
public:
    explicit CodegenError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

struct Symbol {
    bool isGlobal = false;
    bool isConst = false;
    bool hasConstValue = false;
    std::int32_t constValue = 0;
    bool hasCopy = false;
    std::string copyOf;
    std::string label;
    int offset = 0;
    int savedReg = 0;
    bool isDead = false;
    int argReg = -1;
};

struct FunctionLayout {
    int localSlots = 0;
    int paramSlots = 0;
    int frameSize = 0;
    int savedRegs = 0;
    bool savesRa = true;
};

int alignTo(int value, int alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

bool fitsSigned12(std::int32_t value)
{
    return value >= -2048 && value <= 2047;
}

const char* savedRegName(int index)
{
    static constexpr const char* names[] = {
        "",
        "s1",
        "s2",
        "s3",
        "s4",
        "s5",
        "s6",
        "s7",
        "s8",
        "s9",
        "s10",
        "s11",
        "t2",
        "t3",
        "t4",
        "t5",
        "t6",
    };
    return names[index];
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

class AstRiscVBackend {
public:
    AstRiscVBackend(const Program& program, std::ostream& out, CodegenOptions options)
        : program_(program), out_(out), options_(options)
    {
    }

    void generate()
    {
        collectGlobals();
        emitData();
        out_ << "\n.text\n";
        for (const auto& item : program_.items) {
            if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                emitFunction(*funcItem->func);
            }
        }
    }

private:
    void collectGlobals()
    {
        for (const auto& item : program_.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                const auto& decl = *declItem->decl;
                const std::string label = "g_" + sanitizeLabel(decl.name);
                auto [it, inserted] = globalSymbols_.emplace(decl.name, Symbol{true, decl.isConst, false, 0, false, {}, label, 0, 0});
                (void)inserted;
            }
        }

        if (options_.optimize) {
            collectAssignedGlobals();
        }

        for (const auto& item : program_.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                const auto& decl = *declItem->decl;
                auto found = globalSymbols_.find(decl.name);
                if (decl.isConst || (options_.optimize && globalAssigned_.find(decl.name) == globalAssigned_.end())) {
                    found->second.constValue = requireConst(*decl.init);
                    found->second.hasConstValue = true;
                }
            }
        }
    }

    void collectAssignedGlobals()
    {
        for (const auto& item : program_.items) {
            if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                std::vector<std::vector<std::string>> locals;
                locals.emplace_back();
                for (const Param& param : funcItem->func->params) {
                    locals.back().push_back(param.name);
                }
                collectAssignedGlobalsInStmt(*funcItem->func->body, locals, true);
            }
        }
    }

    void collectAssignedGlobalsInStmt(const Stmt& stmt, std::vector<std::vector<std::string>>& locals, bool createsScope)
    {
        if (createsScope) {
            locals.emplace_back();
        }

        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            if (!isLocalName(assign->name, locals) && globalSymbols_.find(assign->name) != globalSymbols_.end()) {
                globalAssigned_[assign->name] = true;
            }
        } else if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            locals.back().push_back(declStmt->decl->name);
        } else if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            collectAssignedGlobalsInBlock(*block, locals, true);
        } else if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            auto thenLocals = locals;
            auto elseLocals = locals;
            collectAssignedGlobalsInStmt(*ifStmt->thenBranch, thenLocals, false);
            if (ifStmt->elseBranch != nullptr) {
                collectAssignedGlobalsInStmt(*ifStmt->elseBranch, elseLocals, false);
            }
        } else if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            auto bodyLocals = locals;
            collectAssignedGlobalsInStmt(*whileStmt->body, bodyLocals, false);
        }

        if (createsScope) {
            locals.pop_back();
        }
    }

    void collectAssignedGlobalsInBlock(const BlockStmt& block, std::vector<std::vector<std::string>>& locals, bool createsScope)
    {
        if (createsScope) {
            locals.emplace_back();
        }
        for (const auto& stmt : block.statements) {
            collectAssignedGlobalsInStmt(*stmt, locals, false);
        }
        if (createsScope) {
            locals.pop_back();
        }
    }

    bool isLocalName(const std::string& name, const std::vector<std::vector<std::string>>& locals) const
    {
        for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
            if (std::find(it->begin(), it->end(), name) != it->end()) {
                return true;
            }
        }
        return false;
    }

    void emitData()
    {
        out_ << ".data\n";
        for (const auto& item : program_.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                const auto& decl = *declItem->decl;
                const auto found = globalSymbols_.find(decl.name);
                const std::int32_t init = requireConst(*decl.init);
                out_ << ".globl " << found->second.label << "\n";
                out_ << found->second.label << ":\n";
                out_ << "  .word " << init << "\n";
            }
        }
    }

    void emitFunction(const FuncDef& func)
    {
        currentFunction_ = &func;
        prepareRegisterPlan(func);
        currentLayout_ = buildLayout(func);
        localScopes_.clear();
        nextLocalOffset_ = -12 - currentLayout_.savedRegs * 4;
        nextLocalSlot_ = 0;
        evalStackBytes_ = 0;
        returnLabel_ = newLabel(".L_return_");
        functionBodyLabel_ = newLabel(".L_body_");
        breakLabels_.clear();
        continueLabels_.clear();

        out_ << "\n.globl " << func.name << "\n";
        out_ << func.name << ":\n";
        out_ << "  addi sp, sp, -" << currentLayout_.frameSize << "\n";
        if (currentLayout_.savesRa) {
            out_ << "  sw ra, " << currentLayout_.frameSize - 4 << "(sp)\n";
        }
        out_ << "  sw s0, " << currentLayout_.frameSize - 8 << "(sp)\n";
        for (int i = 1; i <= currentLayout_.savedRegs; ++i) {
            out_ << "  sw " << savedRegName(i) << ", " << currentLayout_.frameSize - 8 - i * 4 << "(sp)\n";
        }
        out_ << "  addi s0, sp, " << currentLayout_.frameSize << "\n";

        pushScope();
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const int argReg = directParamArgReg(i);
            const Symbol& param = allocateLocal(func.params[i].name, false, std::nullopt, {}, argReg);
            if (options_.optimize && param.isDead) {
                continue;
            }
            if (param.argReg >= 0) {
                continue;
            }
            if (i < 8) {
                storeRegisterOrStack(param, std::string("a") + std::to_string(i));
            } else {
                const int incomingOffset = static_cast<int>((i - 8) * 4);
                out_ << "  lw t0, " << incomingOffset << "(s0)\n";
                storeRegisterOrStack(param, "t0");
            }
        }

        out_ << functionBodyLabel_ << ":\n";
        const bool bodyTerminal = emitBlock(*func.body, false);
        if (func.returnType == Type::Void && !bodyTerminal && !options_.optimize) {
            out_ << "  li a0, 0\n";
        }
        out_ << returnLabel_ << ":\n";
        for (int i = 1; i <= currentLayout_.savedRegs; ++i) {
            out_ << "  lw " << savedRegName(i) << ", " << currentLayout_.frameSize - 8 - i * 4 << "(sp)\n";
        }
        if (currentLayout_.savesRa) {
            out_ << "  lw ra, " << currentLayout_.frameSize - 4 << "(sp)\n";
        }
        out_ << "  lw s0, " << currentLayout_.frameSize - 8 << "(sp)\n";
        out_ << "  addi sp, sp, " << currentLayout_.frameSize << "\n";
        out_ << "  ret\n";
        popScope();
        currentFunction_ = nullptr;
    }

    FunctionLayout buildLayout(const FuncDef& func)
    {
        const int paramSlots = static_cast<int>(func.params.size());
        const int localSlots = countDecls(*func.body);
        const int savedRegs = options_.optimize ? plannedCalleeSavedRegs_ : 0;
        const int frameBytes = alignTo(16 + savedRegs * 4 + (paramSlots + localSlots) * 4, 16);
        const bool savesRa = !options_.optimize || containsCall(*func.body);
        return FunctionLayout{localSlots, paramSlots, std::max(frameBytes, 16), savedRegs, savesRa};
    }

    int countDecls(const Stmt& stmt) const
    {
        if (dynamic_cast<const DeclStmt*>(&stmt) != nullptr) {
            return 1;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            int total = 0;
            for (const auto& child : block->statements) {
                total += countDecls(*child);
            }
            return total;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            return countDecls(*ifStmt->thenBranch) + (ifStmt->elseBranch ? countDecls(*ifStmt->elseBranch) : 0);
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            return countDecls(*whileStmt->body);
        }
        return 0;
    }

    void prepareRegisterPlan(const FuncDef& func)
    {
        const int slotCount = static_cast<int>(func.params.size()) + countDecls(*func.body);
        registerForSlot_.assign(slotCount, 0);
        slotReadCount_.assign(slotCount, 0);
        plannedSavedRegs_ = 0;
        plannedCalleeSavedRegs_ = 0;
        if (!options_.optimize || slotCount == 0) {
            return;
        }

        std::vector<int> weights(slotCount, 0);
        std::vector<bool> canUseRegister(slotCount, false);
        std::vector<std::unordered_map<std::string, int>> scopes;
        scopes.emplace_back();

        int nextSlot = 0;
        const bool leafCanUseArgRegs = !containsCall(*func.body);
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const Param& param = func.params[i];
            scopes.back().emplace(param.name, nextSlot);
            canUseRegister[nextSlot] = !(leafCanUseArgRegs && i > 0 && i < 8);
            ++nextSlot;
        }
        scoreBlock(*func.body, scopes, false, 0, nextSlot, weights, canUseRegister);

        struct Candidate {
            int slot = 0;
            int weight = 0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(slotCount);
        for (int slot = 0; slot < slotCount; ++slot) {
            if (canUseRegister[slot] && weights[slot] > 0) {
                candidates.push_back(Candidate{slot, weights[slot]});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
            if (lhs.weight != rhs.weight) {
                return lhs.weight > rhs.weight;
            }
            return lhs.slot < rhs.slot;
        });

        const int availableRegs = leafCanUseArgRegs ? 16 : 11;
        plannedSavedRegs_ = std::min(availableRegs, static_cast<int>(candidates.size()));
        plannedCalleeSavedRegs_ = std::min(11, plannedSavedRegs_);
        for (int i = 0; i < plannedSavedRegs_; ++i) {
            registerForSlot_[candidates[i].slot] = i + 1;
        }
    }

    void scoreBlock(const BlockStmt& block,
        std::vector<std::unordered_map<std::string, int>>& scopes,
        bool createsScope,
        int loopDepth,
        int& nextSlot,
        std::vector<int>& weights,
        std::vector<bool>& canUseRegister)
    {
        if (createsScope) {
            scopes.emplace_back();
        }
        for (const auto& stmt : block.statements) {
            scoreStmt(*stmt, scopes, loopDepth, nextSlot, weights, canUseRegister);
        }
        if (createsScope) {
            scopes.pop_back();
        }
    }

    void scoreStmt(const Stmt& stmt,
        std::vector<std::unordered_map<std::string, int>>& scopes,
        int loopDepth,
        int& nextSlot,
        std::vector<int>& weights,
        std::vector<bool>& canUseRegister)
    {
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            scoreExpr(*exprStmt->expr, scopes, loopDepth, weights, canUseRegister);
            return;
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            scoreName(assign->name, scopes, loopDepth, 4, false, weights, canUseRegister);
            scoreExpr(*assign->value, scopes, loopDepth, weights, canUseRegister);
            return;
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            scoreExpr(*declStmt->decl->init, scopes, loopDepth, weights, canUseRegister);
            const int slot = nextSlot++;
            scopes.back().emplace(declStmt->decl->name, slot);
            canUseRegister[slot] = !declStmt->decl->isConst;
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            scoreBlock(*block, scopes, true, loopDepth, nextSlot, weights, canUseRegister);
            return;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            scoreExpr(*ifStmt->cond, scopes, loopDepth, weights, canUseRegister);
            auto thenScopes = scopes;
            scoreStmt(*ifStmt->thenBranch, thenScopes, loopDepth, nextSlot, weights, canUseRegister);
            if (ifStmt->elseBranch != nullptr) {
                auto elseScopes = scopes;
                scoreStmt(*ifStmt->elseBranch, elseScopes, loopDepth, nextSlot, weights, canUseRegister);
            }
            return;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            scoreExpr(*whileStmt->cond, scopes, loopDepth + 1, weights, canUseRegister);
            auto bodyScopes = scopes;
            scoreStmt(*whileStmt->body, bodyScopes, loopDepth + 1, nextSlot, weights, canUseRegister);
            return;
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (ret->value != nullptr) {
                scoreExpr(*ret->value, scopes, loopDepth, weights, canUseRegister);
            }
        }
    }

    void scoreExpr(const Expr& expr,
        const std::vector<std::unordered_map<std::string, int>>& scopes,
        int loopDepth,
        std::vector<int>& weights,
        const std::vector<bool>& canUseRegister)
    {
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            scoreName(name->name, scopes, loopDepth, 1, true, weights, canUseRegister);
            return;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            for (const auto& arg : call->args) {
                scoreExpr(*arg, scopes, loopDepth, weights, canUseRegister);
            }
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            scoreExpr(*unary->operand, scopes, loopDepth, weights, canUseRegister);
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            scoreExpr(*binary->lhs, scopes, loopDepth, weights, canUseRegister);
            scoreExpr(*binary->rhs, scopes, loopDepth, weights, canUseRegister);
        }
    }

    void scoreName(const std::string& name,
        const std::vector<std::unordered_map<std::string, int>>& scopes,
        int loopDepth,
        int multiplier,
        bool isRead,
        std::vector<int>& weights,
        const std::vector<bool>& canUseRegister)
    {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                if (found->second >= 0 && found->second < static_cast<int>(weights.size()) && canUseRegister[found->second]) {
                    weights[found->second] += multiplier * loopWeight(loopDepth);
                }
                if (isRead && found->second >= 0 && found->second < static_cast<int>(slotReadCount_.size())) {
                    ++slotReadCount_[found->second];
                }
                return;
            }
        }
    }

    int loopWeight(int loopDepth) const
    {
        int weight = 1;
        for (int i = 0; i < loopDepth && i < 6; ++i) {
            weight *= 8;
        }
        return weight;
    }

    bool containsCall(const Stmt& stmt) const
    {
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            return containsCall(*exprStmt->expr);
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            return containsCall(*assign->value);
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            return containsCall(*declStmt->decl->init);
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            for (const auto& child : block->statements) {
                if (containsCall(*child)) {
                    return true;
                }
            }
            return false;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            return containsCall(*ifStmt->cond) || containsCall(*ifStmt->thenBranch)
                || (ifStmt->elseBranch != nullptr && containsCall(*ifStmt->elseBranch));
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            return containsCall(*whileStmt->cond) || containsCall(*whileStmt->body);
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            return ret->value != nullptr && containsCall(*ret->value);
        }
        return false;
    }

    bool containsCall(const Expr& expr) const
    {
        if (dynamic_cast<const CallExpr*>(&expr) != nullptr) {
            return true;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            return containsCall(*unary->operand);
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            return containsCall(*binary->lhs) || containsCall(*binary->rhs);
        }
        return false;
    }

    void pushScope()
    {
        localScopes_.emplace_back();
    }

    void popScope()
    {
        localScopes_.pop_back();
    }

    const Symbol& allocateLocal(const std::string& name, bool isConst, std::optional<std::int32_t> constValue = std::nullopt, std::string copyOf = {})
    {
        return allocateLocal(name, isConst, constValue, std::move(copyOf), -1);
    }

    const Symbol& allocateLocal(const std::string& name, bool isConst, std::optional<std::int32_t> constValue, std::string copyOf, int argReg)
    {
        const int offset = nextLocalOffset_;
        nextLocalOffset_ -= 4;
        int savedReg = 0;
        const int slot = nextLocalSlot_++;
        bool isDead = false;
        if (options_.optimize && !(isConst && constValue.has_value()) && slot >= 0 && slot < static_cast<int>(registerForSlot_.size())) {
            savedReg = registerForSlot_[slot];
            isDead = slot < static_cast<int>(slotReadCount_.size()) && slotReadCount_[slot] == 0;
        }
        if (argReg >= 0) {
            savedReg = 0;
        }
        auto [it, inserted] = localScopes_.back().emplace(name, Symbol{false, isConst, constValue.has_value(), constValue.value_or(0), !copyOf.empty(), std::move(copyOf), {}, offset, savedReg, isDead, argReg});
        (void)inserted;
        return it->second;
    }

    int directParamArgReg(std::size_t index) const
    {
        if (!options_.optimize || currentLayout_.savesRa || index == 0 || index >= 8) {
            return -1;
        }
        return static_cast<int>(index);
    }

    const Symbol& lookup(const std::string& name) const
    {
        for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return found->second;
            }
        }
        const auto global = globalSymbols_.find(name);
        if (global != globalSymbols_.end()) {
            return global->second;
        }
        throw CodegenError("unknown symbol in codegen: " + name);
    }

    Symbol& lookupMutable(const std::string& name)
    {
        for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return found->second;
            }
        }
        const auto global = globalSymbols_.find(name);
        if (global != globalSymbols_.end()) {
            return global->second;
        }
        throw CodegenError("unknown symbol in codegen: " + name);
    }

    bool emitBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            pushScope();
        }
        bool terminal = false;
        for (const auto& stmt : block.statements) {
            terminal = emitStmt(*stmt);
            if (options_.optimize && terminal) {
                break;
            }
        }
        if (createsScope) {
            popScope();
        }
        return terminal;
    }

    bool emitStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return false;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            if (options_.optimize && isPure(*exprStmt->expr)) {
                return false;
            }
            emitExpr(*exprStmt->expr);
            return false;
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            const auto knownValue = options_.optimize ? tryEvalConst(*assign->value) : std::optional<std::int32_t>{};
            const std::string copyOf = options_.optimize ? copySourceName(*assign->value) : std::string{};
            const Symbol& target = lookup(assign->name);
            if (options_.optimize && target.isDead) {
                if (!isPure(*assign->value)) {
                    emitExpr(*assign->value);
                }
                return false;
            }
            if (options_.optimize && knownValue.has_value()) {
                storeImmediate(target, *knownValue);
                updateKnownValue(assign->name, knownValue, copyOf);
                return false;
            }
            if (options_.optimize && emitOptimizedAssignment(*assign)) {
                updateKnownValue(assign->name, knownValue, copyOf);
                return false;
            }
            emitExpr(*assign->value);
            storeSymbol(assign->name);
            if (options_.optimize) {
                updateKnownValue(assign->name, knownValue, copyOf);
            }
            return false;
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            const auto& decl = *declStmt->decl;
            std::optional<std::int32_t> constValue;
            std::string copyOf;
            if (options_.optimize && decl.isConst) {
                constValue = tryEvalConst(*decl.init);
            }
            if (options_.optimize && !constValue.has_value()) {
                constValue = tryEvalConst(*decl.init);
                copyOf = copySourceName(*decl.init);
            }
            const Symbol& symbol = allocateLocal(decl.name, decl.isConst, constValue, copyOf);
            if (options_.optimize && symbol.isDead) {
                if (!isPure(*decl.init)) {
                    emitExpr(*decl.init);
                }
                return false;
            }
            if (options_.optimize && constValue.has_value()) {
                if (!decl.isConst) {
                    storeImmediate(symbol, *constValue);
                }
                return false;
            }
            emitExpr(*decl.init);
            storeRegisterOrStack(symbol, "a0");
            return false;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            return emitBlock(*block, true);
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            if (options_.optimize) {
                if (const auto cond = tryEvalConst(*ifStmt->cond)) {
                    if (*cond != 0) {
                        return emitStmt(*ifStmt->thenBranch);
                    }
                    if (ifStmt->elseBranch != nullptr) {
                        return emitStmt(*ifStmt->elseBranch);
                    }
                    return false;
                }
            }
            const std::string elseLabel = newLabel(".L_else_");
            const std::string endLabel = newLabel(".L_endif_");
            emitBranchIfZero(*ifStmt->cond, elseLabel);
            const auto scopesBeforeBranches = localScopes_;
            const bool thenTerminal = emitStmt(*ifStmt->thenBranch);
            if (!thenTerminal) {
                out_ << "  j " << endLabel << "\n";
            }
            out_ << elseLabel << ":\n";
            localScopes_ = scopesBeforeBranches;
            bool elseTerminal = false;
            if (ifStmt->elseBranch != nullptr) {
                elseTerminal = emitStmt(*ifStmt->elseBranch);
            }
            out_ << endLabel << ":\n";
            localScopes_ = scopesBeforeBranches;
            clearMutableKnowledge();
            return ifStmt->elseBranch != nullptr && thenTerminal && elseTerminal;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            if (options_.optimize) {
                if (const auto cond = tryEvalConst(*whileStmt->cond); cond.has_value() && *cond == 0) {
                    return false;
                }
                clearMutableKnowledge();
            }
            if (options_.optimize) {
                const std::string bodyLabel = newLabel(".L_while_body_");
                const std::string condLabel = newLabel(".L_while_cond_");
                const std::string endLabel = newLabel(".L_while_end_");
                continueLabels_.push_back(condLabel);
                breakLabels_.push_back(endLabel);
                out_ << "  j " << condLabel << "\n";
                out_ << bodyLabel << ":\n";
                emitStmt(*whileStmt->body);
                out_ << condLabel << ":\n";
                clearMutableKnowledge();
                emitBranchIfNotZero(*whileStmt->cond, bodyLabel);
                out_ << endLabel << ":\n";
                continueLabels_.pop_back();
                breakLabels_.pop_back();
                clearMutableKnowledge();
                return false;
            }
            const std::string condLabel = newLabel(".L_while_cond_");
            const std::string endLabel = newLabel(".L_while_end_");
            continueLabels_.push_back(condLabel);
            breakLabels_.push_back(endLabel);
            out_ << condLabel << ":\n";
            emitBranchIfZero(*whileStmt->cond, endLabel);
            const bool bodyTerminal = emitStmt(*whileStmt->body);
            if (!bodyTerminal) {
                out_ << "  j " << condLabel << "\n";
            }
            out_ << endLabel << ":\n";
            continueLabels_.pop_back();
            breakLabels_.pop_back();
            if (options_.optimize) {
                clearMutableKnowledge();
            }
            return false;
        }
        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            out_ << "  j " << breakLabels_.back() << "\n";
            return true;
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            out_ << "  j " << continueLabels_.back() << "\n";
            return true;
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (ret->value != nullptr) {
                if (options_.optimize) {
                    if (const auto* call = dynamic_cast<const CallExpr*>(ret->value.get())) {
                        if (emitTailCall(*call)) {
                            return true;
                        }
                    }
                }
                emitExpr(*ret->value);
            } else if (!options_.optimize || currentFunction_ == nullptr || currentFunction_->returnType != Type::Void) {
                out_ << "  li a0, 0\n";
            }
            out_ << "  j " << returnLabel_ << "\n";
            return true;
        }
        throw CodegenError("unknown statement in codegen");
    }

    void emitExpr(const Expr& expr)
    {
        if (options_.optimize) {
            if (const auto value = tryEvalConst(expr)) {
                out_ << "  li a0, " << *value << "\n";
                return;
            }
        }
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            out_ << "  li a0, " << intExpr->value << "\n";
            return;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            loadSymbol(name->name);
            return;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            emitCall(*call);
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            emitExpr(*unary->operand);
            switch (unary->op) {
            case UnaryOp::Plus:
                break;
            case UnaryOp::Minus:
                out_ << "  neg a0, a0\n";
                break;
            case UnaryOp::Not:
                out_ << "  seqz a0, a0\n";
                break;
            }
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            emitBinary(*binary);
            return;
        }
        throw CodegenError("unknown expression in codegen");
    }

    void emitBinary(const BinaryExpr& binary)
    {
        if (options_.optimize && emitSimplifiedBinary(binary)) {
            return;
        }
        if (binary.op == BinaryOp::LogicalOr) {
            const std::string trueLabel = newLabel(".L_or_true_");
            const std::string endLabel = newLabel(".L_or_end_");
            emitExpr(*binary.lhs);
            out_ << "  bnez a0, " << trueLabel << "\n";
            emitExpr(*binary.rhs);
            out_ << "  snez a0, a0\n";
            out_ << "  j " << endLabel << "\n";
            out_ << trueLabel << ":\n";
            out_ << "  li a0, 1\n";
            out_ << endLabel << ":\n";
            return;
        }
        if (binary.op == BinaryOp::LogicalAnd) {
            const std::string falseLabel = newLabel(".L_and_false_");
            const std::string endLabel = newLabel(".L_and_end_");
            emitExpr(*binary.lhs);
            out_ << "  beqz a0, " << falseLabel << "\n";
            emitExpr(*binary.rhs);
            out_ << "  snez a0, a0\n";
            out_ << "  j " << endLabel << "\n";
            out_ << falseLabel << ":\n";
            out_ << "  li a0, 0\n";
            out_ << endLabel << ":\n";
            return;
        }

        if (options_.optimize && emitCommonSubexprBinary(binary)) {
            return;
        }
        if (options_.optimize && emitImmediateBinary(binary)) {
            return;
        }
        if (emitBinaryWithSimpleRhs(binary)) {
            return;
        }

        emitExpr(*binary.lhs);
        pushA0();
        emitExpr(*binary.rhs);
        popTo("t0");

        emitBinaryOperation(binary.op);
    }

    bool emitCommonSubexprBinary(const BinaryExpr& binary)
    {
        if (!isPure(*binary.lhs) || !sameExpr(*binary.lhs, *binary.rhs)) {
            return false;
        }

        switch (binary.op) {
        case BinaryOp::Mul:
            emitExpr(*binary.lhs);
            out_ << "  mul a0, a0, a0\n";
            return true;
        case BinaryOp::Div:
            out_ << "  li a0, 1\n";
            return true;
        case BinaryOp::Mod:
            out_ << "  li a0, 0\n";
            return true;
        case BinaryOp::LogicalOr:
        case BinaryOp::LogicalAnd:
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual:
        case BinaryOp::Add:
        case BinaryOp::Sub:
            break;
        }
        return false;
    }

    bool emitImmediateBinary(const BinaryExpr& binary)
    {
        const auto rhsConst = tryEvalConst(*binary.rhs);
        if (!rhsConst.has_value()) {
            return false;
        }

        switch (binary.op) {
        case BinaryOp::Add:
            if (fitsSigned12(*rhsConst)) {
                emitExpr(*binary.lhs);
                out_ << "  addi a0, a0, " << *rhsConst << "\n";
                return true;
            }
            break;
        case BinaryOp::Sub:
            if (*rhsConst != std::numeric_limits<std::int32_t>::min() && fitsSigned12(static_cast<std::int32_t>(-*rhsConst))) {
                emitExpr(*binary.lhs);
                out_ << "  addi a0, a0, " << -*rhsConst << "\n";
                return true;
            }
            break;
        case BinaryOp::Equal:
            if (*rhsConst == 0) {
                emitExpr(*binary.lhs);
                out_ << "  seqz a0, a0\n";
                return true;
            }
            break;
        case BinaryOp::NotEqual:
            if (*rhsConst == 0) {
                emitExpr(*binary.lhs);
                out_ << "  snez a0, a0\n";
                return true;
            }
            break;
        case BinaryOp::Less:
            if (fitsSigned12(*rhsConst)) {
                emitExpr(*binary.lhs);
                out_ << "  slti a0, a0, " << *rhsConst << "\n";
                return true;
            }
            break;
        case BinaryOp::LessEqual:
            if (*rhsConst < std::numeric_limits<std::int32_t>::max() && fitsSigned12(*rhsConst + 1)) {
                emitExpr(*binary.lhs);
                out_ << "  slti a0, a0, " << (*rhsConst + 1) << "\n";
                return true;
            }
            break;
        case BinaryOp::Greater:
            if (*rhsConst < std::numeric_limits<std::int32_t>::max() && fitsSigned12(*rhsConst + 1)) {
                emitExpr(*binary.lhs);
                out_ << "  slti a0, a0, " << (*rhsConst + 1) << "\n";
                out_ << "  xori a0, a0, 1\n";
                return true;
            }
            break;
        case BinaryOp::GreaterEqual:
            if (fitsSigned12(*rhsConst)) {
                emitExpr(*binary.lhs);
                out_ << "  slti a0, a0, " << *rhsConst << "\n";
                out_ << "  xori a0, a0, 1\n";
                return true;
            }
            break;
        case BinaryOp::LogicalOr:
        case BinaryOp::LogicalAnd:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            break;
        }
        return false;
    }

    bool emitBinaryWithSimpleRhs(const BinaryExpr& binary)
    {
        if (!isSimpleValue(*binary.rhs)) {
            return false;
        }
        if (isSimpleValue(*binary.lhs)) {
            emitSimpleToRegister(*binary.lhs, "t0");
        } else {
            emitExpr(*binary.lhs);
            out_ << "  mv t0, a0\n";
        }
        emitSimpleToRegister(*binary.rhs, "a0");
        emitBinaryOperation(binary.op);
        return true;
    }

    void emitBinaryOperation(BinaryOp op)
    {
        switch (op) {
        case BinaryOp::Equal:
            out_ << "  sub a0, t0, a0\n";
            out_ << "  seqz a0, a0\n";
            break;
        case BinaryOp::NotEqual:
            out_ << "  sub a0, t0, a0\n";
            out_ << "  snez a0, a0\n";
            break;
        case BinaryOp::Less:
            out_ << "  slt a0, t0, a0\n";
            break;
        case BinaryOp::LessEqual:
            out_ << "  slt a0, a0, t0\n";
            out_ << "  xori a0, a0, 1\n";
            break;
        case BinaryOp::Greater:
            out_ << "  slt a0, a0, t0\n";
            break;
        case BinaryOp::GreaterEqual:
            out_ << "  slt a0, t0, a0\n";
            out_ << "  xori a0, a0, 1\n";
            break;
        case BinaryOp::Add:
            out_ << "  add a0, t0, a0\n";
            break;
        case BinaryOp::Sub:
            out_ << "  sub a0, t0, a0\n";
            break;
        case BinaryOp::Mul:
            out_ << "  mul a0, t0, a0\n";
            break;
        case BinaryOp::Div:
            out_ << "  div a0, t0, a0\n";
            break;
        case BinaryOp::Mod:
            out_ << "  rem a0, t0, a0\n";
            break;
        case BinaryOp::LogicalOr:
        case BinaryOp::LogicalAnd:
            break;
        }
    }

    void emitBranchIfZero(const Expr& expr, const std::string& label)
    {
        if (options_.optimize && emitOptimizedBranch(expr, label, false)) {
            return;
        }
        emitExpr(expr);
        out_ << "  beqz a0, " << label << "\n";
    }

    void emitBranchIfNotZero(const Expr& expr, const std::string& label)
    {
        if (options_.optimize && emitOptimizedBranch(expr, label, true)) {
            return;
        }
        emitExpr(expr);
        out_ << "  bnez a0, " << label << "\n";
    }

    bool emitOptimizedBranch(const Expr& expr, const std::string& label, bool branchWhenTrue)
    {
        if (const auto value = tryEvalConst(expr)) {
            if ((*value != 0) == branchWhenTrue) {
                out_ << "  j " << label << "\n";
            }
            return true;
        }

        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            if (unary->op == UnaryOp::Not) {
                return emitOptimizedBranch(*unary->operand, label, !branchWhenTrue);
            }
        }

        const auto* binary = dynamic_cast<const BinaryExpr*>(&expr);
        if (binary == nullptr) {
            return false;
        }

        if (binary->op == BinaryOp::LogicalAnd) {
            if (branchWhenTrue) {
                const std::string endLabel = newLabel(".L_and_skip_");
                emitBranchIfZero(*binary->lhs, endLabel);
                emitBranchIfNotZero(*binary->rhs, label);
                out_ << endLabel << ":\n";
            } else {
                emitBranchIfZero(*binary->lhs, label);
                emitBranchIfZero(*binary->rhs, label);
            }
            return true;
        }
        if (binary->op == BinaryOp::LogicalOr) {
            if (branchWhenTrue) {
                emitBranchIfNotZero(*binary->lhs, label);
                emitBranchIfNotZero(*binary->rhs, label);
            } else {
                const std::string endLabel = newLabel(".L_or_skip_");
                emitBranchIfNotZero(*binary->lhs, endLabel);
                emitBranchIfZero(*binary->rhs, label);
                out_ << endLabel << ":\n";
            }
            return true;
        }

        return emitRelationalBranch(*binary, label, branchWhenTrue);
    }

    bool emitRelationalBranch(const BinaryExpr& binary, const std::string& label, bool branchWhenTrue)
    {
        switch (binary.op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual:
            break;
        case BinaryOp::LogicalOr:
        case BinaryOp::LogicalAnd:
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            return false;
        }

        if (const auto rhsConst = tryEvalConst(*binary.rhs);
            rhsConst.has_value() && *rhsConst == 0 && (binary.op == BinaryOp::Equal || binary.op == BinaryOp::NotEqual)) {
            const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
            const bool jumpOnZero = (binary.op == BinaryOp::Equal) == branchWhenTrue;
            out_ << "  " << (jumpOnZero ? "beqz" : "bnez") << " " << lhsReg << ", " << label << "\n";
            return true;
        }
        if (emitImmediateRelationalBranch(binary, label, branchWhenTrue)) {
            return true;
        }

        emitExpr(*binary.lhs);
        if (isSimpleValue(*binary.rhs)) {
            out_ << "  mv t0, a0\n";
            emitSimpleToRegister(*binary.rhs, "a0");
        } else {
            pushA0();
            emitExpr(*binary.rhs);
            popTo("t0");
        }

        if (branchWhenTrue) {
            switch (binary.op) {
            case BinaryOp::Equal:
                out_ << "  beq t0, a0, " << label << "\n";
                return true;
            case BinaryOp::NotEqual:
                out_ << "  bne t0, a0, " << label << "\n";
                return true;
            case BinaryOp::Less:
                out_ << "  blt t0, a0, " << label << "\n";
                return true;
            case BinaryOp::LessEqual:
                out_ << "  bge a0, t0, " << label << "\n";
                return true;
            case BinaryOp::Greater:
                out_ << "  blt a0, t0, " << label << "\n";
                return true;
            case BinaryOp::GreaterEqual:
                out_ << "  bge t0, a0, " << label << "\n";
                return true;
            case BinaryOp::LogicalOr:
            case BinaryOp::LogicalAnd:
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul:
            case BinaryOp::Div:
            case BinaryOp::Mod:
                break;
            }
        } else {
            switch (binary.op) {
            case BinaryOp::Equal:
                out_ << "  bne t0, a0, " << label << "\n";
                return true;
            case BinaryOp::NotEqual:
                out_ << "  beq t0, a0, " << label << "\n";
                return true;
            case BinaryOp::Less:
                out_ << "  bge t0, a0, " << label << "\n";
                return true;
            case BinaryOp::LessEqual:
                out_ << "  blt a0, t0, " << label << "\n";
                return true;
            case BinaryOp::Greater:
                out_ << "  bge a0, t0, " << label << "\n";
                return true;
            case BinaryOp::GreaterEqual:
                out_ << "  blt t0, a0, " << label << "\n";
                return true;
            case BinaryOp::LogicalOr:
            case BinaryOp::LogicalAnd:
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul:
            case BinaryOp::Div:
            case BinaryOp::Mod:
                break;
            }
        }
        return false;
    }

    bool emitImmediateRelationalBranch(const BinaryExpr& binary, const std::string& label, bool branchWhenTrue)
    {
        const auto rhsConst = tryEvalConst(*binary.rhs);
        if (!rhsConst.has_value()) {
            return false;
        }

        auto emitBooleanBranch = [&](bool trueWhenNonZero) {
            out_ << "  " << ((branchWhenTrue == trueWhenNonZero) ? "bnez" : "beqz") << " t0, " << label << "\n";
        };

        switch (binary.op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
            if (*rhsConst != std::numeric_limits<std::int32_t>::min()
                && fitsSigned12(static_cast<std::int32_t>(-*rhsConst))) {
                const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
                out_ << "  addi t0, " << lhsReg << ", " << static_cast<std::int32_t>(-*rhsConst) << "\n";
                const bool nonZeroMeansTrue = binary.op == BinaryOp::NotEqual;
                emitBooleanBranch(nonZeroMeansTrue);
                return true;
            }
            break;
        case BinaryOp::Less:
            if (fitsSigned12(*rhsConst)) {
                const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
                out_ << "  slti t0, " << lhsReg << ", " << *rhsConst << "\n";
                emitBooleanBranch(true);
                return true;
            }
            break;
        case BinaryOp::LessEqual:
            if (*rhsConst < std::numeric_limits<std::int32_t>::max() && fitsSigned12(*rhsConst + 1)) {
                const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
                out_ << "  slti t0, " << lhsReg << ", " << (*rhsConst + 1) << "\n";
                emitBooleanBranch(true);
                return true;
            }
            break;
        case BinaryOp::Greater:
            if (*rhsConst < std::numeric_limits<std::int32_t>::max() && fitsSigned12(*rhsConst + 1)) {
                const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
                out_ << "  slti t0, " << lhsReg << ", " << (*rhsConst + 1) << "\n";
                emitBooleanBranch(false);
                return true;
            }
            break;
        case BinaryOp::GreaterEqual:
            if (fitsSigned12(*rhsConst)) {
                const std::string lhsReg = simpleOperandRegister(*binary.lhs, "a0");
                out_ << "  slti t0, " << lhsReg << ", " << *rhsConst << "\n";
                emitBooleanBranch(false);
                return true;
            }
            break;
        case BinaryOp::LogicalOr:
        case BinaryOp::LogicalAnd:
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            break;
        }
        return false;
    }

    bool isSimpleValue(const Expr& expr)
    {
        if (tryEvalConst(expr).has_value()) {
            return true;
        }
        return dynamic_cast<const IntExpr*>(&expr) != nullptr || dynamic_cast<const NameExpr*>(&expr) != nullptr;
    }

    void emitSimpleToRegister(const Expr& expr, const char* reg)
    {
        if (const auto value = tryEvalConst(expr)) {
            out_ << "  li " << reg << ", " << *value << "\n";
            return;
        }
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            out_ << "  li " << reg << ", " << intExpr->value << "\n";
            return;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const Symbol& symbol = lookup(name->name);
            if (options_.optimize && !symbol.hasConstValue && symbol.hasCopy) {
                emitSimpleToRegister(NameExpr(symbol.copyOf), reg);
                return;
            }
            if (symbol.isGlobal) {
                out_ << "  la t1, " << symbol.label << "\n";
                out_ << "  lw " << reg << ", 0(t1)\n";
            } else if (symbol.argReg >= 0) {
                const std::string source = "a" + std::to_string(symbol.argReg);
                if (source != reg) {
                    out_ << "  mv " << reg << ", " << source << "\n";
                }
            } else if (symbol.savedReg != 0) {
                out_ << "  mv " << reg << ", " << savedRegName(symbol.savedReg) << "\n";
            } else {
                out_ << "  lw " << reg << ", " << symbol.offset << "(s0)\n";
            }
            return;
        }
        emitExpr(expr);
        if (std::string(reg) != "a0") {
            out_ << "  mv " << reg << ", a0\n";
        }
    }

    void emitCall(const CallExpr& call)
    {
        if (options_.optimize && emitSimpleCall(call)) {
            return;
        }
        if (options_.optimize && emitNoNestedCall(call)) {
            return;
        }

        int callReserve = 0;
        if (call.args.size() > 8) {
            callReserve += alignTo(static_cast<int>(call.args.size() - 8) * 4, 16);
        }
        const int alignPad = (16 - ((evalStackBytes_ + callReserve) % 16)) % 16;
        callReserve += alignPad;
        if (callReserve > 0) {
            out_ << "  addi sp, sp, -" << callReserve << "\n";
            evalStackBytes_ += callReserve;
        }

        for (const auto& arg : call.args) {
            emitExpr(*arg);
            pushA0();
        }

        for (int i = static_cast<int>(call.args.size()) - 1; i >= 0; --i) {
            popTo("a0");
            if (i < 8) {
                if (i != 0) {
                    out_ << "  mv a" << i << ", a0\n";
                }
            } else {
                out_ << "  sw a0, " << (i * 4 + (i - 8) * 4) << "(sp)\n";
            }
        }
        out_ << "  call " << call.callee << "\n";

        if (callReserve > 0) {
            out_ << "  addi sp, sp, " << callReserve << "\n";
            evalStackBytes_ -= callReserve;
        }
    }

    bool emitSimpleCall(const CallExpr& call)
    {
        for (const auto& arg : call.args) {
            if (!isSimpleValue(*arg)) {
                return false;
            }
        }

        int callReserve = 0;
        if (call.args.size() > 8) {
            callReserve += alignTo(static_cast<int>(call.args.size() - 8) * 4, 16);
        }
        const int alignPad = (16 - ((evalStackBytes_ + callReserve) % 16)) % 16;
        callReserve += alignPad;
        if (callReserve > 0) {
            out_ << "  addi sp, sp, -" << callReserve << "\n";
            evalStackBytes_ += callReserve;
        }

        for (std::size_t i = 0; i < call.args.size(); ++i) {
            if (i < 8) {
                const std::string reg = "a" + std::to_string(i);
                emitSimpleToRegister(*call.args[i], reg.c_str());
            } else {
                emitSimpleToRegister(*call.args[i], "t0");
                out_ << "  sw t0, " << static_cast<int>((i - 8) * 4) << "(sp)\n";
            }
        }
        out_ << "  call " << call.callee << "\n";

        if (callReserve > 0) {
            out_ << "  addi sp, sp, " << callReserve << "\n";
            evalStackBytes_ -= callReserve;
        }
        return true;
    }

    bool emitNoNestedCall(const CallExpr& call)
    {
        for (const auto& arg : call.args) {
            if (containsCall(*arg)) {
                return false;
            }
        }

        int callReserve = 0;
        if (call.args.size() > 8) {
            callReserve += alignTo(static_cast<int>(call.args.size() - 8) * 4, 16);
        }
        const int alignPad = (16 - ((evalStackBytes_ + callReserve) % 16)) % 16;
        callReserve += alignPad;
        if (callReserve > 0) {
            out_ << "  addi sp, sp, -" << callReserve << "\n";
            evalStackBytes_ += callReserve;
        }

        for (int i = static_cast<int>(call.args.size()) - 1; i >= 0; --i) {
            emitExpr(*call.args[static_cast<std::size_t>(i)]);
            if (i < 8) {
                if (i != 0) {
                    out_ << "  mv a" << i << ", a0\n";
                }
            } else {
                out_ << "  sw a0, " << static_cast<int>((i - 8) * 4) << "(sp)\n";
            }
        }
        out_ << "  call " << call.callee << "\n";

        if (callReserve > 0) {
            out_ << "  addi sp, sp, " << callReserve << "\n";
            evalStackBytes_ -= callReserve;
        }
        return true;
    }

    void pushA0()
    {
        out_ << "  addi sp, sp, -4\n";
        out_ << "  sw a0, 0(sp)\n";
        evalStackBytes_ += 4;
    }

    void popTo(const char* reg)
    {
        out_ << "  lw " << reg << ", 0(sp)\n";
        out_ << "  addi sp, sp, 4\n";
        evalStackBytes_ -= 4;
    }

    void loadSymbol(const std::string& name)
    {
        const Symbol& symbol = lookup(name);
        if (options_.optimize && !symbol.hasConstValue && symbol.hasCopy) {
            loadSymbol(symbol.copyOf);
            return;
        }
        if (symbol.isGlobal) {
            out_ << "  la t0, " << symbol.label << "\n";
            out_ << "  lw a0, 0(t0)\n";
        } else if (symbol.argReg >= 0) {
            if (symbol.argReg != 0) {
                out_ << "  mv a0, a" << symbol.argReg << "\n";
            }
        } else if (symbol.savedReg != 0) {
            out_ << "  mv a0, " << savedRegName(symbol.savedReg) << "\n";
        } else {
            out_ << "  lw a0, " << symbol.offset << "(s0)\n";
        }
    }

    void storeSymbol(const std::string& name)
    {
        const Symbol& symbol = lookup(name);
        if (symbol.isGlobal) {
            out_ << "  la t0, " << symbol.label << "\n";
            out_ << "  sw a0, 0(t0)\n";
        } else if (symbol.argReg >= 0) {
            if (symbol.argReg != 0) {
                out_ << "  mv a" << symbol.argReg << ", a0\n";
            }
        } else if (symbol.savedReg != 0) {
            out_ << "  mv " << savedRegName(symbol.savedReg) << ", a0\n";
        } else {
            out_ << "  sw a0, " << symbol.offset << "(s0)\n";
        }
    }

    void storeRegisterOrStack(const Symbol& symbol, const std::string& sourceReg)
    {
        if (symbol.savedReg != 0) {
            out_ << "  mv " << savedRegName(symbol.savedReg) << ", " << sourceReg << "\n";
        } else if (symbol.argReg >= 0) {
            const std::string target = "a" + std::to_string(symbol.argReg);
            if (target != sourceReg) {
                out_ << "  mv " << target << ", " << sourceReg << "\n";
            }
        } else {
            out_ << "  sw " << sourceReg << ", " << symbol.offset << "(s0)\n";
        }
    }

    void storeImmediate(const Symbol& symbol, std::int32_t value)
    {
        if (symbol.isGlobal) {
            out_ << "  la t0, " << symbol.label << "\n";
            out_ << "  li t1, " << value << "\n";
            out_ << "  sw t1, 0(t0)\n";
        } else if (symbol.argReg >= 0) {
            out_ << "  li a" << symbol.argReg << ", " << value << "\n";
        } else if (symbol.savedReg != 0) {
            out_ << "  li " << savedRegName(symbol.savedReg) << ", " << value << "\n";
        } else {
            out_ << "  li t0, " << value << "\n";
            out_ << "  sw t0, " << symbol.offset << "(s0)\n";
        }
    }

    std::string simpleOperandRegister(const Expr& expr, const char* scratch)
    {
        if (const auto value = tryEvalConst(expr)) {
            out_ << "  li " << scratch << ", " << *value << "\n";
            return scratch;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const Symbol& symbol = lookup(name->name);
            if (options_.optimize && !symbol.hasConstValue && symbol.hasCopy) {
                return simpleOperandRegister(NameExpr(symbol.copyOf), scratch);
            }
            if (!symbol.isGlobal && symbol.savedReg != 0) {
                return savedRegName(symbol.savedReg);
            }
            if (!symbol.isGlobal && symbol.argReg >= 0) {
                return std::string("a") + std::to_string(symbol.argReg);
            }
        }
        emitSimpleToRegister(expr, scratch);
        return scratch;
    }

    bool emitOptimizedAssignment(const AssignStmt& assign)
    {
        if (isName(*assign.value, assign.name)) {
            return true;
        }

        const Symbol& target = lookup(assign.name);
        if (target.savedReg == 0) {
            return false;
        }

        const auto* binary = dynamic_cast<const BinaryExpr*>(assign.value.get());
        if (binary == nullptr) {
            return false;
        }

        const char* dst = savedRegName(target.savedReg);
        if (emitSelfImmediateUpdate(assign.name, *binary, dst)) {
            return true;
        }
        if (emitSelfRegisterUpdate(assign.name, *binary, dst)) {
            return true;
        }
        return false;
    }

    bool emitSelfImmediateUpdate(const std::string& name, const BinaryExpr& binary, const char* dst)
    {
        if (binary.op == BinaryOp::Add || binary.op == BinaryOp::Sub) {
            if (isName(*binary.lhs, name)) {
                const auto rhsConst = tryEvalConst(*binary.rhs);
                if (rhsConst.has_value()) {
                    if (binary.op == BinaryOp::Sub && *rhsConst == std::numeric_limits<std::int32_t>::min()) {
                        return false;
                    }
                    const std::int32_t delta = binary.op == BinaryOp::Add ? *rhsConst : static_cast<std::int32_t>(-*rhsConst);
                    if (fitsSigned12(delta)) {
                        out_ << "  addi " << dst << ", " << dst << ", " << delta << "\n";
                        return true;
                    }
                }
            }
            if (binary.op == BinaryOp::Add && isName(*binary.rhs, name)) {
                const auto lhsConst = tryEvalConst(*binary.lhs);
                if (lhsConst.has_value() && fitsSigned12(*lhsConst)) {
                    out_ << "  addi " << dst << ", " << dst << ", " << *lhsConst << "\n";
                    return true;
                }
            }
        }

        if (binary.op == BinaryOp::Mul) {
            std::optional<std::int32_t> factor;
            if (isName(*binary.lhs, name)) {
                factor = tryEvalConst(*binary.rhs);
            } else if (isName(*binary.rhs, name)) {
                factor = tryEvalConst(*binary.lhs);
            }
            if (factor.has_value()) {
                if (*factor == 0) {
                    out_ << "  li " << dst << ", 0\n";
                    return true;
                }
                if (*factor == 1) {
                    return true;
                }
                if (*factor == -1) {
                    out_ << "  neg " << dst << ", " << dst << "\n";
                    return true;
                }
                if (*factor > 0 && (*factor & (*factor - 1)) == 0) {
                    out_ << "  slli " << dst << ", " << dst << ", " << trailingZeroBits(*factor) << "\n";
                    return true;
                }
            }
        }
        return false;
    }

    bool emitSelfRegisterUpdate(const std::string& name, const BinaryExpr& binary, const char* dst)
    {
        if ((binary.op == BinaryOp::Add || binary.op == BinaryOp::Sub) && isName(*binary.lhs, name) && isSimpleValue(*binary.rhs)) {
            emitSimpleToRegister(*binary.rhs, "t0");
            out_ << "  " << (binary.op == BinaryOp::Add ? "add" : "sub") << " " << dst << ", " << dst << ", t0\n";
            return true;
        }
        if (binary.op == BinaryOp::Add && isName(*binary.rhs, name) && isSimpleValue(*binary.lhs)) {
            emitSimpleToRegister(*binary.lhs, "t0");
            out_ << "  add " << dst << ", " << dst << ", t0\n";
            return true;
        }
        return false;
    }

    bool isName(const Expr& expr, const std::string& name) const
    {
        const auto* nameExpr = dynamic_cast<const NameExpr*>(&expr);
        return nameExpr != nullptr && nameExpr->name == name;
    }

    std::string copySourceName(const Expr& expr) const
    {
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const Symbol& symbol = lookup(name->name);
            if (!symbol.isGlobal) {
                return name->name;
            }
        }
        return {};
    }

    void updateKnownValue(const std::string& name, std::optional<std::int32_t> value, const std::string& copyOf)
    {
        Symbol& symbol = lookupMutable(name);
        if (!symbol.isGlobal) {
            symbol.hasConstValue = value.has_value();
            symbol.constValue = value.value_or(0);
            symbol.hasCopy = !copyOf.empty();
            symbol.copyOf = copyOf;
        }
        invalidateCopiesOf(name);
    }

    void invalidateCopiesOf(const std::string& name)
    {
        for (auto& scope : localScopes_) {
            for (auto& [symbolName, symbol] : scope) {
                if (symbolName != name && symbol.hasCopy && symbol.copyOf == name) {
                    symbol.hasCopy = false;
                    symbol.copyOf.clear();
                }
            }
        }
    }

    void clearMutableKnowledge()
    {
        for (auto& scope : localScopes_) {
            for (auto& [name, symbol] : scope) {
                (void)name;
                if (!symbol.isConst) {
                    symbol.hasConstValue = false;
                    symbol.constValue = 0;
                    symbol.hasCopy = false;
                    symbol.copyOf.clear();
                }
            }
        }
    }

    bool emitTailCall(const CallExpr& call)
    {
        if (currentFunction_ == nullptr || call.callee != currentFunction_->name || call.args.size() != currentFunction_->params.size()) {
            return false;
        }
        if (call.args.empty()) {
            out_ << "  j " << functionBodyLabel_ << "\n";
            return true;
        }

        const int last = static_cast<int>(call.args.size()) - 1;
        for (int i = 0; i < last; ++i) {
            emitExpr(*call.args[static_cast<std::size_t>(i)]);
            pushA0();
        }
        emitExpr(*call.args[static_cast<std::size_t>(last)]);
        storeSymbol(currentFunction_->params[static_cast<std::size_t>(last)].name);
        for (int i = last - 1; i >= 0; --i) {
            popTo("a0");
            storeSymbol(currentFunction_->params[static_cast<std::size_t>(i)].name);
        }
        out_ << "  j " << functionBodyLabel_ << "\n";
        return true;
    }

    static const IntExpr* asInt(const Expr& expr)
    {
        return dynamic_cast<const IntExpr*>(&expr);
    }

    bool isPure(const Expr& expr) const
    {
        if (dynamic_cast<const IntExpr*>(&expr) != nullptr || dynamic_cast<const NameExpr*>(&expr) != nullptr) {
            return true;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            return isPure(*unary->operand);
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            return isPure(*binary->lhs) && isPure(*binary->rhs);
        }
        return false;
    }

    bool sameExpr(const Expr& lhs, const Expr& rhs) const
    {
        if (const auto* left = dynamic_cast<const IntExpr*>(&lhs)) {
            const auto* right = dynamic_cast<const IntExpr*>(&rhs);
            return right != nullptr && left->value == right->value;
        }
        if (const auto* left = dynamic_cast<const NameExpr*>(&lhs)) {
            const auto* right = dynamic_cast<const NameExpr*>(&rhs);
            return right != nullptr && left->name == right->name;
        }
        if (const auto* left = dynamic_cast<const UnaryExpr*>(&lhs)) {
            const auto* right = dynamic_cast<const UnaryExpr*>(&rhs);
            return right != nullptr && left->op == right->op && sameExpr(*left->operand, *right->operand);
        }
        if (const auto* left = dynamic_cast<const BinaryExpr*>(&lhs)) {
            const auto* right = dynamic_cast<const BinaryExpr*>(&rhs);
            return right != nullptr && left->op == right->op && sameExpr(*left->lhs, *right->lhs) && sameExpr(*left->rhs, *right->rhs);
        }
        return false;
    }

    bool emitSimplifiedBinary(const BinaryExpr& binary)
    {
        const IntExpr* lhsInt = asInt(*binary.lhs);
        const IntExpr* rhsInt = asInt(*binary.rhs);
        const auto lhsConst = tryEvalConst(*binary.lhs);
        const auto rhsConst = tryEvalConst(*binary.rhs);

        switch (binary.op) {
        case BinaryOp::Add:
            if (lhsConst.has_value() && *lhsConst != 0 && fitsSigned12(*lhsConst)) {
                emitExpr(*binary.rhs);
                out_ << "  addi a0, a0, " << *lhsConst << "\n";
                return true;
            }
            if (rhsConst.has_value() && *rhsConst == 0) {
                emitExpr(*binary.lhs);
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == 0) {
                emitExpr(*binary.rhs);
                return true;
            }
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                emitExpr(*binary.lhs);
                out_ << "  slli a0, a0, 1\n";
                return true;
            }
            break;
        case BinaryOp::Sub:
            if (rhsConst.has_value() && *rhsConst == 0) {
                emitExpr(*binary.lhs);
                return true;
            }
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            break;
        case BinaryOp::Mul:
            if (rhsConst.has_value() && *rhsConst == 1) {
                emitExpr(*binary.lhs);
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == 1) {
                emitExpr(*binary.rhs);
                return true;
            }
            if (rhsConst.has_value() && *rhsConst == -1) {
                emitExpr(*binary.lhs);
                out_ << "  neg a0, a0\n";
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == -1) {
                emitExpr(*binary.rhs);
                out_ << "  neg a0, a0\n";
                return true;
            }
            if (rhsConst.has_value() && *rhsConst == 0 && isPure(*binary.lhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == 0 && isPure(*binary.rhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            if (rhsConst.has_value() && emitMulByConstant(*binary.lhs, *rhsConst)) {
                return true;
            }
            if (lhsConst.has_value() && emitMulByConstant(*binary.rhs, *lhsConst)) {
                return true;
            }
            if (rhsConst.has_value() && *rhsConst > 0 && (*rhsConst & (*rhsConst - 1)) == 0) {
                emitExpr(*binary.lhs);
                out_ << "  slli a0, a0, " << trailingZeroBits(*rhsConst) << "\n";
                return true;
            }
            if (lhsConst.has_value() && *lhsConst > 0 && (*lhsConst & (*lhsConst - 1)) == 0) {
                emitExpr(*binary.rhs);
                out_ << "  slli a0, a0, " << trailingZeroBits(*lhsConst) << "\n";
                return true;
            }
            break;
        case BinaryOp::Div:
            if (rhsConst.has_value() && *rhsConst == 1) {
                emitExpr(*binary.lhs);
                return true;
            }
            if (rhsConst.has_value() && *rhsConst == -1) {
                emitExpr(*binary.lhs);
                out_ << "  neg a0, a0\n";
                return true;
            }
            break;
        case BinaryOp::Mod:
            if (rhsConst.has_value() && *rhsConst == 1) {
                if (!isPure(*binary.lhs)) {
                    emitExpr(*binary.lhs);
                }
                out_ << "  li a0, 0\n";
                return true;
            }
            break;
        case BinaryOp::LogicalOr:
            if (lhsConst.has_value()) {
                if (*lhsConst != 0) {
                    out_ << "  li a0, 1\n";
                } else {
                    emitExpr(*binary.rhs);
                    out_ << "  snez a0, a0\n";
                }
                return true;
            }
            break;
        case BinaryOp::LogicalAnd:
            if (lhsConst.has_value()) {
                if (*lhsConst == 0) {
                    out_ << "  li a0, 0\n";
                } else {
                    emitExpr(*binary.rhs);
                    out_ << "  snez a0, a0\n";
                }
                return true;
            }
            break;
        case BinaryOp::Equal:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 1\n";
                return true;
            }
            break;
        case BinaryOp::NotEqual:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            break;
        case BinaryOp::Less:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            break;
        case BinaryOp::LessEqual:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 1\n";
                return true;
            }
            break;
        case BinaryOp::Greater:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 0\n";
                return true;
            }
            break;
        case BinaryOp::GreaterEqual:
            if (isPure(*binary.lhs) && sameExpr(*binary.lhs, *binary.rhs)) {
                out_ << "  li a0, 1\n";
                return true;
            }
            break;
        }
        return false;
    }

    bool emitMulByConstant(const Expr& expr, std::int32_t constant)
    {
        if (constant == 0 || constant == 1 || constant == -1 || constant == std::numeric_limits<std::int32_t>::min()) {
            return false;
        }

        std::uint32_t magnitude = constant < 0 ? static_cast<std::uint32_t>(-constant) : static_cast<std::uint32_t>(constant);
        int bits[32];
        int bitCount = 0;
        for (int bit = 0; bit < 31; ++bit) {
            if ((magnitude & (static_cast<std::uint32_t>(1) << bit)) != 0) {
                bits[bitCount++] = bit;
            }
        }
        if (bitCount <= 1 || bitCount > 3) {
            return false;
        }

        emitExpr(expr);
        out_ << "  mv t0, a0\n";
        for (int i = 0; i < bitCount; ++i) {
            const int bit = bits[i];
            const char* target = i == 0 ? "a0" : "t1";
            if (bit == 0) {
                out_ << "  mv " << target << ", t0\n";
            } else {
                out_ << "  slli " << target << ", t0, " << bit << "\n";
            }
            if (i > 0) {
                out_ << "  add a0, a0, t1\n";
            }
        }
        if (constant < 0) {
            out_ << "  neg a0, a0\n";
        }
        return true;
    }

    int trailingZeroBits(std::int32_t value) const
    {
        int count = 0;
        while ((value & 1) == 0) {
            ++count;
            value >>= 1;
        }
        return count;
    }

    std::int32_t requireConst(const Expr& expr)
    {
        if (const auto value = tryEvalConst(expr)) {
            return *value;
        }
        throw CodegenError("global initializer is not constant");
    }

    std::optional<std::int32_t> tryEvalConst(const Expr& expr)
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return intExpr->value;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const auto value = tryEvalConst(*unary->operand);
            if (!value.has_value()) {
                return std::nullopt;
            }
            switch (unary->op) {
            case UnaryOp::Plus:
                return *value;
            case UnaryOp::Minus:
                return -*value;
            case UnaryOp::Not:
                return *value == 0 ? 1 : 0;
            }
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            const auto lhs = tryEvalConst(*binary->lhs);
            const auto rhs = tryEvalConst(*binary->rhs);
            if (!lhs.has_value() || !rhs.has_value()) {
                return std::nullopt;
            }
            switch (binary->op) {
            case BinaryOp::LogicalOr:
                return (*lhs != 0 || *rhs != 0) ? 1 : 0;
            case BinaryOp::LogicalAnd:
                return (*lhs != 0 && *rhs != 0) ? 1 : 0;
            case BinaryOp::Equal:
                return *lhs == *rhs ? 1 : 0;
            case BinaryOp::NotEqual:
                return *lhs != *rhs ? 1 : 0;
            case BinaryOp::Less:
                return *lhs < *rhs ? 1 : 0;
            case BinaryOp::LessEqual:
                return *lhs <= *rhs ? 1 : 0;
            case BinaryOp::Greater:
                return *lhs > *rhs ? 1 : 0;
            case BinaryOp::GreaterEqual:
                return *lhs >= *rhs ? 1 : 0;
            case BinaryOp::Add:
                return *lhs + *rhs;
            case BinaryOp::Sub:
                return *lhs - *rhs;
            case BinaryOp::Mul:
                return *lhs * *rhs;
            case BinaryOp::Div:
                if (*rhs == 0) {
                    return std::nullopt;
                }
                return *lhs / *rhs;
            case BinaryOp::Mod:
                if (*rhs == 0) {
                    return std::nullopt;
                }
                return *lhs % *rhs;
            }
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const Symbol& symbol = lookup(name->name);
            if (symbol.hasConstValue) {
                return symbol.constValue;
            }
            if (symbol.hasCopy) {
                return tryEvalConst(NameExpr(symbol.copyOf));
            }
        }
        return std::nullopt;
    }

    std::string newLabel(const char* prefix)
    {
        return std::string(prefix) + std::to_string(nextLabel_++);
    }

    const Program& program_;
    std::ostream& out_;
    CodegenOptions options_;
    std::unordered_map<std::string, Symbol> globalSymbols_;
    std::unordered_map<std::string, bool> globalAssigned_;
    std::vector<std::unordered_map<std::string, Symbol>> localScopes_;
    const FuncDef* currentFunction_ = nullptr;
    FunctionLayout currentLayout_;
    int nextLocalOffset_ = -8;
    int nextLocalSlot_ = 0;
    int plannedSavedRegs_ = 0;
    int plannedCalleeSavedRegs_ = 0;
    int evalStackBytes_ = 0;
    int nextLabel_ = 0;
    std::string returnLabel_;
    std::string functionBodyLabel_;
    std::vector<std::string> breakLabels_;
    std::vector<std::string> continueLabels_;
    std::vector<int> registerForSlot_;
    std::vector<int> slotReadCount_;
};

} // namespace

RiscVCodeGenerator::RiscVCodeGenerator(CodegenOptions options)
    : options_(options)
{
}

void RiscVCodeGenerator::generate(const Program& program, std::ostream& out)
{
    if (options_.optimize) {
        Program optimized = optimizeAst(program);
        IrBuilder irBuilder;
        ir::Module module = irBuilder.build(optimized);
        PassManager passes = buildDefaultPassPipeline(true);
        passes.run(module);
        AstRiscVBackend backend(optimized, out, options_);
        backend.generate();
        return;
    }

    AstRiscVBackend backend(program, out, options_);
    backend.generate();
}

} // namespace toyc
