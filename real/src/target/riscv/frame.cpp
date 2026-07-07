#include "frame.h"

namespace toyc::riscv {

int alignTo(int value, int alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace toyc::riscv
