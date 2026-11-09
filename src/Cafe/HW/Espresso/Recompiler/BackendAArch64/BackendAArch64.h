#pragma once

#include "HW/Espresso/Recompiler/IML/IMLInstruction.h"
#include "../PPCRecompiler.h"

bool PPCRecompiler_generateAArch64Code(struct PPCRecFunction_t* PPCRecFunction, struct ppcImlGenContext_t* ppcImlGenContext);
// emits an entry point for each enterable segment which calls the function produced by the LLVM backend
// stats is optional, it counts entries and the kind of exit
bool PPCRecompiler_generateAArch64LLVMEntryPoints(struct PPCRecFunction_t* PPCRecFunction, struct ppcImlGenContext_t* ppcImlGenContext, void* llvmFunction, uint64* stats);
void PPCRecompiler_cleanupAArch64Code(void* code, size_t size);

void PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();

// architecture specific constants
namespace IMLArchAArch64
{
	static constexpr int PHYSREG_GPR_BASE = 0;
	static constexpr int PHYSREG_GPR_COUNT = 24; // x24 holds the remaining cycles
	static constexpr int PHYSREG_FPR_BASE = PHYSREG_GPR_COUNT;
	static constexpr int PHYSREG_FPR_COUNT = 31;
}; // namespace IMLArchAArch64