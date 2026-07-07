#pragma once

namespace toyc::riscv {

struct FrameLayout {
    int frameSize = 0;
    int savedRaOffset = 0;
};

int alignTo(int value, int alignment);

} // namespace toyc::riscv
