#include "builder.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

ir::Type toIrType(Type type)
{
    return type == Type::Void ? ir::Type::Void : ir::Type::Int;
}

ir::BinaryOpcode toIrBinaryOpcode(BinaryOp op)
{
    switch (op) {
    case BinaryOp::LogicalOr:
        return ir::BinaryOpcode::LogicalOr;
    case BinaryOp::LogicalAnd:
        return ir::BinaryOpcode::LogicalAnd;
    case BinaryOp::Equal:
        return ir::BinaryOpcode::Equal;
    case BinaryOp::NotEqual:
        return ir::BinaryOpcode::NotEqual;
    case BinaryOp::Less:
        return ir::BinaryOpcode::Less;
    case BinaryOp::LessEqual:
        return ir::BinaryOpcode::LessEqual;
    case BinaryOp::Greater:
        return ir::BinaryOpcode::Greater;
    case BinaryOp::GreaterEqual:
        return ir::BinaryOpcode::GreaterEqual;
    case BinaryOp::Add:
        return ir::BinaryOpcode::Add;
    case BinaryOp::Sub:
        return ir::BinaryOpcode::Sub;
    case BinaryOp::Mul:
        return ir::BinaryOpcode::Mul;
    case BinaryOp::Div:
        return ir::BinaryOpcode::Div;
    case BinaryOp::Mod:
        return ir::BinaryOpcode::Mod;
    }
    throw std::runtime_error("unknown binary opcode");
}

ir::UnaryOpcode toIrUnaryOpcode(UnaryOp op)
{
    switch (op) {
    case UnaryOp::Plus:
        return ir::UnaryOpcode::Plus;
    case UnaryOp::Minus:
        return ir::UnaryOpcode::Minus;
    case UnaryOp::Not:
        return ir::UnaryOpcode::Not;
    }
    throw std::runtime_error("unknown unary opcode");
}

bool isLogical(BinaryOp op)
{
    return op == BinaryOp::LogicalAnd || op == BinaryOp::LogicalOr;
}

class FunctionLowerer {
public:
    FunctionLowerer(
        const FuncDef& func,
        const std::unordered_map<std::string, std::int32_t>& constants,
        const std::unordered_map<std::string, bool>& globals)
        : func_(func), constants_(constants), globals_(globals)
    {
    }

    ir::Function lower()
    {
        function_.returnType = toIrType(func_.returnType);
        function_.name = func_.name;

        enterScope();
        for (const Param& param : func_.params) {
            const std::string symbol = declareLocal(param.name);
            function_.params.push_back(symbol);
        }

        currentBlock_ = newBlock(".entry");
        lowerBlock(*func_.body);

        if (isCurrentBlockOpen()) {
            ir::Terminator terminator;
            terminator.kind = ir::TerminatorKind::Return;
            if (func_.returnType == Type::Int) {
                terminator.returnValue = ir::Operand::imm(0);
                terminator.hasReturnValue = true;
            }
            emitTerminator(terminator);
        }

        leaveScope();
        return std::move(function_);
    }

private:
    void enterScope()
    {
        scopes_.push_back({});
    }

    void leaveScope()
    {
        scopes_.pop_back();
    }

    std::string declareLocal(const std::string& name)
    {
        if (scopes_.empty()) {
            enterScope();
        }
        const std::string symbol = name + "#" + std::to_string(nextLocalId_++);
        scopes_.back()[name] = symbol;
        return symbol;
    }

    std::optional<std::string> lookupLocal(const std::string& name) const
    {
        for (auto iter = scopes_.rbegin(); iter != scopes_.rend(); ++iter) {
            const auto found = iter->find(name);
            if (found != iter->end()) {
                return found->second;
            }
        }
        return std::nullopt;
    }

    ir::Value newValue()
    {
        return ir::Value{function_.nextValue++};
    }

    int newBlock(const std::string& hint)
    {
        ir::BasicBlock block;
        block.label = hint + std::to_string(function_.blocks.size());
        function_.blocks.push_back(std::move(block));
        return static_cast<int>(function_.blocks.size() - 1);
    }

    ir::BasicBlock& currentBlock()
    {
        return function_.blocks.at(static_cast<std::size_t>(currentBlock_));
    }

    bool isCurrentBlockOpen() const
    {
        return currentBlock_ >= 0
            && currentBlock_ < static_cast<int>(function_.blocks.size())
            && !function_.blocks[static_cast<std::size_t>(currentBlock_)].hasTerminator;
    }

    void setCurrentBlock(int block)
    {
        currentBlock_ = block;
    }

    ir::Operand emitValue(ir::Instruction instruction)
    {
        instruction.dst = newValue();
        const ir::Value dst = instruction.dst;
        currentBlock().instructions.push_back(std::move(instruction));
        return ir::Operand::ref(dst);
    }

    void emit(ir::Instruction instruction)
    {
        currentBlock().instructions.push_back(std::move(instruction));
    }

    void emitTerminator(ir::Terminator terminator)
    {
        ir::BasicBlock& block = currentBlock();
        block.terminator = terminator;
        block.hasTerminator = true;
    }

    void emitJumpIfOpen(int target)
    {
        if (!isCurrentBlockOpen()) {
            return;
        }
        ir::Terminator terminator;
        terminator.kind = ir::TerminatorKind::Jump;
        terminator.trueBlock = target;
        emitTerminator(terminator);
    }

    void lowerBlock(const BlockStmt& block)
    {
        enterScope();
        for (const StmtPtr& stmt : block.statements) {
            if (!isCurrentBlockOpen()) {
                continue;
            }
            lowerStmt(*stmt);
        }
        leaveScope();
    }

    void lowerStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return;
        }

        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            lowerExpr(*exprStmt->expr);
            return;
        }

        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            const ir::Operand value = lowerExpr(*assign->value);
            ir::Instruction instruction;
            instruction.operands.push_back(value);
            if (const auto local = lookupLocal(assign->name); local.has_value()) {
                instruction.kind = ir::InstructionKind::StoreLocal;
                instruction.symbol = *local;
            } else {
                instruction.kind = ir::InstructionKind::StoreGlobal;
                instruction.symbol = assign->name;
                instruction.hasSideEffect = true;
            }
            emit(std::move(instruction));
            return;
        }

        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            const ir::Operand init = lowerExpr(*declStmt->decl->init);
            const std::string symbol = declareLocal(declStmt->decl->name);
            ir::Instruction instruction;
            instruction.kind = ir::InstructionKind::StoreLocal;
            instruction.symbol = symbol;
            instruction.operands.push_back(init);
            emit(std::move(instruction));
            return;
        }

        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            lowerBlock(*block);
            return;
        }

        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            lowerIf(*ifStmt);
            return;
        }

        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            lowerWhile(*whileStmt);
            return;
        }

        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            if (breakTargets_.empty()) {
                throw std::runtime_error("break outside loop in IR builder");
            }
            ir::Terminator terminator;
            terminator.kind = ir::TerminatorKind::Jump;
            terminator.trueBlock = breakTargets_.back();
            emitTerminator(terminator);
            return;
        }

        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            if (continueTargets_.empty()) {
                throw std::runtime_error("continue outside loop in IR builder");
            }
            ir::Terminator terminator;
            terminator.kind = ir::TerminatorKind::Jump;
            terminator.trueBlock = continueTargets_.back();
            emitTerminator(terminator);
            return;
        }

        if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
            ir::Terminator terminator;
            terminator.kind = ir::TerminatorKind::Return;
            if (returnStmt->value) {
                terminator.returnValue = lowerExpr(*returnStmt->value);
                terminator.hasReturnValue = true;
            }
            emitTerminator(terminator);
            return;
        }

        throw std::runtime_error("unknown statement in IR builder");
    }

    void lowerIf(const IfStmt& stmt)
    {
        const int thenBlock = newBlock(".if.then");
        const int endBlock = newBlock(".if.end");
        const int elseBlock = stmt.elseBranch ? newBlock(".if.else") : endBlock;

        lowerCondBranch(*stmt.cond, thenBlock, elseBlock);

        setCurrentBlock(thenBlock);
        lowerStmt(*stmt.thenBranch);
        emitJumpIfOpen(endBlock);

        if (stmt.elseBranch) {
            setCurrentBlock(elseBlock);
            lowerStmt(*stmt.elseBranch);
            emitJumpIfOpen(endBlock);
        }

        setCurrentBlock(endBlock);
    }

    void lowerWhile(const WhileStmt& stmt)
    {
        const int condBlock = newBlock(".while.cond");
        const int bodyBlock = newBlock(".while.body");
        const int endBlock = newBlock(".while.end");

        emitJumpIfOpen(condBlock);

        setCurrentBlock(condBlock);
        lowerCondBranch(*stmt.cond, bodyBlock, endBlock);

        breakTargets_.push_back(endBlock);
        continueTargets_.push_back(condBlock);
        setCurrentBlock(bodyBlock);
        lowerStmt(*stmt.body);
        emitJumpIfOpen(condBlock);
        continueTargets_.pop_back();
        breakTargets_.pop_back();

        setCurrentBlock(endBlock);
    }

    ir::Operand lowerExpr(const Expr& expr)
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return ir::Operand::imm(intExpr->value);
        }

        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            if (const auto local = lookupLocal(name->name); local.has_value()) {
                ir::Instruction instruction;
                instruction.kind = ir::InstructionKind::LoadLocal;
                instruction.symbol = *local;
                return emitValue(std::move(instruction));
            }
            const auto constant = constants_.find(name->name);
            if (constant != constants_.end()) {
                return ir::Operand::imm(constant->second);
            }
            ir::Instruction instruction;
            instruction.kind = ir::InstructionKind::LoadGlobal;
            instruction.symbol = name->name;
            return emitValue(std::move(instruction));
        }

        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            ir::Instruction instruction;
            instruction.kind = ir::InstructionKind::Call;
            instruction.symbol = call->callee;
            instruction.hasSideEffect = true;
            instruction.operands.reserve(call->args.size());
            for (const ExprPtr& arg : call->args) {
                instruction.operands.push_back(lowerExpr(*arg));
            }
            return emitValue(std::move(instruction));
        }

        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            ir::Instruction instruction;
            instruction.kind = ir::InstructionKind::Unary;
            instruction.unaryOp = toIrUnaryOpcode(unary->op);
            instruction.operands.push_back(lowerExpr(*unary->operand));
            return emitValue(std::move(instruction));
        }

        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            if (isLogical(binary->op)) {
                return lowerLogicalValue(*binary);
            }

            ir::Instruction instruction;
            instruction.kind = ir::InstructionKind::Binary;
            instruction.binaryOp = toIrBinaryOpcode(binary->op);
            instruction.operands.push_back(lowerExpr(*binary->lhs));
            instruction.operands.push_back(lowerExpr(*binary->rhs));
            return emitValue(std::move(instruction));
        }

        throw std::runtime_error("unknown expression in IR builder");
    }

    ir::Operand lowerLogicalValue(const Expr& expr)
    {
        const std::string temp = "__logic" + std::to_string(nextTempId_++);
        const int trueBlock = newBlock(".logic.true");
        const int falseBlock = newBlock(".logic.false");
        const int endBlock = newBlock(".logic.end");

        lowerCondBranch(expr, trueBlock, falseBlock);

        setCurrentBlock(trueBlock);
        emitStoreLocal(temp, ir::Operand::imm(1));
        emitJumpIfOpen(endBlock);

        setCurrentBlock(falseBlock);
        emitStoreLocal(temp, ir::Operand::imm(0));
        emitJumpIfOpen(endBlock);

        setCurrentBlock(endBlock);
        ir::Instruction load;
        load.kind = ir::InstructionKind::LoadLocal;
        load.symbol = temp;
        return emitValue(std::move(load));
    }

    void lowerCondBranch(const Expr& expr, int trueBlock, int falseBlock)
    {
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            if (unary->op == UnaryOp::Not) {
                lowerCondBranch(*unary->operand, falseBlock, trueBlock);
                return;
            }
        }

        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            if (binary->op == BinaryOp::LogicalAnd) {
                const int rhsBlock = newBlock(".land.rhs");
                lowerCondBranch(*binary->lhs, rhsBlock, falseBlock);
                setCurrentBlock(rhsBlock);
                lowerCondBranch(*binary->rhs, trueBlock, falseBlock);
                return;
            }
            if (binary->op == BinaryOp::LogicalOr) {
                const int rhsBlock = newBlock(".lor.rhs");
                lowerCondBranch(*binary->lhs, trueBlock, rhsBlock);
                setCurrentBlock(rhsBlock);
                lowerCondBranch(*binary->rhs, trueBlock, falseBlock);
                return;
            }
        }

        ir::Terminator terminator;
        terminator.kind = ir::TerminatorKind::Branch;
        terminator.condition = lowerExpr(expr);
        terminator.trueBlock = trueBlock;
        terminator.falseBlock = falseBlock;
        emitTerminator(terminator);
    }

    void emitStoreLocal(const std::string& symbol, ir::Operand value)
    {
        ir::Instruction instruction;
        instruction.kind = ir::InstructionKind::StoreLocal;
        instruction.symbol = symbol;
        instruction.operands.push_back(value);
        emit(std::move(instruction));
    }

    const FuncDef& func_;
    const std::unordered_map<std::string, std::int32_t>& constants_;
    const std::unordered_map<std::string, bool>& globals_;
    ir::Function function_;
    int currentBlock_ = -1;
    int nextLocalId_ = 0;
    int nextTempId_ = 0;
    std::vector<std::unordered_map<std::string, std::string>> scopes_;
    std::vector<int> breakTargets_;
    std::vector<int> continueTargets_;
};

} // namespace

ir::Module IrBuilder::build(const Program& program)
{
    constants_.clear();
    std::unordered_map<std::string, bool> globals;
    ir::Module module;

    for (const auto& item : program.items) {
        if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
            const auto init = evalConst(*declItem->decl->init);
            if (!init.has_value()) {
                throw std::runtime_error("IR builder expected constant global initializer");
            }
            module.globals.push_back(ir::Global{declItem->decl->isConst, declItem->decl->name, *init});
            globals[declItem->decl->name] = declItem->decl->isConst;
            if (declItem->decl->isConst) {
                constants_[declItem->decl->name] = *init;
            }
        }
    }

    for (const auto& item : program.items) {
        if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
            FunctionLowerer lowerer(*funcItem->func, constants_, globals);
            module.functions.push_back(lowerer.lower());
        }
    }

    return module;
}

std::optional<std::int32_t> IrBuilder::evalConst(const Expr& expr) const
{
    if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
        return intExpr->value;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
        const auto found = constants_.find(name->name);
        if (found != constants_.end()) {
            return found->second;
        }
        return std::nullopt;
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
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs / *rhs};
        case BinaryOp::Mod:
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs % *rhs};
        }
    }
    return std::nullopt;
}

ir::Type IrBuilder::mapType(Type type) const
{
    return toIrType(type);
}

} // namespace toyc
