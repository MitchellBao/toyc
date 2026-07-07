#include "ast_optimizer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toyc {
namespace {

struct KnownValue {
    std::optional<std::int32_t> constant;
    bool hasCopy = false;
    std::string copyName;
    int copyBinding = -1;
};

struct Binding {
    int id = -1;
    bool isConst = false;
    KnownValue known;
};

ExprPtr intExpr(std::int32_t value)
{
    return std::make_unique<IntExpr>(value);
}

ExprPtr nameExpr(const std::string& name)
{
    return std::make_unique<NameExpr>(name);
}

ExprPtr cloneExpr(const Expr& expr)
{
    if (const auto* intValue = dynamic_cast<const IntExpr*>(&expr)) {
        return intExpr(intValue->value);
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
        return nameExpr(name->name);
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        std::vector<ExprPtr> args;
        args.reserve(call->args.size());
        for (const ExprPtr& arg : call->args) {
            args.push_back(cloneExpr(*arg));
        }
        return std::make_unique<CallExpr>(call->callee, std::move(args));
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return std::make_unique<UnaryExpr>(unary->op, cloneExpr(*unary->operand));
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return std::make_unique<BinaryExpr>(binary->op, cloneExpr(*binary->lhs), cloneExpr(*binary->rhs));
    }
    return intExpr(0);
}

DeclPtr cloneDecl(const Decl& decl)
{
    return std::make_unique<VarDecl>(decl.isConst, decl.name, cloneExpr(*decl.init));
}

bool isPure(const Expr& expr)
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

bool isNoOpStmt(const Stmt& stmt)
{
    return dynamic_cast<const EmptyStmt*>(&stmt) != nullptr;
}

bool sameExpr(const Expr& lhs, const Expr& rhs)
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

std::optional<std::int32_t> asInt(const Expr& expr)
{
    if (const auto* intValue = dynamic_cast<const IntExpr*>(&expr)) {
        return intValue->value;
    }
    return std::nullopt;
}

std::optional<std::int32_t> evalConst(const Expr& expr)
{
    if (const auto* intValue = dynamic_cast<const IntExpr*>(&expr)) {
        return intValue->value;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        const auto operand = evalConst(*unary->operand);
        if (!operand.has_value()) {
            return std::nullopt;
        }
        switch (unary->op) {
        case UnaryOp::Plus:
            return *operand;
        case UnaryOp::Minus:
            return -*operand;
        case UnaryOp::Not:
            return *operand == 0 ? 1 : 0;
        }
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        const auto lhs = evalConst(*binary->lhs);
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        if (binary->op == BinaryOp::LogicalAnd && *lhs == 0) {
            return 0;
        }
        if (binary->op == BinaryOp::LogicalOr && *lhs != 0) {
            return 1;
        }
        const auto rhs = evalConst(*binary->rhs);
        if (!rhs.has_value()) {
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
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs / *rhs};
        case BinaryOp::Mod:
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs % *rhs};
        }
    }
    return std::nullopt;
}

class GlobalAssignmentCollector {
public:
    explicit GlobalAssignmentCollector(std::unordered_set<std::string> globals)
        : globals_(std::move(globals))
    {
    }

    std::unordered_set<std::string> collect(const Program& program)
    {
        for (const auto& item : program.items) {
            if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                scopes_.clear();
                scopes_.emplace_back();
                for (const Param& param : funcItem->func->params) {
                    scopes_.back().insert(param.name);
                }
                collectBlock(*funcItem->func->body, true);
            }
        }
        return std::move(assigned_);
    }

private:
    void collectStmt(const Stmt& stmt)
    {
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            if (!isLocal(assign->name) && globals_.contains(assign->name)) {
                assigned_.insert(assign->name);
            }
            collectExpr(*assign->value);
            return;
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            collectExpr(*declStmt->decl->init);
            scopes_.back().insert(declStmt->decl->name);
            return;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            collectExpr(*exprStmt->expr);
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            collectBlock(*block, true);
            return;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            collectExpr(*ifStmt->cond);
            auto saved = scopes_;
            collectStmt(*ifStmt->thenBranch);
            scopes_ = saved;
            if (ifStmt->elseBranch) {
                collectStmt(*ifStmt->elseBranch);
                scopes_ = std::move(saved);
            }
            return;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            collectExpr(*whileStmt->cond);
            auto saved = scopes_;
            collectStmt(*whileStmt->body);
            scopes_ = std::move(saved);
            return;
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (ret->value) {
                collectExpr(*ret->value);
            }
        }
    }

    void collectBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            scopes_.emplace_back();
        }
        for (const StmtPtr& stmt : block.statements) {
            collectStmt(*stmt);
        }
        if (createsScope) {
            scopes_.pop_back();
        }
    }

    void collectExpr(const Expr& expr)
    {
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            for (const ExprPtr& arg : call->args) {
                collectExpr(*arg);
            }
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            collectExpr(*unary->operand);
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            collectExpr(*binary->lhs);
            collectExpr(*binary->rhs);
        }
    }

    bool isLocal(const std::string& name) const
    {
        for (auto iter = scopes_.rbegin(); iter != scopes_.rend(); ++iter) {
            if (iter->contains(name)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_set<std::string> globals_;
    std::unordered_set<std::string> assigned_;
    std::vector<std::unordered_set<std::string>> scopes_;
};

class AstOptimizer {
public:
    Program optimize(const Program& program)
    {
        collectGlobals(program);

        Program result;
        result.items.reserve(program.items.size());
        for (const auto& item : program.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                result.items.push_back(std::make_unique<TopDecl>(optimizeGlobalDecl(*declItem->decl)));
            } else if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                result.items.push_back(std::make_unique<TopFunc>(optimizeFunction(*funcItem->func)));
            }
        }
        return result;
    }

private:
    void collectGlobals(const Program& program)
    {
        std::unordered_set<std::string> globalNames;
        for (const auto& item : program.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                globalNames.insert(declItem->decl->name);
            }
        }

        std::unordered_map<std::string, std::int32_t> foldedGlobals;
        for (const auto& item : program.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                auto init = foldGlobalConst(*declItem->decl->init, foldedGlobals);
                if (!init.has_value()) {
                    init = 0;
                }
                globalInitializers_[declItem->decl->name] = *init;
                if (declItem->decl->isConst) {
                    foldedGlobals[declItem->decl->name] = *init;
                }
            }
        }

        GlobalAssignmentCollector collector(std::move(globalNames));
        const auto assigned = collector.collect(program);
        for (const auto& item : program.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                if (declItem->decl->isConst || !assigned.contains(declItem->decl->name)) {
                    globalValues_[declItem->decl->name] = globalInitializers_[declItem->decl->name];
                }
            }
        }
    }

    std::optional<std::int32_t> foldGlobalConst(
        const Expr& expr,
        const std::unordered_map<std::string, std::int32_t>& foldedGlobals) const
    {
        if (const auto* intValue = dynamic_cast<const IntExpr*>(&expr)) {
            return intValue->value;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const auto found = foldedGlobals.find(name->name);
            if (found != foldedGlobals.end()) {
                return found->second;
            }
            return std::nullopt;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const auto operand = foldGlobalConst(*unary->operand, foldedGlobals);
            if (!operand.has_value()) {
                return std::nullopt;
            }
            switch (unary->op) {
            case UnaryOp::Plus:
                return *operand;
            case UnaryOp::Minus:
                return -*operand;
            case UnaryOp::Not:
                return *operand == 0 ? 1 : 0;
            }
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            const auto lhs = foldGlobalConst(*binary->lhs, foldedGlobals);
            const auto rhs = foldGlobalConst(*binary->rhs, foldedGlobals);
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
                return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs / *rhs};
            case BinaryOp::Mod:
                return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs % *rhs};
            }
        }
        return std::nullopt;
    }

    DeclPtr optimizeGlobalDecl(const Decl& decl)
    {
        const auto found = globalInitializers_.find(decl.name);
        if (found != globalInitializers_.end()) {
            return std::make_unique<VarDecl>(decl.isConst, decl.name, intExpr(found->second));
        }
        return cloneDecl(decl);
    }

    FuncPtr optimizeFunction(const FuncDef& func)
    {
        scopes_.clear();
        nextBindingId_ = 0;
        enterScope();
        for (const Param& param : func.params) {
            declare(param.name, false, {});
        }
        auto body = optimizeBlock(*func.body, true);
        leaveScope();

        std::vector<Param> params;
        params.reserve(func.params.size());
        for (const Param& param : func.params) {
            params.emplace_back(param.name);
        }
        return std::make_unique<FuncDef>(func.returnType, func.name, std::move(params), std::move(body));
    }

    BlockPtr optimizeBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            enterScope();
        }

        std::vector<StmtPtr> statements;
        statements.reserve(block.statements.size());
        for (const StmtPtr& stmt : block.statements) {
            StmtPtr optimized = optimizeStmt(*stmt);
            if (!isNoOpStmt(*optimized)) {
                const bool terminal = isTerminal(*optimized);
                statements.push_back(std::move(optimized));
                if (terminal) {
                    break;
                }
            }
        }

        if (createsScope) {
            leaveScope();
        }
        return std::make_unique<BlockStmt>(std::move(statements));
    }

    StmtPtr optimizeStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return std::make_unique<EmptyStmt>();
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            ExprPtr expr = optimizeExpr(*exprStmt->expr);
            if (isPure(*expr)) {
                return std::make_unique<EmptyStmt>();
            }
            return std::make_unique<ExprStmt>(std::move(expr));
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            ExprPtr value = optimizeExpr(*assign->value);
            const KnownValue known = knownFromExpr(*value);
            if (Binding* binding = lookupLocal(assign->name)) {
                invalidateCopiesOf(binding->id);
                binding->known = known;
            }
            return std::make_unique<AssignStmt>(assign->name, std::move(value));
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            ExprPtr init = optimizeExpr(*declStmt->decl->init);
            const KnownValue known = knownFromExpr(*init);
            declare(declStmt->decl->name, declStmt->decl->isConst, known);
            return std::make_unique<DeclStmt>(std::make_unique<VarDecl>(declStmt->decl->isConst, declStmt->decl->name, std::move(init)));
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            return optimizeBlock(*block, true);
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            return optimizeIf(*ifStmt);
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            return optimizeWhile(*whileStmt);
        }
        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            return std::make_unique<BreakStmt>();
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            return std::make_unique<ContinueStmt>();
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            ExprPtr value;
            if (ret->value) {
                value = optimizeExpr(*ret->value);
            }
            return std::make_unique<ReturnStmt>(std::move(value));
        }
        return std::make_unique<EmptyStmt>();
    }

    StmtPtr optimizeIf(const IfStmt& stmt)
    {
        ExprPtr cond = optimizeExpr(*stmt.cond);
        if (const auto value = asInt(*cond)) {
            if (*value != 0) {
                return optimizeStmt(*stmt.thenBranch);
            }
            if (stmt.elseBranch) {
                return optimizeStmt(*stmt.elseBranch);
            }
            return std::make_unique<EmptyStmt>();
        }

        auto saved = scopes_;
        StmtPtr thenBranch = optimizeStmt(*stmt.thenBranch);
        scopes_ = saved;
        StmtPtr elseBranch;
        if (stmt.elseBranch) {
            elseBranch = optimizeStmt(*stmt.elseBranch);
        }
        scopes_ = std::move(saved);
        clearMutableKnowledge();
        return std::make_unique<IfStmt>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
    }

    StmtPtr optimizeWhile(const WhileStmt& stmt)
    {
        clearMutableKnowledge();
        ExprPtr cond = optimizeExpr(*stmt.cond);
        if (const auto value = asInt(*cond); value.has_value() && *value == 0) {
            return std::make_unique<EmptyStmt>();
        }

        clearMutableKnowledge();
        StmtPtr body = optimizeStmt(*stmt.body);
        clearMutableKnowledge();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
    }

    ExprPtr optimizeExpr(const Expr& expr)
    {
        if (const auto* intValue = dynamic_cast<const IntExpr*>(&expr)) {
            return intExpr(intValue->value);
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            if (const Binding* binding = lookupLocal(name->name)) {
                if (binding->known.constant.has_value()) {
                    return intExpr(*binding->known.constant);
                }
                if (binding->known.hasCopy) {
                    const Binding* source = lookupLocal(binding->known.copyName);
                    if (source != nullptr && source->id == binding->known.copyBinding) {
                        return nameExpr(binding->known.copyName);
                    }
                }
                return nameExpr(name->name);
            }
            const auto global = globalValues_.find(name->name);
            if (global != globalValues_.end()) {
                return intExpr(global->second);
            }
            return nameExpr(name->name);
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            std::vector<ExprPtr> args;
            args.reserve(call->args.size());
            for (const ExprPtr& arg : call->args) {
                args.push_back(optimizeExpr(*arg));
            }
            return std::make_unique<CallExpr>(call->callee, std::move(args));
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            ExprPtr operand = optimizeExpr(*unary->operand);
            if (const auto value = evalConst(*operand)) {
                switch (unary->op) {
                case UnaryOp::Plus:
                    return intExpr(*value);
                case UnaryOp::Minus:
                    return intExpr(-*value);
                case UnaryOp::Not:
                    return intExpr(*value == 0 ? 1 : 0);
                }
            }
            if (unary->op == UnaryOp::Plus) {
                return operand;
            }
            return std::make_unique<UnaryExpr>(unary->op, std::move(operand));
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            return optimizeBinary(*binary);
        }
        return intExpr(0);
    }

    ExprPtr optimizeBinary(const BinaryExpr& binary)
    {
        ExprPtr lhs = optimizeExpr(*binary.lhs);

        if (binary.op == BinaryOp::LogicalAnd) {
            if (const auto lhsValue = asInt(*lhs); lhsValue.has_value() && *lhsValue == 0) {
                return intExpr(0);
            }
        }
        if (binary.op == BinaryOp::LogicalOr) {
            if (const auto lhsValue = asInt(*lhs); lhsValue.has_value() && *lhsValue != 0) {
                return intExpr(1);
            }
        }

        ExprPtr rhs = optimizeExpr(*binary.rhs);
        if (const auto folded = evalConst(BinaryExpr(binary.op, cloneExpr(*lhs), cloneExpr(*rhs)))) {
            return intExpr(*folded);
        }

        ExprPtr simplified = simplifyBinary(binary.op, std::move(lhs), std::move(rhs));
        return simplified;
    }

    ExprPtr simplifyBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs)
    {
        const auto lhsInt = asInt(*lhs);
        const auto rhsInt = asInt(*rhs);

        switch (op) {
        case BinaryOp::Add:
            if (rhsInt.has_value() && *rhsInt == 0) {
                return lhs;
            }
            if (lhsInt.has_value() && *lhsInt == 0) {
                return rhs;
            }
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return std::make_unique<BinaryExpr>(BinaryOp::Mul, std::move(lhs), intExpr(2));
            }
            break;
        case BinaryOp::Sub:
            if (rhsInt.has_value() && *rhsInt == 0) {
                return lhs;
            }
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(0);
            }
            break;
        case BinaryOp::Mul:
            if (rhsInt.has_value() && *rhsInt == 1) {
                return lhs;
            }
            if (lhsInt.has_value() && *lhsInt == 1) {
                return rhs;
            }
            if (rhsInt.has_value() && *rhsInt == -1) {
                return std::make_unique<UnaryExpr>(UnaryOp::Minus, std::move(lhs));
            }
            if (lhsInt.has_value() && *lhsInt == -1) {
                return std::make_unique<UnaryExpr>(UnaryOp::Minus, std::move(rhs));
            }
            if (rhsInt.has_value() && *rhsInt == 0 && isPure(*lhs)) {
                return intExpr(0);
            }
            if (lhsInt.has_value() && *lhsInt == 0 && isPure(*rhs)) {
                return intExpr(0);
            }
            break;
        case BinaryOp::Div:
            if (rhsInt.has_value() && *rhsInt == 1) {
                return lhs;
            }
            if (rhsInt.has_value() && *rhsInt == -1) {
                return std::make_unique<UnaryExpr>(UnaryOp::Minus, std::move(lhs));
            }
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(1);
            }
            break;
        case BinaryOp::Mod:
            if (rhsInt.has_value() && *rhsInt == 1 && isPure(*lhs)) {
                return intExpr(0);
            }
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(0);
            }
            break;
        case BinaryOp::Equal:
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(1);
            }
            break;
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::Greater:
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(0);
            }
            break;
        case BinaryOp::LessEqual:
        case BinaryOp::GreaterEqual:
            if (isPure(*lhs) && sameExpr(*lhs, *rhs)) {
                return intExpr(1);
            }
            break;
        case BinaryOp::LogicalAnd:
            if (lhsInt.has_value() && *lhsInt == 0) {
                return intExpr(0);
            }
            if (rhsInt.has_value() && *rhsInt == 0 && isPure(*lhs)) {
                return intExpr(0);
            }
            break;
        case BinaryOp::LogicalOr:
            if (lhsInt.has_value() && *lhsInt != 0) {
                return intExpr(1);
            }
            if (rhsInt.has_value() && *rhsInt != 0 && isPure(*lhs)) {
                return intExpr(1);
            }
            break;
        }

        return std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
    }

    KnownValue knownFromExpr(const Expr& expr) const
    {
        KnownValue known;
        if (const auto value = asInt(expr)) {
            known.constant = *value;
            return known;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            if (const Binding* source = lookupLocal(name->name)) {
                known.hasCopy = true;
                known.copyName = name->name;
                known.copyBinding = source->id;
            }
        }
        return known;
    }

    bool isTerminal(const Stmt& stmt) const
    {
        if (dynamic_cast<const ReturnStmt*>(&stmt) != nullptr
            || dynamic_cast<const BreakStmt*>(&stmt) != nullptr
            || dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            return true;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            for (const StmtPtr& child : block->statements) {
                if (isTerminal(*child)) {
                    return true;
                }
            }
            return false;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            return ifStmt->elseBranch != nullptr && isTerminal(*ifStmt->thenBranch) && isTerminal(*ifStmt->elseBranch);
        }
        return false;
    }

    void enterScope()
    {
        scopes_.push_back({});
    }

    void leaveScope()
    {
        scopes_.pop_back();
    }

    Binding* lookupLocal(const std::string& name)
    {
        for (auto iter = scopes_.rbegin(); iter != scopes_.rend(); ++iter) {
            const auto found = iter->find(name);
            if (found != iter->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    const Binding* lookupLocal(const std::string& name) const
    {
        for (auto iter = scopes_.rbegin(); iter != scopes_.rend(); ++iter) {
            const auto found = iter->find(name);
            if (found != iter->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    void declare(const std::string& name, bool isConst, KnownValue known)
    {
        Binding binding;
        binding.id = nextBindingId_++;
        binding.isConst = isConst;
        binding.known = std::move(known);
        scopes_.back()[name] = std::move(binding);
    }

    void invalidateCopiesOf(int binding)
    {
        for (auto& scope : scopes_) {
            for (auto& [name, item] : scope) {
                (void)name;
                if (item.known.hasCopy && item.known.copyBinding == binding) {
                    item.known.hasCopy = false;
                    item.known.copyName.clear();
                    item.known.copyBinding = -1;
                }
            }
        }
    }

    void clearMutableKnowledge()
    {
        for (auto& scope : scopes_) {
            for (auto& [name, binding] : scope) {
                (void)name;
                if (!binding.isConst) {
                    binding.known = {};
                } else if (binding.known.hasCopy) {
                    const Binding* source = lookupLocal(binding.known.copyName);
                    if (source == nullptr || source->id != binding.known.copyBinding) {
                        binding.known.hasCopy = false;
                        binding.known.copyName.clear();
                        binding.known.copyBinding = -1;
                    }
                }
            }
        }
    }

    std::unordered_map<std::string, std::int32_t> globalInitializers_;
    std::unordered_map<std::string, std::int32_t> globalValues_;
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    int nextBindingId_ = 0;
};

} // namespace

Program optimizeAst(const Program& program)
{
    AstOptimizer optimizer;
    return optimizer.optimize(program);
}

} // namespace toyc
