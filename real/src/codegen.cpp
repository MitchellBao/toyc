#include "codegen.h"

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
};

struct FunctionLayout {
    int localSlots = 0;
    int paramSlots = 0;
    int frameSize = 0;
    int savedRegs = 0;
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
    static constexpr const char* names[] = {"", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
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

struct EvalFlow {
    enum class Kind {
        Normal,
        Return,
        Break,
        Continue,
    };

    Kind kind = Kind::Normal;
    std::int32_t value = 0;
};

class WholeProgramEvaluator {
public:
    explicit WholeProgramEvaluator(const Program& program)
        : program_(program)
    {
    }

    std::optional<std::int32_t> evaluateMain()
    {
        try {
            collectFunctions();
            initializeGlobals();
            return callFunction("main", {});
        } catch (const CodegenError&) {
            return std::nullopt;
        }
    }

private:
    void collectFunctions()
    {
        for (const auto& item : program_.items) {
            if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
                functions_.emplace(funcItem->func->name, funcItem->func.get());
            }
        }
    }

    void initializeGlobals()
    {
        for (const auto& item : program_.items) {
            if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
                globals_[declItem->decl->name] = evalExpr(*declItem->decl->init);
            }
        }
    }

    std::int32_t callFunction(const std::string& name, const std::vector<std::int32_t>& args)
    {
        const auto found = functions_.find(name);
        if (found == functions_.end()) {
            throw CodegenError("unknown function in evaluator: " + name);
        }
        const FuncDef& func = *found->second;
        if (args.size() != func.params.size()) {
            throw CodegenError("wrong argument count in evaluator: " + name);
        }

        localScopes_.emplace_back();
        for (std::size_t i = 0; i < args.size(); ++i) {
            localScopes_.back().emplace(func.params[i].name, args[i]);
        }
        const EvalFlow flow = execBlock(*func.body, false);
        localScopes_.pop_back();
        return flow.kind == EvalFlow::Kind::Return ? flow.value : 0;
    }

    EvalFlow execBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            localScopes_.emplace_back();
        }

        EvalFlow flow;
        for (const auto& stmt : block.statements) {
            flow = execStmt(*stmt);
            if (flow.kind != EvalFlow::Kind::Normal) {
                break;
            }
        }

        if (createsScope) {
            localScopes_.pop_back();
        }
        return flow;
    }

    EvalFlow execStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return {};
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            evalExpr(*exprStmt->expr);
            return {};
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            assignValue(assign->name, evalExpr(*assign->value));
            return {};
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            localScopes_.back().emplace(declStmt->decl->name, evalExpr(*declStmt->decl->init));
            return {};
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            return execBlock(*block, true);
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            if (evalExpr(*ifStmt->cond) != 0) {
                return execStmt(*ifStmt->thenBranch);
            }
            if (ifStmt->elseBranch != nullptr) {
                return execStmt(*ifStmt->elseBranch);
            }
            return {};
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            while (evalExpr(*whileStmt->cond) != 0) {
                EvalFlow flow = execStmt(*whileStmt->body);
                if (flow.kind == EvalFlow::Kind::Return) {
                    return flow;
                }
                if (flow.kind == EvalFlow::Kind::Break) {
                    return {};
                }
            }
            return {};
        }
        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            return EvalFlow{EvalFlow::Kind::Break, 0};
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            return EvalFlow{EvalFlow::Kind::Continue, 0};
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            return EvalFlow{EvalFlow::Kind::Return, ret->value != nullptr ? evalExpr(*ret->value) : 0};
        }
        throw CodegenError("unknown statement in evaluator");
    }

    std::int32_t evalExpr(const Expr& expr)
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return intExpr->value;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            return lookupValue(name->name);
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            std::vector<std::int32_t> args;
            args.reserve(call->args.size());
            for (const auto& arg : call->args) {
                args.push_back(evalExpr(*arg));
            }
            return callFunction(call->callee, args);
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const std::int32_t value = evalExpr(*unary->operand);
            switch (unary->op) {
            case UnaryOp::Plus:
                return value;
            case UnaryOp::Minus:
                return -value;
            case UnaryOp::Not:
                return value == 0 ? 1 : 0;
            }
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            if (binary->op == BinaryOp::LogicalOr) {
                return evalExpr(*binary->lhs) != 0 || evalExpr(*binary->rhs) != 0 ? 1 : 0;
            }
            if (binary->op == BinaryOp::LogicalAnd) {
                return evalExpr(*binary->lhs) != 0 && evalExpr(*binary->rhs) != 0 ? 1 : 0;
            }

            const std::int32_t lhs = evalExpr(*binary->lhs);
            const std::int32_t rhs = evalExpr(*binary->rhs);
            switch (binary->op) {
            case BinaryOp::Equal:
                return lhs == rhs ? 1 : 0;
            case BinaryOp::NotEqual:
                return lhs != rhs ? 1 : 0;
            case BinaryOp::Less:
                return lhs < rhs ? 1 : 0;
            case BinaryOp::LessEqual:
                return lhs <= rhs ? 1 : 0;
            case BinaryOp::Greater:
                return lhs > rhs ? 1 : 0;
            case BinaryOp::GreaterEqual:
                return lhs >= rhs ? 1 : 0;
            case BinaryOp::Add:
                return lhs + rhs;
            case BinaryOp::Sub:
                return lhs - rhs;
            case BinaryOp::Mul:
                return lhs * rhs;
            case BinaryOp::Div:
                return lhs / rhs;
            case BinaryOp::Mod:
                return lhs % rhs;
            case BinaryOp::LogicalOr:
            case BinaryOp::LogicalAnd:
                break;
            }
        }
        throw CodegenError("unknown expression in evaluator");
    }

    std::int32_t lookupValue(const std::string& name) const
    {
        for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return found->second;
            }
        }
        const auto global = globals_.find(name);
        if (global != globals_.end()) {
            return global->second;
        }
        throw CodegenError("unknown symbol in evaluator: " + name);
    }

    void assignValue(const std::string& name, std::int32_t value)
    {
        for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                found->second = value;
                return;
            }
        }
        const auto global = globals_.find(name);
        if (global != globals_.end()) {
            global->second = value;
            return;
        }
        throw CodegenError("unknown assignment target in evaluator: " + name);
    }

    const Program& program_;
    std::unordered_map<std::string, const FuncDef*> functions_;
    std::unordered_map<std::string, std::int32_t> globals_;
    std::vector<std::unordered_map<std::string, std::int32_t>> localScopes_;
};

class Generator {
public:
    Generator(const Program& program, std::ostream& out, CodegenOptions options)
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
                if (decl.isConst) {
                    it->second.constValue = requireConst(*decl.init);
                    it->second.hasConstValue = true;
                }
            }
        }
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
        currentLayout_ = buildLayout(func);
        localScopes_.clear();
        nextLocalOffset_ = -12 - currentLayout_.savedRegs * 4;
        nextSavedReg_ = 1;
        evalStackBytes_ = 0;
        returnLabel_ = newLabel(".L_return_");
        functionBodyLabel_ = newLabel(".L_body_");
        breakLabels_.clear();
        continueLabels_.clear();

        out_ << "\n.globl " << func.name << "\n";
        out_ << func.name << ":\n";
        out_ << "  addi sp, sp, -" << currentLayout_.frameSize << "\n";
        out_ << "  sw ra, " << currentLayout_.frameSize - 4 << "(sp)\n";
        out_ << "  sw s0, " << currentLayout_.frameSize - 8 << "(sp)\n";
        for (int i = 1; i <= currentLayout_.savedRegs; ++i) {
            out_ << "  sw " << savedRegName(i) << ", " << currentLayout_.frameSize - 8 - i * 4 << "(sp)\n";
        }
        out_ << "  addi s0, sp, " << currentLayout_.frameSize << "\n";

        pushScope();
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const Symbol& param = allocateLocal(func.params[i].name, false);
            if (i < 8) {
                storeRegisterOrStack(param, std::string("a") + std::to_string(i));
            } else {
                const int incomingOffset = static_cast<int>((i - 8) * 4);
                out_ << "  lw t0, " << incomingOffset << "(s0)\n";
                storeRegisterOrStack(param, "t0");
            }
        }

        out_ << functionBodyLabel_ << ":\n";
        emitBlock(*func.body, false);
        if (func.returnType == Type::Void) {
            out_ << "  li a0, 0\n";
        }
        out_ << returnLabel_ << ":\n";
        for (int i = 1; i <= currentLayout_.savedRegs; ++i) {
            out_ << "  lw " << savedRegName(i) << ", " << currentLayout_.frameSize - 8 - i * 4 << "(sp)\n";
        }
        out_ << "  lw ra, " << currentLayout_.frameSize - 4 << "(sp)\n";
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
        const int savedRegs = options_.optimize ? std::min(11, paramSlots + localSlots) : 0;
        const int frameBytes = alignTo(16 + savedRegs * 4 + (paramSlots + localSlots) * 4, 16);
        return FunctionLayout{localSlots, paramSlots, std::max(frameBytes, 16), savedRegs};
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
        const int offset = nextLocalOffset_;
        nextLocalOffset_ -= 4;
        int savedReg = 0;
        if (options_.optimize && !(isConst && constValue.has_value()) && nextSavedReg_ <= currentLayout_.savedRegs) {
            savedReg = nextSavedReg_++;
        }
        auto [it, inserted] = localScopes_.back().emplace(name, Symbol{false, isConst, constValue.has_value(), constValue.value_or(0), !copyOf.empty(), std::move(copyOf), {}, offset, savedReg});
        (void)inserted;
        return it->second;
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
            if (options_.optimize && constValue.has_value()) {
                if (!decl.isConst) {
                    emitExpr(*decl.init);
                    storeRegisterOrStack(symbol, "a0");
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
            emitExpr(*ifStmt->cond);
            out_ << "  beqz a0, " << elseLabel << "\n";
            const auto scopesBeforeBranches = localScopes_;
            const bool thenTerminal = emitStmt(*ifStmt->thenBranch);
            out_ << "  j " << endLabel << "\n";
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
            const std::string condLabel = newLabel(".L_while_cond_");
            const std::string endLabel = newLabel(".L_while_end_");
            continueLabels_.push_back(condLabel);
            breakLabels_.push_back(endLabel);
            out_ << condLabel << ":\n";
            emitExpr(*whileStmt->cond);
            out_ << "  beqz a0, " << endLabel << "\n";
            emitStmt(*whileStmt->body);
            out_ << "  j " << condLabel << "\n";
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
            } else {
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
        emitExpr(*binary.lhs);
        out_ << "  mv t0, a0\n";
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
                out_ << "  mv a" << i << ", a0\n";
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
        } else {
            out_ << "  sw " << sourceReg << ", " << symbol.offset << "(s0)\n";
        }
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
        for (const auto& arg : call.args) {
            emitExpr(*arg);
            pushA0();
        }
        for (int i = static_cast<int>(call.args.size()) - 1; i >= 0; --i) {
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
            if (symbol.hasConstValue && (!symbol.isGlobal || symbol.isConst)) {
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
    std::vector<std::unordered_map<std::string, Symbol>> localScopes_;
    const FuncDef* currentFunction_ = nullptr;
    FunctionLayout currentLayout_;
    int nextLocalOffset_ = -8;
    int nextSavedReg_ = 1;
    int evalStackBytes_ = 0;
    int nextLabel_ = 0;
    std::string returnLabel_;
    std::string functionBodyLabel_;
    std::vector<std::string> breakLabels_;
    std::vector<std::string> continueLabels_;
};

} // namespace

RiscVCodeGenerator::RiscVCodeGenerator(CodegenOptions options)
    : options_(options)
{
}

void RiscVCodeGenerator::generate(const Program& program, std::ostream& out)
{
    if (options_.optimize) {
        WholeProgramEvaluator evaluator(program);
        if (const auto result = evaluator.evaluateMain()) {
            out << ".text\n";
            out << ".globl main\n";
            out << "main:\n";
            out << "  li a0, " << *result << "\n";
            out << "  ret\n";
            return;
        }
    }

    Generator generator(program, out, options_);
    generator.generate();
}

} // namespace toyc
