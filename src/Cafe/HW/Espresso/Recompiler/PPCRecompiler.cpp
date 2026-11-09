#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#ifdef ENABLE_LLVM_RECOMPILER
#include "BackendLLVM/BackendLLVM.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"

#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION	0 // if 1, then function recompilation will block and execute on the thread that called PPCRecompiler_visitAddressNoBlock
#define PPCREC_LOG_RECOMPILATION_RESULTS		0

struct ppcInvalidationRange
{
	MPTR startAddress;
	uint32 size;

	ppcInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {};
};

struct ppcRecompilerFuncRange
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

struct
{
	std::atomic_bool initialized{false};
	FSpinlock recompilerSpinlock;
	std::queue<MPTR> targetQueue;
	std::vector<ppcInvalidationRange> invalidationRanges;
	std::atomic_int_fast32_t recompilerEnableCount{0};
	// recompiler thread
	std::thread workerThread;
	std::atomic_bool workerThreadStopSignal{false};
	// function storage
	RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> functionStorage;
}s_ppcRecompilerState;

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
static std::mutex s_singleRecompilationMutex;
#endif

void PPCRecompiler_recompileAtAddress(uint32 address);

// this function does never block and can fail if the recompiler lock cannot be acquired immediately
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	s_ppcRecompilerState.recompilerSpinlock.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	s_singleRecompilationMutex.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompiler_recompileAtAddress(enterAddress);
	}
	s_singleRecompilationMutex.unlock();
	return;
#endif
	// quick read-only check without lock
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	// try to acquire lock
	if (!s_ppcRecompilerState.recompilerSpinlock.try_lock())
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		// was visited since previous check
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	// add to recompilation queue and flag as visited
	s_ppcRecompilerState.targetQueue.emplace(enterAddress);
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevCsr = _mm_getcsr();
	if (prevCsr & _MM_ROUND_MASK) [[unlikely]]
		_mm_setcsr(prevCsr & ~_MM_ROUND_MASK);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	if (prevCsr & _MM_ROUND_MASK) [[unlikely]]
		_mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | (prevCsr & _MM_ROUND_MASK));
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// enter
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);
void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext);

// creating jitmap.enable in the user data folder writes a perf map of all recompiled functions to jitmap.txt, so profiles
// taken on devices without a debugger can be attributed to guest functions. jitdump.enable also dumps their code.
static void PPCRecompiler_AddToJitMap(const void* code, size_t size, const std::string& name)
{
	static std::mutex s_mutex;
	static bool s_initialized = false;
	static FileStream* s_mapFile = nullptr;
	std::unique_lock lock(s_mutex);
	if (!s_initialized)
	{
		s_initialized = true;
		std::error_code ec;
		if (fs::exists(ActiveSettings::GetUserDataPath("jitmap.enable"), ec))
		{
			s_mapFile = FileStream::createFile2(ActiveSettings::GetUserDataPath("jitmap.txt"));
			if (fs::exists(ActiveSettings::GetUserDataPath("jitdump.enable"), ec))
			{
				fs::create_directories(ActiveSettings::GetUserDataPath("dump/recompiler"), ec);
				ActiveSettings::EnableDumpRecompilerFunctions(true);
			}
		}
	}
	if (!s_mapFile)
		return;
	s_mapFile->writeString(fmt::format("{:x} {:x} {}\n", (uintptr_t)code, size, name).c_str());
	s_mapFile->Flush();
}

#ifdef ENABLE_LLVM_RECOMPILER
std::atomic<uint32> g_PPCRecompilerLLVM_generation{1}; // call sites start zeroed, so they never match

// host entry points of functions translated by LLVM, to resolve direct calls between them
static std::mutex s_llvmEntryPointMutex;
static std::unordered_map<void*, std::pair<void*, uint32>> s_llvmEntryPoints;

static void PPCRecompiler_addToLLVMFunctionList(uint32 address);

static void PPCRecompiler_registerLLVMEntryPoints(PPCRecFunction_t* ppcRecFunc, const std::vector<std::pair<MPTR, uint32>>& entryPoints, void* llvmFunction)
{
	std::unique_lock lock(s_llvmEntryPointMutex);
	// the entry indices of the translated function follow the order of the enterable segments, like the entry points
	for (size_t i = 0; i < entryPoints.size(); i++)
		s_llvmEntryPoints[(uint8*)ppcRecFunc->x86Code + entryPoints[i].second] = {llvmFunction, (uint32)i};
}
#endif

// with useCachedLLVM a translation of the LLVM backend is used if the cache has one, usedLLVM tells if it was
PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker,
												  bool useCachedLLVM = false, bool* usedLLVM = nullptr)
{
	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				return nullptr;
			}
		}
	}

	PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
	if (compiledSuccessfully == false)
	{
		delete ppcRecFunc;
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			delete ppcRecFunc;
			return nullptr;
		}
	}

	// apply passes
	if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
	{
		delete ppcRecFunc;
		return nullptr;
	}

	bool useLLVMEntryPoints = false;
#if defined(ARCH_X86_64)
	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		return nullptr;
	}
#elif defined(__aarch64__)
	bool aarch64GenerationSuccess = false;
#ifdef ENABLE_LLVM_RECOMPILER
	// the LLVM backend works on the IML before register allocation
	PPCRecompilerLLVM_Result llvmResult;
	const bool useLLVM = useCachedLLVM && PPCRecompilerLLVM_LoadCached(ppcImlGenContext, llvmResult);
	if (usedLLVM)
		*usedLLVM = useLLVM;
	if (useLLVM)
	{
		aarch64GenerationSuccess = PPCRecompiler_generateAArch64LLVMEntryPoints(ppcRecFunc, &ppcImlGenContext, (void*)llvmResult.function, PPCRecompilerLLVM_IsStatsEnabled() ? g_PPCRecompilerLLVM_stats : nullptr);
		useLLVMEntryPoints = true;
	}
	else
#endif
	{
		PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);
		aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
	}
	if (aarch64GenerationSuccess == false)
	{
		return nullptr;
	}
#endif
	PPCRecompiler_AddToJitMap(ppcRecFunc->x86Code, ppcRecFunc->x86Size, fmt::format("ppc_{:08x}{}", ppcRecFunc->ppcAddress, useLLVMEntryPoints ? "_entry" : ""));
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}
#ifdef ENABLE_LLVM_RECOMPILER
	if (useLLVM)
	{
		PPCRecompiler_AddToJitMap(llvmResult.code, llvmResult.codeSize, fmt::format("ppc_{:08x}_llvm", ppcRecFunc->ppcAddress));
		if (ActiveSettings::DumpRecompilerFunctionsEnabled())
		{
			FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}_llvm.bin", ppcRecFunc->ppcAddress)));
			if (fs)
			{
				fs->writeData(llvmResult.code, llvmResult.codeSize);
				delete fs;
			}
		}
	}
#endif

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}
#ifdef ENABLE_LLVM_RECOMPILER
	if (useLLVMEntryPoints)
		PPCRecompiler_registerLLVMEntryPoints(ppcRecFunc, entryPointsOut, llvmResult.function);
#endif

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	IMLOptimizer_OptimizeSinglePrecisionStores(ppcImlGenContext);

	return true;
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	// update jump table
	s_ppcRecompilerState.recompilerSpinlock.lock();

	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// check if the current range got invalidated during the time it took to recompile it
	bool isInvalidated = false;
	for (auto& invRange : s_ppcRecompilerState.invalidationRanges)
	{
		MPTR rStartAddr = invRange.startAddress;
		MPTR rEndAddr = rStartAddr + invRange.size;
		for (auto& recFuncRange : ppcRecFunc->list_ranges)
		{
			if (recFuncRange.ppcAddress < (rEndAddr) && (recFuncRange.ppcAddress + recFuncRange.ppcSize) > rStartAddr)
			{
				isInvalidated = true;
				break;
			}
		}
	}
	s_ppcRecompilerState.invalidationRanges.clear();
	if (isInvalidated)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// update jump table and remember which entries we updated
	cemu_assert_debug(ppcRecFunc->jumpTableEntries.empty());
	for (auto& itr : entryPoints)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
		ppcRecFunc->jumpTableEntries.emplace_back(itr.first, ((uint8*)ppcRecFunc->x86Code + itr.second));
	}

	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	for (uint32 v = range.startAddress; v < (range.startAddress + range.length); v += 4)
	{
		auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
		if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}

	// register ranges
	for (auto& r : ppcRecFunc->list_ranges)
	{
		r.storedRange = s_ppcRecompilerState.functionStorage.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
	}
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	s_ppcRecompilerState.recompilerSpinlock.lock();
	std::set<uint32> entryAddresses;
	entryAddresses.emplace(address);
	s_ppcRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	bool useCachedLLVM = false;
#ifdef ENABLE_LLVM_RECOMPILER
	useCachedLLVM = PPCRecompilerLLVM_IsEnabled();
#endif
	bool usedLLVM = false;
	PPCRecFunction_t* func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries, useCachedLLVM, &usedLLVM);
	if (!func)
		return; // recompilation failed
	bool isActive = PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
#ifdef ENABLE_LLVM_RECOMPILER
	// call sites that found no translated function there can call this one directly now
	if (isActive && usedLLVM)
		g_PPCRecompilerLLVM_generation.fetch_add(1, std::memory_order_release);
	// cached ones too, so the list stays complete when the cache is rebuilt
	if (isActive && useCachedLLVM)
		PPCRecompiler_addToLLVMFunctionList(address);
#endif
}

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	return;
#endif

	while (true)
	{
        if(s_ppcRecompilerState.workerThreadStopSignal)
            return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		while (true)
		{
			s_ppcRecompilerState.recompilerSpinlock.lock();
			if (s_ppcRecompilerState.targetQueue.empty())
			{
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				break;
			}
			auto enterAddress = s_ppcRecompilerState.targetQueue.front();
			s_ppcRecompilerState.targetQueue.pop();

			auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
			if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
			{
				// only recompile functions if marked as visited
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				continue;
			}
			s_ppcRecompilerState.recompilerSpinlock.unlock();

			PPCRecompiler_recompileAtAddress(enterAddress);
			if(s_ppcRecompilerState.workerThreadStopSignal)
				return;
		}
	}
}

#ifdef ENABLE_LLVM_RECOMPILER
// Functions a title ran in earlier sessions are translated by LLVM before it starts, the recompiler then takes those
// translations from the cache. Functions that run for the first time are translated natively and added to the list for
// the next start, so the slow translation never runs while the title does.
static struct
{
	std::mutex mutex;
	std::unordered_set<uint32> addresses;
	std::unique_ptr<FileStream> file;
	std::atomic_bool isPending{false};
	std::atomic<uint32> preparedCount{0};
	std::atomic<uint32> totalCount{0};
}s_llvmFunctionList;

static void PPCRecompiler_openLLVMFunctionList()
{
	std::unique_lock lock(s_llvmFunctionList.mutex);
	s_llvmFunctionList.addresses.clear();
	s_llvmFunctionList.file.reset();
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("llvm"), ec);
	const fs::path path = ActiveSettings::GetCachePath("llvm/{:016x}.functions", CafeSystem::GetForegroundTitleId());
	auto data = FileStream::LoadIntoMemory(path);
	if (!data)
	{
		s_llvmFunctionList.file.reset(FileStream::createFile2(path));
		return;
	}
	const size_t validSize = data->size() & ~3;
	for (size_t i = 0; i < validSize; i += 4)
	{
		uint32 address;
		memcpy(&address, data->data() + i, sizeof(address));
		s_llvmFunctionList.addresses.emplace(address);
	}
	if (validSize != data->size())
		fs::resize_file(path, validSize, ec);
	s_llvmFunctionList.file.reset(FileStream::openFile2(path, true));
	if (s_llvmFunctionList.file)
		s_llvmFunctionList.file->SetPosition(validSize);
}

static void PPCRecompiler_addToLLVMFunctionList(uint32 address)
{
	std::unique_lock lock(s_llvmFunctionList.mutex);
	if (!s_llvmFunctionList.file || !s_llvmFunctionList.addresses.emplace(address).second)
		return;
	s_llvmFunctionList.file->writeU32(address);
	s_llvmFunctionList.file->Flush();
}

static void PPCRecompiler_prepareLLVMFunction(uint32 address)
{
	// the list can contain code of modules the title loads later
	if (!RPLLoader_FindModuleByCodeAddr(address))
		return;
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (!funcBoundaries.getRangeForAddress(address, range))
		return;
	std::set<uint32> entryAddresses;
	entryAddresses.emplace(address);
	PPCRecFunction_t ppcRecFunc{};
	ppcRecFunc.ppcAddress = range.startAddress;
	ppcRecFunc.ppcSize = range.length;
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	if (!PPCRecompiler_generateIntermediateCode(ppcImlGenContext, &ppcRecFunc, entryAddresses, funcBoundaries))
		return;
	PPCRecompiler_ApplyIMLPasses(ppcImlGenContext);
	PPCRecompilerLLVM_Prepare(ppcImlGenContext, ppcRecFunc);
}

#if BOOST_OS_LINUX
// keeps the calling thread off the fastest cores, they get hot the quickest
static void PPCRecompiler_avoidFastestCores()
{
	std::vector<uint32> maxFrequencies;
	while (true)
	{
		std::ifstream file(fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", maxFrequencies.size()));
		uint32 frequency;
		if (!(file >> frequency))
			break;
		maxFrequencies.emplace_back(frequency);
	}
	if (maxFrequencies.empty())
		return;
	const uint32 fastest = *std::max_element(maxFrequencies.begin(), maxFrequencies.end());
	cpu_set_t cores;
	CPU_ZERO(&cores);
	for (size_t i = 0; i < maxFrequencies.size(); i++)
	{
		if (maxFrequencies[i] < fastest)
			CPU_SET(i, &cores);
	}
	if (CPU_COUNT(&cores) > 0)
		sched_setaffinity(0, sizeof(cores), &cores);
}
#endif

void PPCRecompiler_prepareKnownFunctions()
{
	if (!s_llvmFunctionList.isPending)
		return;
	std::vector<uint32> addresses;
	{
		std::unique_lock lock(s_llvmFunctionList.mutex);
		addresses.assign(s_llvmFunctionList.addresses.begin(), s_llvmFunctionList.addresses.end());
	}
	std::sort(addresses.begin(), addresses.end());
	s_llvmFunctionList.preparedCount = 0;
	s_llvmFunctionList.totalCount = (uint32)addresses.size();
	BenchmarkTimer timer;
	timer.Start();
	// half of the cores, and not the fastest ones, so the device doesn't heat up too much
	std::atomic<size_t> nextIndex{0};
	std::vector<std::thread> threads;
	const uint32 threadCount = std::max(2u, std::thread::hardware_concurrency() / 2);
	for (uint32 i = 0; i < threadCount; i++)
	{
		threads.emplace_back([&]() {
			SetThreadName("PPCRecLLVM");
#if BOOST_OS_LINUX
			PPCRecompiler_avoidFastestCores();
#endif
			for (size_t index = nextIndex++; index < addresses.size(); index = nextIndex++)
			{
				PPCRecompiler_prepareLLVMFunction(addresses[index]);
				s_llvmFunctionList.preparedCount++;
			}
		});
	}
	for (std::thread& thread : threads)
		thread.join();
	timer.Stop();
	cemuLog_log(LogType::Force, "LLVM: Prepared {} functions in {:.1f}s", addresses.size(), timer.GetElapsedMilliseconds() / 1000.0);
	s_llvmFunctionList.isPending = false;
}

bool PPCRecompiler_getLLVMPreparationProgress(uint32& preparedCount, uint32& totalCount)
{
	preparedCount = s_llvmFunctionList.preparedCount;
	totalCount = s_llvmFunctionList.totalCount;
	return s_llvmFunctionList.isPending;
}
#endif

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

#ifdef ENABLE_LLVM_RECOMPILER
void PPCRecompilerLLVM_ResolveCall(uint32 target, PPCRecompilerLLVM_CallSite* site)
{
	// read before the jump table, a change during the lookup makes the call site resolve again
	const uint32 generation = g_PPCRecompilerLLVM_generation.load(std::memory_order_acquire);
	g_PPCRecompilerLLVM_stats[LLVM_STAT_RESOLVES]++;
	void* function = nullptr;
	uint32 entryIndex = 0;
	if (target / PPC_REC_ALLOC_BLOCK_SIZE < ppcRecompiler_reservedBlockMask.size() && ppcRecompiler_reservedBlockMask[target / PPC_REC_ALLOC_BLOCK_SIZE])
	{
		void* entryPoint = (void*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[target / 4];
		std::unique_lock lock(s_llvmEntryPointMutex);
		if (auto it = s_llvmEntryPoints.find(entryPoint); it != s_llvmEntryPoints.end())
		{
			std::tie(function, entryIndex) = it->second;
			g_PPCRecompilerLLVM_stats[LLVM_STAT_RESOLVED]++;
		}
	}
	site->function = function;
	site->entryIndex = entryIndex;
	site->key.store(((uint64)generation << 32) | target, std::memory_order_release);
}
#endif

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange* rangesOut, size_t* countInOut)
{
	s_ppcRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	s_ppcRecompilerState.functionStorage.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t* PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
	cemu_assert_debug(s_ppcRecompilerState.recompilerSpinlock.is_locked());
	// unlink entrypoints from JumpTable
	for (auto& entrypoint : func->jumpTableEntries)
	{
		if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] == entrypoint.hostEntrypoint)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
	// delete from storage
	for (auto& r : func->list_ranges)
	{
		if(r.storedRange)
			s_ppcRecompilerState.functionStorage.deleteRange(r.storedRange);
		r.storedRange = nullptr;
	}
#ifdef ENABLE_LLVM_RECOMPILER
	// direct calls from translated code must not reach the deleted function anymore
	g_PPCRecompilerLLVM_generation.fetch_add(1, std::memory_order_release);
#endif
	// todo - free x86 code
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (!s_ppcRecompilerState.initialized)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);

	s_ppcRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;
	// delete functions which intersect the invalidated range
	while (s_ppcRecompilerState.functionStorage.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc))
		PPCRecompiler_deleteFunction(rFunc);
	// add entry to invalidation queue, this is used to invalidate functions for which recompilation has already started
	s_ppcRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_constDouble2p149[0] = 0x1p149;
	ppcRecompilerInstanceData->_x64XMM_constDouble2p149[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
	ppcRecompilerInstanceData->_x64_pextSingleMask = 0xC7FFFFFFE0000000ULL; // sign, exponent bit 62, then bits 58-29
}
#else
void PPCRecompiler_initPlatform()
{

}
#endif

void PPCRecompiler_init()
{
	s_ppcRecompilerState.recompilerEnableCount = 0;
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		cemuLog_log(LogType::Force, "Using singlecore interpreter");
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	cemuLog_logDebug(LogType::Force, "Reserving {}MB for recompiler instance data", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();

	cemuLog_log(LogType::Force, "Recompiler initialized");

	s_ppcRecompilerState.initialized = true;
	s_ppcRecompilerState.recompilerEnableCount = 1; // enabled

	// launch recompilation thread
    s_ppcRecompilerState.workerThreadStopSignal = false;
    s_ppcRecompilerState.workerThread = std::thread(PPCRecompiler_thread);
#ifdef ENABLE_LLVM_RECOMPILER
	if (PPCRecompilerLLVM_IsEnabled())
	{
		PPCRecompiler_openLLVMFunctionList();
		s_llvmFunctionList.isPending = true;
	}
#endif
}

void PPCRecompiler_Shutdown()
{
    // shut down recompiler thread
    s_ppcRecompilerState.workerThreadStopSignal = true;
    if(s_ppcRecompilerState.workerThread.joinable())
        s_ppcRecompilerState.workerThread.join();
#ifdef ENABLE_LLVM_RECOMPILER
	{
		std::unique_lock lock(s_llvmFunctionList.mutex);
		s_llvmFunctionList.file.reset();
	}
	s_llvmFunctionList.isPending = false;
#endif
    // clean up queues
    while(!s_ppcRecompilerState.targetQueue.empty())
        s_ppcRecompilerState.targetQueue.pop();
    s_ppcRecompilerState.invalidationRanges.clear();
    // clean range store
    s_ppcRecompilerState.functionStorage.clear();
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
	s_ppcRecompilerState.recompilerEnableCount = 0;
	s_ppcRecompilerState.initialized = false;
}

// For each Enable call, Disable needs to be called once and vice versa
void PPCRecompiler_Enable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount++;
}

void PPCRecompiler_Disable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount--;
}