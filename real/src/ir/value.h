#pragma once

#include <cstdint>

namespace toyc::ir {

enum class Type {
    Int,
    Void,
};

struct Value {
    int id = -1;

    friend bool operator==(Value lhs, Value rhs)
    {
        return lhs.id == rhs.id;
    }
};

struct Operand {
    bool isImmediate = false;
    std::int32_t immediate = 0;
    Value value;

    static Operand imm(std::int32_t value)
    {
        Operand operand;
        operand.isImmediate = true;
        operand.immediate = value;
        return operand;
    }

    static Operand ref(Value value)
    {
        Operand operand;
        operand.value = value;
        return operand;
    }
};

} // namespace toyc::ir
