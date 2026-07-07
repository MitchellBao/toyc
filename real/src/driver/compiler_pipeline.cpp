#include "compiler_pipeline.h"

#include "ir/builder.h"
#include "ir/verifier.h"
#include "passes/pass_manager.h"
#include "target/riscv/asm_printer.h"

#include <iostream>

namespace toyc {

CompilerPipeline::CompilerPipeline(CompilerOptions options)
    : options_(options)
{
}

void CompilerPipeline::emitAssembly(const Program& program, std::ostream& out) const
{
    IrBuilder builder;
    ir::Module module = builder.build(program);

    ir::Verifier verifier;
    verifier.verify(module);

    passes::PassManager passes = passes::buildPipeline(options_.optimize, options_.emitStats, &std::cerr);
    passes.run(module);

    verifier.verify(module);

    riscv::AsmPrinter printer;
    printer.print(module, out, options_.emitStats ? &std::cerr : nullptr);
}

} // namespace toyc
