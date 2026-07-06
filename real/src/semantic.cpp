#include "semantic.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace toyc {
namespace {

struct FunctionInfo {
    Type returnType;
    int paramCount;
    int order;
};

struct SymbolInfo {
    bool isConst = false;
    bool hasConstValue = false;
    std::int32_t constValue = 0;
    Type type = Type::Int;
};

class Analyzer {
public:
    void analyze(const Program& program)
    {
        int order = 0;
        for (const auto& item : program.items) {
            if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                const auto& func = *funcItem->func;
                if (functions_.contains(func.name)) {
                    fail("function redefined: " + func.name);
                }
                functions_.emplace(func.name, FunctionInfo{func.returnType, static_cast<int>(func.params.size()), order});
            }
            ++order;
        }

        const auto mainIt = functions_.find("main");
        if (mainIt == functions_.end() || mainIt->second.returnType != Type::Int || mainIt->second.paramCount != 0) {
            fail("program must define int main()");
        }

        pushScope();
        order = 0;
        for (const auto& item : program.items) {
            currentOrder_ = order;
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                analyzeDecl(*declItem->decl, true);
            } else if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                analyzeFunc(*funcItem->func);
            }
            ++order;
        }
        popScope();
    }

private:
    void fail(const std::string& message) const
    {
        throw SemanticError(message);
    }

    void pushScope()
    {
        scopes_.emplace_back();
    }

    void popScope()
    {
        scopes_.pop_back();
    }

    void declare(const std::string& name, const SymbolInfo& info)
    {
        auto& scope = scopes_.back();
        if (scope.contains(name)) {
            fail("symbol redefined in the same scope: " + name);
        }
        scope.emplace(name, info);
    }

    const SymbolInfo* lookup(const std::string& name) const
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    void analyzeFunc(const FuncDef& func)
    {
        currentFunction_ = &func;
        loopDepth_ = 0;
        pushScope();
        for (const auto& param : func.params) {
            declare(param.name, SymbolInfo{false, false, 0, Type::Int});
        }
        const bool returns = analyzeBlock(*func.body, false);
        if (func.returnType == Type::Int && !returns) {
            fail("int function may exit without returning a value: " + func.name);
        }
        popScope();
        currentFunction_ = nullptr;
    }

    bool analyzeBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            pushScope();
        }

        bool guaranteedReturn = false;
        for (const auto& stmt : block.statements) {
            if (!guaranteedReturn && analyzeStmt(*stmt)) {
                guaranteedReturn = true;
            }
        }

        if (createsScope) {
            popScope();
        }
        return guaranteedReturn;
    }

    bool analyzeStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return false;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            analyzeExpr(*exprStmt->expr, false);
            return false;
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            const auto* symbol = lookup(assign->name);
            if (symbol == nullptr) {
                fail("assignment to undeclared symbol: " + assign->name);
            }
            if (symbol->isConst) {
                fail("assignment to const symbol: " + assign->name);
            }
            const Type rhsType = analyzeExpr(*assign->value, true);
            if (rhsType != Type::Int) {
                fail("assignment right side must be int: " + assign->name);
            }
            return false;
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            analyzeDecl(*declStmt->decl, false);
            return false;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            return analyzeBlock(*block, true);
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            requireInt(*ifStmt->cond, "if condition must be int");
            const bool thenReturns = analyzeStmt(*ifStmt->thenBranch);
            const bool elseReturns = ifStmt->elseBranch != nullptr && analyzeStmt(*ifStmt->elseBranch);
            return ifStmt->elseBranch != nullptr && thenReturns && elseReturns;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            requireInt(*whileStmt->cond, "while condition must be int");
            const auto constCond = evalConst(*whileStmt->cond);
            ++loopDepth_;
            const bool bodyReturns = analyzeStmt(*whileStmt->body);
            --loopDepth_;
            return constCond.has_value() && *constCond != 0 && bodyReturns;
        }
        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            if (loopDepth_ == 0) {
                fail("break outside loop");
            }
            return false;
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            if (loopDepth_ == 0) {
                fail("continue outside loop");
            }
            return false;
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (currentFunction_ == nullptr) {
                fail("return outside function");
            }
            if (currentFunction_->returnType == Type::Void) {
                if (ret->value != nullptr) {
                    fail("void function cannot return a value: " + currentFunction_->name);
                }
            } else {
                if (ret->value == nullptr) {
                    fail("int function must return a value: " + currentFunction_->name);
                }
                requireInt(*ret->value, "return value must be int");
            }
            return true;
        }
        fail("unknown statement");
        return false;
    }

    void analyzeDecl(const Decl& decl, bool global)
    {
        if (global && functions_.contains(decl.name)) {
            fail("global symbol conflicts with function name: " + decl.name);
        }
        SymbolInfo info;
        info.isConst = decl.isConst;
        info.type = Type::Int;
        if (decl.isConst) {
            const auto value = evalConst(*decl.init);
            if (!value.has_value()) {
                fail("const initializer is not a compile-time constant: " + decl.name);
            }
            info.hasConstValue = true;
            info.constValue = *value;
        } else {
            if (global) {
                const auto value = evalConst(*decl.init);
                if (!value.has_value()) {
                    fail("global variable initializer is not a compile-time constant: " + decl.name);
                }
                info.hasConstValue = true;
                info.constValue = *value;
            } else {
                requireInt(*decl.init, "variable initializer must be int");
            }
        }
        declare(decl.name, info);
    }

    void requireInt(const Expr& expr, const std::string& message)
    {
        if (analyzeExpr(expr, true) != Type::Int) {
            fail(message);
        }
    }

    Type analyzeExpr(const Expr& expr, bool requireValue)
    {
        if (dynamic_cast<const IntExpr*>(&expr) != nullptr) {
            return Type::Int;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            if (lookup(name->name) == nullptr) {
                fail("use of undeclared symbol: " + name->name);
            }
            return Type::Int;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            const auto found = functions_.find(call->callee);
            if (found == functions_.end()) {
                fail("call to function before definition: " + call->callee);
            }
            if (found->second.order > currentOrder_ && (currentFunction_ == nullptr || call->callee != currentFunction_->name)) {
                fail("call to function before definition: " + call->callee);
            }
            if (found->second.paramCount != static_cast<int>(call->args.size())) {
                fail("wrong argument count for function: " + call->callee);
            }
            for (const auto& arg : call->args) {
                requireInt(*arg, "function argument must be int");
            }
            if (requireValue && found->second.returnType == Type::Void) {
                fail("void function call cannot be used as a value: " + call->callee);
            }
            return found->second.returnType;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            requireInt(*unary->operand, "unary operand must be int");
            return Type::Int;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            requireInt(*binary->lhs, "binary left operand must be int");
            requireInt(*binary->rhs, "binary right operand must be int");
            return Type::Int;
        }
        fail("unknown expression");
        return Type::Int;
    }

    std::optional<std::int32_t> evalConst(const Expr& expr)
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return intExpr->value;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const auto* symbol = lookup(name->name);
            if (symbol == nullptr || !symbol->isConst || !symbol->hasConstValue) {
                return std::nullopt;
            }
            return symbol->constValue;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const auto value = evalConst(*unary->operand);
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
            const auto lhs = evalConst(*binary->lhs);
            const auto rhs = evalConst(*binary->rhs);
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
        return std::nullopt;
    }

    std::unordered_map<std::string, FunctionInfo> functions_;
    std::vector<std::unordered_map<std::string, SymbolInfo>> scopes_;
    const FuncDef* currentFunction_ = nullptr;
    int currentOrder_ = 0;
    int loopDepth_ = 0;
};

} // namespace

void SemanticAnalyzer::analyze(const Program& program)
{
    Analyzer analyzer;
    analyzer.analyze(program);
}

} // namespace toyc
