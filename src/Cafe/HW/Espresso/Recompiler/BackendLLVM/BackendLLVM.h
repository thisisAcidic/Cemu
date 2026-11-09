#pragma once

struct PPCRecFunction_t;
struct ppcImlGenContext_t;

// Optional backend that translates the IML of a function with LLVM. It runs on the IML before register allocation and
// produces a function with the signature of PPCRecompilerLLVM_Function, which the native backend calls through small
// entry thunks. Translations are made before a title starts and kept in a cache per title.
// Enabled by creating llvm.enable in the user data folder.

// Translated functions use the preserve_none calling convention and take hCPU, the memory base, the entry index and the
// depth of direct calls. They return the guest address to continue at, the low two bits say how (see llvmEntryPoint).
using PPCRecompilerLLVM_Function = void*;

// Calls from translated code to other translated functions are host calls. Each call site caches its target here.
struct PPCRecompilerLLVM_CallSite
{
	std::atomic<uint64> key; // guest address in the low half, generation in the high half, written last
	PPCRecompilerLLVM_Function function; // nullptr if the target isn't translated by LLVM
	uint64 entryIndex;
};

// changes whenever a jump table entry stops pointing to the translation a call site may have cached
extern std::atomic<uint32> g_PPCRecompilerLLVM_generation;
// implemented by the recompiler, called from translated code when a call site has no valid target
void PPCRecompilerLLVM_ResolveCall(uint32 target, PPCRecompilerLLVM_CallSite* site);

// counters written to the log every 10 seconds when llvm.stats exists in the user data folder:
// exits of functions entered through an entry point by kind (jump, leave, call, return), entries, call site resolves, resolves that found a translated function
enum
{
	LLVM_STAT_ENTRIES = 4,
	LLVM_STAT_RESOLVES = 5,
	LLVM_STAT_RESOLVED = 6,
	LLVM_STAT_COUNT = 8,
};
extern uint64 g_PPCRecompilerLLVM_stats[LLVM_STAT_COUNT];
bool PPCRecompilerLLVM_IsStatsEnabled();

bool PPCRecompilerLLVM_IsEnabled();

struct PPCRecompilerLLVM_Result
{
	PPCRecompilerLLVM_Function function;
	std::vector<uint32> entryAddresses; // guest address of each entry index
	// generated code, for perf maps and dumps
	const void* code;
	size_t codeSize;
};

// returns the translation from the cache, false if there is none and the native backend has to be used
bool PPCRecompilerLLVM_LoadCached(ppcImlGenContext_t& ppcImlGenContext, PPCRecompilerLLVM_Result& result);
// translates the function into the cache unless it is there already, functions LLVM can't handle are skipped
void PPCRecompilerLLVM_Prepare(ppcImlGenContext_t& ppcImlGenContext, const PPCRecFunction_t& function);
