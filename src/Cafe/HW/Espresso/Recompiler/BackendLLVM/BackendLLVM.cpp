#include "BackendLLVM.h"

#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCFunctionBoundaryTracker.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Recompiler/IML/IML.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Common/version.h"
#include "config/ActiveSettings.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/AllocationActions.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <sys/mman.h>

ATTR_MS_ABI uint32 PPCRecompiler_GetTBL();
ATTR_MS_ABI uint32 PPCRecompiler_GetTBU();

namespace
{
	// kept in the low bits of the returned guest address, the entry points emitted by the AArch64 backend dispatch on them
	constexpr uint32 EXIT_JUMP = 0;
	constexpr uint32 EXIT_LEAVE = 1;
	constexpr uint32 EXIT_CALL = 2;
	constexpr uint32 EXIT_RETURN = 3;

	// direct calls nest on the host stack of the guest thread's fiber
	constexpr uint32 MAX_CALL_DEPTH = 48;

	// C++ functions the IML calls with CALL_IMM
	struct HelperFunction
	{
		const char* name;
		void* address;
	};

	const HelperFunction s_helperFunctions[] = {
		{"fres_espresso", (void*)fres_espresso},
		{"frsqrte_espresso", (void*)frsqrte_espresso},
		{"PPCRecompiler_GetTBL", (void*)PPCRecompiler_GetTBL},
		{"PPCRecompiler_GetTBU", (void*)PPCRecompiler_GetTBU},
		{"PPCRecompilerLLVM_ResolveCall", (void*)PPCRecompilerLLVM_ResolveCall},
	};

	const char* GetHelperName(uintptr_t address)
	{
		for (const HelperFunction& helper : s_helperFunctions)
		{
			if ((uintptr_t)helper.address == address)
				return helper.name;
		}
		return nullptr;
	}

	struct NameLocation
	{
		uint32 offset;
		uint8 size;
		bool isFloat;
	};

	// where the recompiler keeps each register name in PPCInterpreter_t, the same as the native backends
	bool GetNameLocation(IMLName name, NameLocation& location)
	{
		if (name >= PPCREC_NAME_R0 && name < PPCREC_NAME_R0 + 32)
		{
			location = {(uint32)(offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * (name - PPCREC_NAME_R0)), 4, false};
			return true;
		}
		if (name >= PPCREC_NAME_SPR0 && name < PPCREC_NAME_SPR0 + 999)
		{
			const uint32 sprIndex = name - PPCREC_NAME_SPR0;
			if (sprIndex == SPR_LR)
				location = {(uint32)offsetof(PPCInterpreter_t, spr.LR), 4, false};
			else if (sprIndex == SPR_CTR)
				location = {(uint32)offsetof(PPCInterpreter_t, spr.CTR), 4, false};
			else if (sprIndex == SPR_XER)
				location = {(uint32)offsetof(PPCInterpreter_t, spr.XER), 4, false};
			else if (sprIndex >= SPR_UGQR0 && sprIndex <= SPR_UGQR7)
				location = {(uint32)(offsetof(PPCInterpreter_t, spr.UGQR) + sizeof(PPCInterpreter_t::spr.UGQR[0]) * (sprIndex - SPR_UGQR0)), 4, false};
			else
				return false;
			return true;
		}
		if (name >= PPCREC_NAME_TEMPORARY && name < PPCREC_NAME_TEMPORARY + 4)
		{
			location = {(uint32)(offsetof(PPCInterpreter_t, temporaryGPR_reg) + sizeof(uint32) * (name - PPCREC_NAME_TEMPORARY)), 4, false};
			return true;
		}
		if (name == PPCREC_NAME_XER_CA)
		{
			location = {(uint32)offsetof(PPCInterpreter_t, xer_ca), 1, false};
			return true;
		}
		if (name == PPCREC_NAME_XER_SO)
		{
			location = {(uint32)offsetof(PPCInterpreter_t, xer_so), 1, false};
			return true;
		}
		if (name >= PPCREC_NAME_CR && name <= PPCREC_NAME_CR_LAST)
		{
			location = {(uint32)(offsetof(PPCInterpreter_t, cr) + (name - PPCREC_NAME_CR)), 1, false};
			return true;
		}
		if (name == PPCREC_NAME_CPU_MEMRES_EA)
		{
			location = {(uint32)offsetof(PPCInterpreter_t, reservedMemAddr), 4, false};
			return true;
		}
		if (name == PPCREC_NAME_CPU_MEMRES_VAL)
		{
			location = {(uint32)offsetof(PPCInterpreter_t, reservedMemValue), 4, false};
			return true;
		}
		if (name >= PPCREC_NAME_FPR_HALF && name < PPCREC_NAME_FPR_HALF + 64)
		{
			const uint32 regIndex = (name - PPCREC_NAME_FPR_HALF) / 2;
			const uint32 pairIndex = (name - PPCREC_NAME_FPR_HALF) % 2;
			location = {(uint32)(offsetof(PPCInterpreter_t, fpr) + sizeof(FPR_t) * regIndex + (pairIndex ? sizeof(double) : 0)), 8, true};
			return true;
		}
		if (name >= PPCREC_NAME_TEMPORARY_FPR0 && name < PPCREC_NAME_TEMPORARY_FPR0 + 8)
		{
			location = {(uint32)(offsetof(PPCInterpreter_t, temporaryFPR) + sizeof(FPR_t) * (name - PPCREC_NAME_TEMPORARY_FPR0)), 8, true};
			return true;
		}
		return false;
	}

	class FunctionTranslator
	{
	  public:
		FunctionTranslator(ppcImlGenContext_t& iml, llvm::LLVMContext& context, llvm::Module& module)
			: m_iml(iml), m_context(context), m_module(module), m_builder(context)
		{
			m_i8 = llvm::Type::getInt8Ty(context);
			m_i16 = llvm::Type::getInt16Ty(context);
			m_i32 = llvm::Type::getInt32Ty(context);
			m_i64 = llvm::Type::getInt64Ty(context);
			m_f32 = llvm::Type::getFloatTy(context);
			m_f64 = llvm::Type::getDoubleTy(context);
			m_ptr = llvm::PointerType::getUnqual(context);
		}

		bool Translate(const std::string& name, std::vector<uint32>& entryAddresses)
		{
			auto* functionType = llvm::FunctionType::get(m_i32, {m_ptr, m_ptr, m_i32, m_i32}, false);
			m_function = llvm::Function::Create(functionType, llvm::Function::ExternalLinkage, name, m_module);
			m_function->addFnAttr(llvm::Attribute::NoUnwind);
			// only called from the entry points emitted by the AArch64 backend, which don't need any registers preserved
			m_function->setCallingConv(llvm::CallingConv::PreserveNone);
			// the context is only accessed through this pointer while the function runs, the helpers called by CALL_IMM don't touch it
			m_function->addParamAttr(0, llvm::Attribute::NoAlias);
			m_hCPU = m_function->getArg(0);
			m_memoryBase = m_function->getArg(1);

			auto* entryBlock = llvm::BasicBlock::Create(m_context, "entry", m_function);
			m_exitBlock = llvm::BasicBlock::Create(m_context, "exit", m_function);
			for (IMLSegment* segment : m_iml.segmentList2)
				m_segmentBlocks[segment] = llvm::BasicBlock::Create(m_context, "", m_function);

			if (auto it = m_iml.mappedRegs.find(PPCREC_NAME_R0 + 1); it != m_iml.mappedRegs.end())
				m_stackPointerRegId = it->second.GetRegID();

			// registers only read by this function don't have to be written back
			std::unordered_set<IMLRegID> writtenRegs;
			for (IMLSegment* segment : m_iml.segmentList2)
			{
				for (const IMLInstruction& instruction : segment->imlList)
				{
					IMLUsedRegisters registersUsed;
					instruction.CheckRegisterUsage(&registersUsed);
					registersUsed.ForEachWrittenGPR([&](IMLReg reg) { writtenRegs.emplace(reg.GetRegID()); });
				}
			}

			// every register name lives in a local variable which LLVM promotes to host registers
			m_builder.SetInsertPoint(entryBlock);
			for (auto& [name, reg] : m_iml.mappedRegs)
			{
				NameLocation location;
				if (!GetNameLocation(name, location))
					return Reject("unknown register name");
				const bool isFloat = reg.GetBaseFormat() == IMLRegFormat::F64;
				if (isFloat != location.isFloat)
					return Reject("register name format mismatch");
				m_slots[reg.GetRegID()] = {m_builder.CreateAlloca(isFloat ? m_f64 : m_i64), location, writtenRegs.contains(reg.GetRegID())};
			}
			m_cyclesSlot = m_builder.CreateAlloca(m_i32);
			m_builder.CreateStore(m_builder.CreateLoad(m_i32, ContextPointer(offsetof(PPCInterpreter_t, remainingCycles))), m_cyclesSlot);

			// each entry point loads the registers it needs from the context
			auto liveIn = ComputeLiveIn(writtenRegs);
			auto* unreachableBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* entrySwitch = m_builder.CreateSwitch(m_function->getArg(2), unreachableBlock);
			for (IMLSegment* segment : m_iml.segmentList2)
			{
				if (!segment->isEnterable)
					continue;
				auto* loadBlock = llvm::BasicBlock::Create(m_context, "", m_function);
				entrySwitch->addCase(m_builder.getInt32((uint32)entryAddresses.size()), loadBlock);
				entryAddresses.emplace_back(segment->enterPPCAddress);
				m_entryLoadBlocks[segment->enterPPCAddress] = loadBlock;
				m_builder.SetInsertPoint(loadBlock);
				for (IMLRegID regId : liveIn[segment])
				{
					Slot& slot = m_slots[regId];
					llvm::Value* value;
					if (slot.location.isFloat)
						value = m_builder.CreateLoad(m_f64, ContextPointer(slot.location.offset));
					else
						value = m_builder.CreateZExt(m_builder.CreateLoad(IntType(slot.location.size), ContextPointer(slot.location.offset)), m_i64);
					m_builder.CreateStore(value, slot.slot);
				}
				m_builder.CreateBr(m_segmentBlocks[segment]);
			}
			m_builder.SetInsertPoint(unreachableBlock);
			m_builder.CreateUnreachable();

			m_builder.SetInsertPoint(m_exitBlock);
			m_exitAddress = m_builder.CreatePHI(m_i32, 4);
			WriteBack();
			m_builder.CreateRet(m_exitAddress);

			for (IMLSegment* segment : m_iml.segmentList2)
			{
				m_builder.SetInsertPoint(m_segmentBlocks[segment]);
				if (!TranslateSegment(segment))
					return false;
			}
			if (entryAddresses.empty())
				return Reject("no entry points");
			std::string error;
			llvm::raw_string_ostream errorStream(error);
			if (llvm::verifyFunction(*m_function, &errorStream))
			{
				cemuLog_log(LogType::Force, "LLVM: Generated invalid IR for 0x{:08x}: {}", m_iml.debug_entryPPCAddress, error);
				return Reject("invalid IR");
			}
			return true;
		}

		const char* GetRejectReason() const { return m_rejectReason; }

	  private:
		static bool IsExit(const IMLInstruction& instruction)
		{
			if (instruction.type != IMLType::MACRO)
				return false;
			return instruction.operation == IMLOperation::MACRO_BL || instruction.operation == IMLOperation::MACRO_B_FAR ||
				   instruction.operation == IMLOperation::MACRO_B_TO_REG || instruction.operation == IMLOperation::MACRO_LEAVE;
		}

		// Registers that are read before being written on some path from the start of each segment. Exits write back
		// the registers the function modifies, so those count as read there.
		std::unordered_map<IMLSegment*, std::unordered_set<IMLRegID>> ComputeLiveIn(const std::unordered_set<IMLRegID>& writtenRegs)
		{
			struct SegmentUsage
			{
				std::unordered_set<IMLRegID> read;
				std::unordered_set<IMLRegID> written;
				bool exits = false;
			};
			std::unordered_map<IMLSegment*, SegmentUsage> usage;
			for (IMLSegment* segment : m_iml.segmentList2)
			{
				SegmentUsage& segmentUsage = usage[segment];
				for (const IMLInstruction& instruction : segment->imlList)
				{
					IMLUsedRegisters registersUsed;
					instruction.CheckRegisterUsage(&registersUsed);
					registersUsed.ForEachReadGPR([&](IMLReg reg) {
						if (!segmentUsage.written.contains(reg.GetRegID()))
							segmentUsage.read.emplace(reg.GetRegID());
					});
					if (IsExit(instruction))
					{
						for (IMLRegID regId : writtenRegs)
						{
							if (!segmentUsage.written.contains(regId))
								segmentUsage.read.emplace(regId);
						}
						segmentUsage.exits = true;
						break;
					}
					registersUsed.ForEachWrittenGPR([&](IMLReg reg) { segmentUsage.written.emplace(reg.GetRegID()); });
				}
			}
			// the sets only grow, so a pass without a size change is the fixed point
			std::unordered_map<IMLSegment*, std::unordered_set<IMLRegID>> liveIn;
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (auto it = m_iml.segmentList2.rbegin(); it != m_iml.segmentList2.rend(); ++it)
				{
					IMLSegment* segment = *it;
					SegmentUsage& segmentUsage = usage[segment];
					std::unordered_set<IMLRegID> live = segmentUsage.read;
					if (!segmentUsage.exits)
					{
						for (IMLSegment* successor : {segment->nextSegmentBranchTaken, segment->nextSegmentBranchNotTaken})
						{
							if (!successor)
								continue;
							for (IMLRegID regId : liveIn[successor])
							{
								if (!segmentUsage.written.contains(regId))
									live.emplace(regId);
							}
						}
					}
					if (live.size() != liveIn[segment].size())
					{
						liveIn[segment] = std::move(live);
						changed = true;
					}
				}
			}
			return liveIn;
		}

		struct Slot
		{
			llvm::AllocaInst* slot;
			NameLocation location;
			bool isWritten;
		};

		bool Reject(const char* reason)
		{
			m_rejectReason = reason;
			return false;
		}

		llvm::Type* IntType(uint32 size)
		{
			switch (size)
			{
			case 1: return m_i8;
			case 2: return m_i16;
			case 8: return m_i64;
			default: return m_i32;
			}
		}

		llvm::Type* RegType(IMLReg reg)
		{
			switch (reg.GetRegFormat())
			{
			case IMLRegFormat::I64: return m_i64;
			case IMLRegFormat::I16: return m_i16;
			case IMLRegFormat::I8: return m_i8;
			case IMLRegFormat::F64: return m_f64;
			default: return m_i32;
			}
		}

		llvm::Value* ContextPointer(size_t offset)
		{
			return m_builder.CreateConstInBoundsGEP1_64(m_i8, m_hCPU, offset);
		}

		Slot* GetSlot(IMLReg reg)
		{
			auto it = m_slots.find(reg.GetRegID());
			return it == m_slots.end() ? nullptr : &it->second;
		}

		// integer registers are stored with 64 bits, reads in a narrower format truncate, writes zero extend like on AArch64
		llvm::Value* GPR(IMLReg reg)
		{
			llvm::Value* value = m_builder.CreateLoad(m_i64, GetSlot(reg)->slot);
			auto* type = RegType(reg);
			return type == m_i64 ? value : m_builder.CreateTrunc(value, type);
		}

		llvm::Value* GPR32(IMLReg reg)
		{
			return m_builder.CreateTrunc(m_builder.CreateLoad(m_i64, GetSlot(reg)->slot), m_i32);
		}

		void SetGPR(IMLReg reg, llvm::Value* value)
		{
			if (value->getType() != m_i64)
				value = m_builder.CreateZExt(value, m_i64);
			m_builder.CreateStore(value, GetSlot(reg)->slot);
		}

		llvm::Value* FPR(IMLReg reg)
		{
			return m_builder.CreateLoad(m_f64, GetSlot(reg)->slot);
		}

		void SetFPR(IMLReg reg, llvm::Value* value)
		{
			m_builder.CreateStore(value, GetSlot(reg)->slot);
		}

		bool HasSlots(std::initializer_list<IMLReg> regs)
		{
			for (const IMLReg& reg : regs)
			{
				if (reg.IsValid() && !GetSlot(reg))
					return false;
			}
			return true;
		}

		struct GuestAddress
		{
			llvm::Value* pointer;
			bool isStack;
		};

		// guest addresses wrap around at 32 bits
		GuestAddress GuestPointer(IMLReg memReg, sint32 offset, const IMLReg* indexReg)
		{
			llvm::Value* address = GPR32(memReg);
			if (offset != 0)
				address = m_builder.CreateAdd(address, m_builder.getInt32((uint32)offset));
			if (indexReg)
				address = m_builder.CreateAdd(address, GPR32(*indexReg));
			const bool isStack = !indexReg && m_stackPointerRegId && memReg.GetRegID() == *m_stackPointerRegId;
			return {m_builder.CreateInBoundsGEP(m_i8, m_memoryBase, m_builder.CreateZExt(address, m_i64)), isStack};
		}

		// Guest memory is shared with other cores and the GPU, so accesses must not be merged or moved out of loops.
		// The stack of a thread isn't, accesses relative to r1 can be optimized like ordinary memory.
		llvm::Value* LoadGuest(llvm::Type* type, const GuestAddress& address)
		{
			auto* load = m_builder.CreateLoad(type, address.pointer, !address.isStack);
			load->setAlignment(llvm::Align(1));
			return load;
		}

		void StoreGuest(llvm::Value* value, const GuestAddress& address)
		{
			auto* store = m_builder.CreateStore(value, address.pointer, !address.isStack);
			store->setAlignment(llvm::Align(1));
		}

		llvm::Value* ByteSwap(llvm::Value* value)
		{
			return m_builder.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, value);
		}

		llvm::Value* Compare(IMLCondition cond, llvm::Value* a, llvm::Value* b)
		{
			switch (cond)
			{
			case IMLCondition::EQ: return m_builder.CreateICmpEQ(a, b);
			case IMLCondition::NEQ: return m_builder.CreateICmpNE(a, b);
			case IMLCondition::SIGNED_GT: return m_builder.CreateICmpSGT(a, b);
			case IMLCondition::SIGNED_LT: return m_builder.CreateICmpSLT(a, b);
			case IMLCondition::UNSIGNED_GT: return m_builder.CreateICmpUGT(a, b);
			case IMLCondition::UNSIGNED_LT: return m_builder.CreateICmpULT(a, b);
			default: return nullptr;
			}
		}

		// stores the registers this function modifies and the remaining cycles to the context
		void WriteBack()
		{
			for (auto& [regId, slot] : m_slots)
			{
				if (!slot.isWritten)
					continue;
				llvm::Value* value = m_builder.CreateLoad(slot.location.isFloat ? m_f64 : m_i64, slot.slot);
				if (!slot.location.isFloat)
					value = m_builder.CreateTrunc(value, IntType(slot.location.size));
				m_builder.CreateStore(value, ContextPointer(slot.location.offset));
			}
			m_builder.CreateStore(m_builder.CreateLoad(m_i32, m_cyclesSlot), ContextPointer(offsetof(PPCInterpreter_t, remainingCycles)));
		}

		// A guest call becomes a host call if the target is translated by LLVM too. When the callee returns to the
		// instruction after the call this function continues there, any other result is passed on like an exit.
		// The registers are in the context during the call, so both cases only need to reload what is live afterwards.
		void DirectCall(uint32 target, uint32 returnAddress)
		{
			WriteBack();
			auto continueIt = m_entryLoadBlocks.find(returnAddress);
			if (continueIt == m_entryLoadBlocks.end())
			{
				m_builder.CreateRet(m_builder.getInt32(target | EXIT_CALL));
				return;
			}
			auto* lookupBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* resolveBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* checkBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* callBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* returnedBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* passOnBlock = llvm::BasicBlock::Create(m_context, "", m_function);
			auto* fallbackBlock = llvm::BasicBlock::Create(m_context, "", m_function);

			llvm::Value* depth = m_function->getArg(3);
			m_builder.CreateCondBr(m_builder.CreateICmpULT(depth, m_builder.getInt32(MAX_CALL_DEPTH)), lookupBlock, fallbackBlock);

			// PPCRecompilerLLVM_CallSite: key, function, entry index
			auto* siteType = llvm::ArrayType::get(m_i64, 3);
			auto* site = new llvm::GlobalVariable(m_module, siteType, false, llvm::GlobalValue::InternalLinkage, llvm::Constant::getNullValue(siteType));
			site->setAlignment(llvm::Align(8));
			m_builder.SetInsertPoint(lookupBlock);
			auto* generationLoad = m_builder.CreateLoad(m_i32, m_module.getOrInsertGlobal("g_PPCRecompilerLLVM_generation", m_i32));
			generationLoad->setAtomic(llvm::AtomicOrdering::Monotonic);
			generationLoad->setAlignment(llvm::Align(4));
			auto* keyLoad = m_builder.CreateLoad(m_i64, site);
			keyLoad->setAtomic(llvm::AtomicOrdering::Acquire);
			keyLoad->setAlignment(llvm::Align(8));
			llvm::Value* expectedKey = m_builder.CreateOr(m_builder.CreateShl(m_builder.CreateZExt(generationLoad, m_i64), 32), m_builder.getInt64(target));
			m_builder.CreateCondBr(m_builder.CreateICmpEQ(keyLoad, expectedKey), checkBlock, resolveBlock);

			m_builder.SetInsertPoint(resolveBlock);
			auto resolve = m_module.getOrInsertFunction("PPCRecompilerLLVM_ResolveCall", llvm::FunctionType::get(m_builder.getVoidTy(), {m_i32, m_ptr}, false));
			m_builder.CreateCall(resolve, {m_builder.getInt32(target), site});
			m_builder.CreateBr(checkBlock);

			m_builder.SetInsertPoint(checkBlock);
			llvm::Value* function = m_builder.CreateLoad(m_ptr, m_builder.CreateConstInBoundsGEP1_64(m_i8, site, 8));
			llvm::Value* entryIndex = m_builder.CreateLoad(m_i32, m_builder.CreateConstInBoundsGEP1_64(m_i8, site, 16));
			m_builder.CreateCondBr(m_builder.CreateIsNotNull(function), callBlock, fallbackBlock);

			m_builder.SetInsertPoint(callBlock);
			auto* call = m_builder.CreateCall(m_function->getFunctionType(), function, {m_hCPU, m_memoryBase, entryIndex, m_builder.CreateAdd(depth, m_builder.getInt32(1))});
			call->setCallingConv(llvm::CallingConv::PreserveNone);
			m_builder.CreateCondBr(m_builder.CreateICmpEQ(call, m_builder.getInt32(returnAddress | EXIT_RETURN)), returnedBlock, passOnBlock);

			m_builder.SetInsertPoint(returnedBlock);
			m_builder.CreateStore(m_builder.CreateLoad(m_i32, ContextPointer(offsetof(PPCInterpreter_t, remainingCycles))), m_cyclesSlot);
			m_builder.CreateBr(continueIt->second);

			// the callee already wrote everything back
			m_builder.SetInsertPoint(passOnBlock);
			m_builder.CreateRet(call);

			m_builder.SetInsertPoint(fallbackBlock);
			m_builder.CreateRet(m_builder.getInt32(target | EXIT_CALL));
		}

		void Exit(llvm::Value* nextAddress)
		{
			m_exitAddress->addIncoming(nextAddress, m_builder.GetInsertBlock());
			m_builder.CreateBr(m_exitBlock);
		}

		bool TranslateSegment(IMLSegment* segment)
		{
			// operands of the last OP_X86_CMP, used by a following X86_EFLAGS_JCC
			llvm::Value* flagsA = nullptr;
			llvm::Value* flagsB = nullptr;
			for (IMLInstruction& instruction : segment->imlList)
			{
				switch (instruction.type)
				{
				case IMLType::NO_OP:
					break;
				case IMLType::JUMP:
					m_builder.CreateBr(m_segmentBlocks[segment->nextSegmentBranchTaken]);
					return true;
				case IMLType::CONDITIONAL_JUMP:
				{
					if (!HasSlots({instruction.op_conditional_jump.registerBool}))
						return Reject("missing register");
					llvm::Value* bit = m_builder.CreateTrunc(GPR32(instruction.op_conditional_jump.registerBool), m_builder.getInt1Ty());
					if (!instruction.op_conditional_jump.mustBeTrue)
						bit = m_builder.CreateNot(bit);
					m_builder.CreateCondBr(bit, m_segmentBlocks[segment->nextSegmentBranchTaken], m_segmentBlocks[segment->nextSegmentBranchNotTaken]);
					return true;
				}
				case IMLType::X86_EFLAGS_JCC:
				{
					if (!flagsA)
						return Reject("flags jump without compare");
					llvm::Value* bit = Compare(instruction.op_x86_eflags_jcc.cond, flagsA, flagsB);
					if (!bit)
						return Reject("unsupported condition");
					if (instruction.op_x86_eflags_jcc.invertedCondition)
						bit = m_builder.CreateNot(bit);
					m_builder.CreateCondBr(bit, m_segmentBlocks[segment->nextSegmentBranchTaken], m_segmentBlocks[segment->nextSegmentBranchNotTaken]);
					return true;
				}
				case IMLType::CJUMP_CYCLE_CHECK:
				{
					llvm::Value* outOfCycles = m_builder.CreateICmpSLT(m_builder.CreateLoad(m_i32, m_cyclesSlot), m_builder.getInt32(0));
					m_builder.CreateCondBr(outOfCycles, m_segmentBlocks[segment->nextSegmentBranchTaken], m_segmentBlocks[segment->nextSegmentBranchNotTaken]);
					return true;
				}
				case IMLType::MACRO:
				{
					bool exits = false;
					if (!TranslateMacro(instruction, exits))
						return false;
					if (exits)
						return true;
					break;
				}
				default:
					if (!TranslateInstruction(instruction, flagsA, flagsB))
						return false;
					break;
				}
			}
			if (!segment->nextSegmentBranchNotTaken)
				return Reject("segment without successor");
			m_builder.CreateBr(m_segmentBlocks[segment->nextSegmentBranchNotTaken]);
			return true;
		}

		bool TranslateMacro(IMLInstruction& instruction, bool& exits)
		{
			switch (instruction.operation)
			{
			case IMLOperation::MACRO_COUNT_CYCLES:
				m_builder.CreateStore(m_builder.CreateSub(m_builder.CreateLoad(m_i32, m_cyclesSlot), m_builder.getInt32(instruction.op_macro.param)), m_cyclesSlot);
				return true;
			case IMLOperation::MACRO_BL:
			{
				// the native backends write LR after all registers were written back, do the same
				const uint32 newLR = instruction.op_macro.param + 4;
				bool lrMapped = false;
				for (auto& [regId, slot] : m_slots)
				{
					if (slot.isWritten && slot.location.offset == offsetof(PPCInterpreter_t, spr.LR) && !slot.location.isFloat)
					{
						m_builder.CreateStore(m_builder.getInt64(newLR), slot.slot);
						lrMapped = true;
					}
				}
				if (!lrMapped)
					m_builder.CreateStore(m_builder.getInt32(newLR), ContextPointer(offsetof(PPCInterpreter_t, spr.LR)));
				DirectCall(instruction.op_macro.param2, newLR);
				exits = true;
				return true;
			}
			case IMLOperation::MACRO_B_FAR:
				Exit(m_builder.getInt32(instruction.op_macro.param2));
				exits = true;
				return true;
			case IMLOperation::MACRO_B_TO_REG:
			{
				if (!HasSlots({instruction.op_macro.paramReg}))
					return Reject("missing register");
				uint32 exitKind = EXIT_JUMP;
				if ((IMLBranchToRegKind)instruction.op_macro.param == IMLBranchToRegKind::Call)
					exitKind = EXIT_CALL;
				else if ((IMLBranchToRegKind)instruction.op_macro.param == IMLBranchToRegKind::Return)
					exitKind = EXIT_RETURN;
				Exit(m_builder.CreateOr(m_builder.CreateAnd(GPR32(instruction.op_macro.paramReg), ~3u), exitKind));
				exits = true;
				return true;
			}
			case IMLOperation::MACRO_LEAVE:
				Exit(m_builder.getInt32(instruction.op_macro.param | EXIT_LEAVE));
				exits = true;
				return true;
			default:
				// HLE calls can switch to another thread and stay with the native backend for now
				return Reject("unsupported macro");
			}
		}

		bool TranslateInstruction(IMLInstruction& instruction, llvm::Value*& flagsA, llvm::Value*& flagsB)
		{
			switch (instruction.type)
			{
			case IMLType::R_R:
				return TranslateRR(instruction, flagsA, flagsB);
			case IMLType::R_S32:
				return TranslateRS32(instruction, flagsA, flagsB);
			case IMLType::R_R_S32:
				return TranslateRRS32(instruction);
			case IMLType::R_R_S32_CARRY:
				return TranslateCarry(instruction.operation, instruction.op_r_r_s32_carry.regR, GPR32(instruction.op_r_r_s32_carry.regA),
									  m_builder.getInt32((uint32)instruction.op_r_r_s32_carry.immS32), instruction.op_r_r_s32_carry.regCarry);
			case IMLType::R_R_R:
				return TranslateRRR(instruction);
			case IMLType::R_R_R_CARRY:
				return TranslateCarry(instruction.operation, instruction.op_r_r_r_carry.regR, GPR32(instruction.op_r_r_r_carry.regA),
									  GPR32(instruction.op_r_r_r_carry.regB), instruction.op_r_r_r_carry.regCarry);
			case IMLType::COMPARE:
			{
				// a following X86_EFLAGS_JCC can use the flags of this compare when the bool result is needed elsewhere too
				flagsA = GPR32(instruction.op_compare.regA);
				flagsB = GPR32(instruction.op_compare.regB);
				llvm::Value* result = Compare(instruction.op_compare.cond, flagsA, flagsB);
				if (!result)
					return Reject("unsupported condition");
				SetGPR(instruction.op_compare.regR, result);
				return true;
			}
			case IMLType::COMPARE_S32:
			{
				flagsA = GPR32(instruction.op_compare_s32.regA);
				flagsB = m_builder.getInt32((uint32)instruction.op_compare_s32.immS32);
				llvm::Value* result = Compare(instruction.op_compare_s32.cond, flagsA, flagsB);
				if (!result)
					return Reject("unsupported condition");
				SetGPR(instruction.op_compare_s32.regR, result);
				return true;
			}
			case IMLType::LOAD:
			case IMLType::LOAD_INDEXED:
				return TranslateLoad(instruction, instruction.type == IMLType::LOAD_INDEXED);
			case IMLType::STORE:
			case IMLType::STORE_INDEXED:
				return TranslateStore(instruction, instruction.type == IMLType::STORE_INDEXED);
			case IMLType::FPR_LOAD:
			case IMLType::FPR_LOAD_INDEXED:
				return TranslateFPRLoad(instruction, instruction.type == IMLType::FPR_LOAD_INDEXED);
			case IMLType::FPR_STORE:
			case IMLType::FPR_STORE_INDEXED:
				return TranslateFPRStore(instruction, instruction.type == IMLType::FPR_STORE_INDEXED);
			case IMLType::FPR_R_R:
				return TranslateFPRRR(instruction);
			case IMLType::FPR_R_R_R:
				return TranslateFPRRRR(instruction);
			case IMLType::FPR_R_R_R_R:
			{
				if (instruction.operation != IMLOperation::OP_FPR_SELECT)
					return Reject("unsupported float operation");
				llvm::Value* isNotNegative = m_builder.CreateFCmpOGE(FPR(instruction.op_fpr_r_r_r_r.regA), llvm::ConstantFP::get(m_f64, 0.0));
				SetFPR(instruction.op_fpr_r_r_r_r.regR, m_builder.CreateSelect(isNotNegative, FPR(instruction.op_fpr_r_r_r_r.regC), FPR(instruction.op_fpr_r_r_r_r.regB)));
				return true;
			}
			case IMLType::FPR_R:
				return TranslateFPRR(instruction);
			case IMLType::FPR_COMPARE:
			{
				llvm::Value* a = FPR(instruction.op_fpr_compare.regA);
				llvm::Value* b = FPR(instruction.op_fpr_compare.regB);
				llvm::Value* result;
				switch (instruction.op_fpr_compare.cond)
				{
				case IMLCondition::UNORDERED_GT: result = m_builder.CreateFCmpOGT(a, b); break;
				case IMLCondition::UNORDERED_LT: result = m_builder.CreateFCmpOLT(a, b); break;
				case IMLCondition::UNORDERED_EQ: result = m_builder.CreateFCmpOEQ(a, b); break;
				case IMLCondition::UNORDERED_U: result = m_builder.CreateFCmpUNO(a, b); break;
				default: return Reject("unsupported float condition");
				}
				SetGPR(instruction.op_fpr_compare.regR, result);
				return true;
			}
			case IMLType::ATOMIC_CMP_STORE:
			{
				auto& op = instruction.op_atomic_compare_store;
				llvm::Value* pointer = m_builder.CreateInBoundsGEP(m_i8, m_memoryBase, m_builder.CreateZExt(GPR32(op.regEA), m_i64));
				auto* exchange = m_builder.CreateAtomicCmpXchg(pointer, GPR32(op.regCompareValue), GPR32(op.regWriteValue), llvm::MaybeAlign(4),
															   llvm::AtomicOrdering::SequentiallyConsistent, llvm::AtomicOrdering::SequentiallyConsistent);
				SetGPR(op.regBoolOut, m_builder.CreateExtractValue(exchange, 1));
				return true;
			}
			case IMLType::CALL_IMM:
				return TranslateCall(instruction);
			default:
				return Reject("unsupported instruction type");
			}
		}

		bool TranslateRR(IMLInstruction& instruction, llvm::Value*& flagsA, llvm::Value*& flagsB)
		{
			IMLReg regR = instruction.op_r_r.regR;
			IMLReg regA = instruction.op_r_r.regA;
			switch (instruction.operation)
			{
			case IMLOperation::OP_ASSIGN:
				SetGPR(regR, GPR32(regA));
				return true;
			case IMLOperation::OP_ENDIAN_SWAP:
				SetGPR(regR, ByteSwap(GPR32(regA)));
				return true;
			case IMLOperation::OP_ASSIGN_S8_TO_S32:
				SetGPR(regR, m_builder.CreateSExt(m_builder.CreateTrunc(GPR32(regA), m_i8), m_i32));
				return true;
			case IMLOperation::OP_ASSIGN_S16_TO_S32:
				SetGPR(regR, m_builder.CreateSExt(m_builder.CreateTrunc(GPR32(regA), m_i16), m_i32));
				return true;
			case IMLOperation::OP_NOT:
				SetGPR(regR, m_builder.CreateNot(GPR32(regA)));
				return true;
			case IMLOperation::OP_NEG:
				SetGPR(regR, m_builder.CreateNeg(GPR32(regA)));
				return true;
			case IMLOperation::OP_CNTLZW:
				SetGPR(regR, m_builder.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, GPR32(regA), m_builder.getFalse()));
				return true;
			case IMLOperation::OP_X86_CMP:
				flagsA = GPR32(regR);
				flagsB = GPR32(regA);
				return true;
			default:
				return Reject("unsupported r_r operation");
			}
		}

		bool TranslateRS32(IMLInstruction& instruction, llvm::Value*& flagsA, llvm::Value*& flagsB)
		{
			IMLReg regR = instruction.op_r_immS32.regR;
			const uint32 imm = (uint32)instruction.op_r_immS32.immS32;
			switch (instruction.operation)
			{
			case IMLOperation::OP_ASSIGN:
				SetGPR(regR, m_builder.getInt32(imm));
				return true;
			case IMLOperation::OP_LEFT_ROTATE:
			{
				llvm::Value* value = GPR32(regR);
				SetGPR(regR, m_builder.CreateIntrinsic(llvm::Intrinsic::fshl, {m_i32}, {value, value, m_builder.getInt32(imm & 31)}));
				return true;
			}
			case IMLOperation::OP_X86_CMP:
				flagsA = GPR32(regR);
				flagsB = m_builder.getInt32(imm);
				return true;
			default:
				return Reject("unsupported r_s32 operation");
			}
		}

		bool TranslateRRS32(IMLInstruction& instruction)
		{
			IMLReg regR = instruction.op_r_r_s32.regR;
			llvm::Value* a = GPR32(instruction.op_r_r_s32.regA);
			const uint32 imm = (uint32)instruction.op_r_r_s32.immS32;
			llvm::Value* b = m_builder.getInt32(imm);
			switch (instruction.operation)
			{
			case IMLOperation::OP_ADD: SetGPR(regR, m_builder.CreateAdd(a, b)); return true;
			case IMLOperation::OP_SUB: SetGPR(regR, m_builder.CreateSub(a, b)); return true;
			case IMLOperation::OP_AND: SetGPR(regR, m_builder.CreateAnd(a, b)); return true;
			case IMLOperation::OP_OR: SetGPR(regR, m_builder.CreateOr(a, b)); return true;
			case IMLOperation::OP_XOR: SetGPR(regR, m_builder.CreateXor(a, b)); return true;
			case IMLOperation::OP_MULTIPLY_SIGNED: SetGPR(regR, m_builder.CreateMul(a, b)); return true;
			case IMLOperation::OP_LEFT_SHIFT: SetGPR(regR, m_builder.CreateShl(a, imm & 31)); return true;
			case IMLOperation::OP_RIGHT_SHIFT_U: SetGPR(regR, m_builder.CreateLShr(a, imm & 31)); return true;
			case IMLOperation::OP_RIGHT_SHIFT_S: SetGPR(regR, m_builder.CreateAShr(a, imm & 31)); return true;
			default: return Reject("unsupported r_r_s32 operation");
			}
		}

		// carry is the carry out of the unsigned 32 bit addition, a set carry register adds one
		bool TranslateCarry(IMLOperation operation, IMLReg regR, llvm::Value* a, llvm::Value* b, IMLReg regCarry)
		{
			llvm::Value* sum = m_builder.CreateAdd(m_builder.CreateZExt(a, m_i64), m_builder.CreateZExt(b, m_i64));
			if (operation == IMLOperation::OP_ADD_WITH_CARRY)
				sum = m_builder.CreateAdd(sum, m_builder.CreateZExt(m_builder.CreateICmpNE(GPR32(regCarry), m_builder.getInt32(0)), m_i64));
			else if (operation != IMLOperation::OP_ADD)
				return Reject("unsupported carry operation");
			SetGPR(regR, m_builder.CreateTrunc(sum, m_i32));
			SetGPR(regCarry, m_builder.CreateTrunc(m_builder.CreateLShr(sum, 32), m_i32));
			return true;
		}

		bool TranslateRRR(IMLInstruction& instruction)
		{
			IMLReg regR = instruction.op_r_r_r.regR;
			llvm::Value* a = GPR32(instruction.op_r_r_r.regA);
			llvm::Value* b = GPR32(instruction.op_r_r_r.regB);
			// register shift amounts are taken modulo 32 like on AArch64
			llvm::Value* shift = m_builder.CreateAnd(b, 31);
			switch (instruction.operation)
			{
			case IMLOperation::OP_ADD: SetGPR(regR, m_builder.CreateAdd(a, b)); return true;
			case IMLOperation::OP_SUB: SetGPR(regR, m_builder.CreateSub(a, b)); return true;
			case IMLOperation::OP_OR: SetGPR(regR, m_builder.CreateOr(a, b)); return true;
			case IMLOperation::OP_AND: SetGPR(regR, m_builder.CreateAnd(a, b)); return true;
			case IMLOperation::OP_XOR:
				// the IML doesn't count this as a read of the register, it may not have been loaded
				if (instruction.op_r_r_r.regA == instruction.op_r_r_r.regB)
					SetGPR(regR, m_builder.getInt32(0));
				else
					SetGPR(regR, m_builder.CreateXor(a, b));
				return true;
			case IMLOperation::OP_MULTIPLY_SIGNED: SetGPR(regR, m_builder.CreateMul(a, b)); return true;
			case IMLOperation::OP_LEFT_SHIFT: SetGPR(regR, m_builder.CreateShl(a, shift)); return true;
			case IMLOperation::OP_RIGHT_SHIFT_U: SetGPR(regR, m_builder.CreateLShr(a, shift)); return true;
			case IMLOperation::OP_RIGHT_SHIFT_S: SetGPR(regR, m_builder.CreateAShr(a, shift)); return true;
			case IMLOperation::OP_SLW:
			case IMLOperation::OP_SRW:
			{
				// shift amounts of 32 to 63 give zero
				llvm::Value* shifted = instruction.operation == IMLOperation::OP_SLW ? m_builder.CreateShl(a, shift) : m_builder.CreateLShr(a, shift);
				llvm::Value* tooFar = m_builder.CreateICmpNE(m_builder.CreateAnd(b, 32), m_builder.getInt32(0));
				SetGPR(regR, m_builder.CreateSelect(tooFar, m_builder.getInt32(0), shifted));
				return true;
			}
			case IMLOperation::OP_LEFT_ROTATE:
				SetGPR(regR, m_builder.CreateIntrinsic(llvm::Intrinsic::fshl, {m_i32}, {a, a, b}));
				return true;
			case IMLOperation::OP_DIVIDE_SIGNED:
			{
				// divw gives the sign of the dividend for a zero divisor and -1 on overflow
				llvm::Value* divisorZero = m_builder.CreateICmpEQ(b, m_builder.getInt32(0));
				llvm::Value* overflow = m_builder.CreateAnd(m_builder.CreateICmpEQ(b, m_builder.getInt32(0xFFFFFFFF)), m_builder.CreateICmpEQ(a, m_builder.getInt32(0x80000000)));
				llvm::Value* safeDivisor = m_builder.CreateSelect(m_builder.CreateOr(divisorZero, overflow), m_builder.getInt32(1), b);
				llvm::Value* quotient = m_builder.CreateSDiv(a, safeDivisor);
				quotient = m_builder.CreateSelect(overflow, m_builder.getInt32(0xFFFFFFFF), quotient);
				quotient = m_builder.CreateSelect(divisorZero, m_builder.CreateAShr(a, 31), quotient);
				SetGPR(regR, quotient);
				return true;
			}
			case IMLOperation::OP_DIVIDE_UNSIGNED:
			{
				llvm::Value* divisorZero = m_builder.CreateICmpEQ(b, m_builder.getInt32(0));
				llvm::Value* quotient = m_builder.CreateUDiv(a, m_builder.CreateSelect(divisorZero, m_builder.getInt32(1), b));
				SetGPR(regR, m_builder.CreateSelect(divisorZero, m_builder.getInt32(0), quotient));
				return true;
			}
			case IMLOperation::OP_MULTIPLY_HIGH_SIGNED:
			case IMLOperation::OP_MULTIPLY_HIGH_UNSIGNED:
			{
				const bool isSigned = instruction.operation == IMLOperation::OP_MULTIPLY_HIGH_SIGNED;
				llvm::Value* a64 = isSigned ? m_builder.CreateSExt(a, m_i64) : m_builder.CreateZExt(a, m_i64);
				llvm::Value* b64 = isSigned ? m_builder.CreateSExt(b, m_i64) : m_builder.CreateZExt(b, m_i64);
				SetGPR(regR, m_builder.CreateTrunc(m_builder.CreateLShr(m_builder.CreateMul(a64, b64), 32), m_i32));
				return true;
			}
			default:
				return Reject("unsupported r_r_r operation");
			}
		}

		bool TranslateLoad(IMLInstruction& instruction, bool indexed)
		{
			auto& op = instruction.op_storeLoad;
			GuestAddress pointer = GuestPointer(op.registerMem, op.immS32, indexed ? &op.registerMem2 : nullptr);
			llvm::Value* value;
			switch (op.copyWidth)
			{
			case 32:
				value = LoadGuest(m_i32, pointer);
				if (op.flags2.swapEndian)
					value = ByteSwap(value);
				break;
			case 16:
				value = LoadGuest(m_i16, pointer);
				if (op.flags2.swapEndian)
					value = ByteSwap(value);
				value = op.flags2.signExtend ? m_builder.CreateSExt(value, m_i32) : m_builder.CreateZExt(value, m_i32);
				break;
			case 8:
				value = LoadGuest(m_i8, pointer);
				value = op.flags2.signExtend ? m_builder.CreateSExt(value, m_i32) : m_builder.CreateZExt(value, m_i32);
				break;
			default:
				return Reject("unsupported load width");
			}
			SetGPR(op.registerData, value);
			return true;
		}

		bool TranslateStore(IMLInstruction& instruction, bool indexed)
		{
			auto& op = instruction.op_storeLoad;
			llvm::Value* value = GPR32(op.registerData);
			GuestAddress pointer = GuestPointer(op.registerMem, op.immS32, indexed ? &op.registerMem2 : nullptr);
			switch (op.copyWidth)
			{
			case 32:
				break;
			case 16:
				value = m_builder.CreateTrunc(value, m_i16);
				break;
			case 8:
				value = m_builder.CreateTrunc(value, m_i8);
				break;
			default:
				return Reject("unsupported store width");
			}
			if (op.flags2.swapEndian && op.copyWidth != 8)
				value = ByteSwap(value);
			StoreGuest(value, pointer);
			return true;
		}

		bool TranslateFPRLoad(IMLInstruction& instruction, bool indexed)
		{
			auto& op = instruction.op_storeLoad;
			GuestAddress pointer = GuestPointer(op.registerMem, op.immS32, indexed ? &op.registerMem2 : nullptr);
			if (op.mode == PPCREC_FPR_LD_MODE_SINGLE)
			{
				llvm::Value* bits = ByteSwap(LoadGuest(m_i32, pointer));
				if (op.flags2.notExpanded)
					SetFPR(op.registerData, m_builder.CreateBitCast(m_builder.CreateZExt(bits, m_i64), m_f64)); // kept as single in the low bits
				else
					SetFPR(op.registerData, m_builder.CreateFPExt(m_builder.CreateBitCast(bits, m_f32), m_f64));
				return true;
			}
			if (op.mode == PPCREC_FPR_LD_MODE_DOUBLE)
			{
				SetFPR(op.registerData, m_builder.CreateBitCast(ByteSwap(LoadGuest(m_i64, pointer)), m_f64));
				return true;
			}
			return Reject("unsupported float load mode");
		}

		llvm::Value* LowBits32(llvm::Value* doubleValue)
		{
			return m_builder.CreateTrunc(m_builder.CreateBitCast(doubleValue, m_i64), m_i32);
		}

		// stfs truncates the mantissa instead of rounding, same as the native backend
		llvm::Value* TruncateToSingleBits(llvm::Value* value)
		{
			llvm::Value* bits = m_builder.CreateBitCast(value, m_i64);
			llvm::Value* exponent = m_builder.CreateTrunc(m_builder.CreateAnd(m_builder.CreateLShr(bits, 52), 0x7FF), m_i32);
			llvm::Value* isDenormal = m_builder.CreateICmpULT(m_builder.CreateSub(exponent, m_builder.getInt32(874)), m_builder.getInt32(23));
			// normal numbers: sign and top exponent bit, then the remaining 30 bits
			llvm::Value* top = m_builder.CreateTrunc(m_builder.CreateLShr(bits, 62), m_i32);
			llvm::Value* rest = m_builder.CreateTrunc(m_builder.CreateAnd(m_builder.CreateLShr(bits, 29), 0x3FFFFFFF), m_i32);
			llvm::Value* normal = m_builder.CreateOr(rest, m_builder.CreateShl(top, 30));
			// denormals: scale before extracting the mantissa
			llvm::Value* scaled = m_builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, m_builder.CreateFMul(value, llvm::ConstantFP::get(m_f64, 0x1p149)));
			llvm::Value* mantissa = m_builder.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, {m_i32, m_f64}, {scaled});
			llvm::Value* sign = m_builder.CreateTrunc(m_builder.CreateLShr(bits, 63), m_i32);
			llvm::Value* denormal = m_builder.CreateOr(mantissa, m_builder.CreateShl(sign, 31));
			return m_builder.CreateSelect(isDenormal, denormal, normal);
		}

		bool TranslateFPRStore(IMLInstruction& instruction, bool indexed)
		{
			auto& op = instruction.op_storeLoad;
			llvm::Value* value = FPR(op.registerData);
			llvm::Value* bits;
			switch (op.mode)
			{
			case PPCREC_FPR_ST_MODE_SINGLE:
				bits = op.flags2.notExpanded ? LowBits32(value) : TruncateToSingleBits(value);
				break;
			case PPCREC_FPR_ST_MODE_SINGLE_FTZ:
				bits = op.flags2.notExpanded ? LowBits32(value) : m_builder.CreateBitCast(m_builder.CreateFPTrunc(value, m_f32), m_i32);
				break;
			case PPCREC_FPR_ST_MODE_DOUBLE:
				bits = m_builder.CreateBitCast(value, m_i64);
				break;
			case PPCREC_FPR_ST_MODE_UI32_FROM_PS0:
				bits = LowBits32(value);
				break;
			default:
				return Reject("unsupported float store mode");
			}
			StoreGuest(ByteSwap(bits), GuestPointer(op.registerMem, op.immS32, indexed ? &op.registerMem2 : nullptr));
			return true;
		}

		bool TranslateFPRRR(IMLInstruction& instruction)
		{
			IMLReg regR = instruction.op_fpr_r_r.regR;
			IMLReg regA = instruction.op_fpr_r_r.regA;
			switch (instruction.operation)
			{
			case IMLOperation::OP_FPR_FLOAT_TO_INT:
				SetGPR(regR, m_builder.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, {m_i32, m_f64}, {FPR(regA)}));
				return true;
			case IMLOperation::OP_FPR_INT_TO_FLOAT:
				SetFPR(regR, m_builder.CreateSIToFP(GPR32(regA), m_f64));
				return true;
			case IMLOperation::OP_FPR_BITCAST_INT_TO_FLOAT:
				if (regA.GetRegFormat() == IMLRegFormat::I32)
					SetFPR(regR, m_builder.CreateFPExt(m_builder.CreateBitCast(GPR32(regA), m_f32), m_f64));
				else if (regA.GetRegFormat() == IMLRegFormat::I64)
					SetFPR(regR, m_builder.CreateBitCast(GPR(regA), m_f64));
				else
					return Reject("unsupported bitcast");
				return true;
			case IMLOperation::OP_FPR_ASSIGN: SetFPR(regR, FPR(regA)); return true;
			case IMLOperation::OP_FPR_MULTIPLY: SetFPR(regR, m_builder.CreateFMul(FPR(regR), FPR(regA))); return true;
			case IMLOperation::OP_FPR_DIVIDE: SetFPR(regR, m_builder.CreateFDiv(FPR(regR), FPR(regA))); return true;
			case IMLOperation::OP_FPR_ADD: SetFPR(regR, m_builder.CreateFAdd(FPR(regR), FPR(regA))); return true;
			case IMLOperation::OP_FPR_SUB: SetFPR(regR, m_builder.CreateFSub(FPR(regR), FPR(regA))); return true;
			case IMLOperation::OP_FPR_FCTIWZ:
				// the native backend converts to a 64 bit integer, keep that
				SetFPR(regR, m_builder.CreateBitCast(m_builder.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, {m_i64, m_f64}, {FPR(regA)}), m_f64));
				return true;
			default:
				return Reject("unsupported float operation");
			}
		}

		bool TranslateFPRRRR(IMLInstruction& instruction)
		{
			llvm::Value* a = FPR(instruction.op_fpr_r_r_r.regA);
			llvm::Value* b = FPR(instruction.op_fpr_r_r_r.regB);
			switch (instruction.operation)
			{
			case IMLOperation::OP_FPR_MULTIPLY: SetFPR(instruction.op_fpr_r_r_r.regR, m_builder.CreateFMul(a, b)); return true;
			case IMLOperation::OP_FPR_ADD: SetFPR(instruction.op_fpr_r_r_r.regR, m_builder.CreateFAdd(a, b)); return true;
			case IMLOperation::OP_FPR_SUB: SetFPR(instruction.op_fpr_r_r_r.regR, m_builder.CreateFSub(a, b)); return true;
			default: return Reject("unsupported float operation");
			}
		}

		bool TranslateFPRR(IMLInstruction& instruction)
		{
			IMLReg regR = instruction.op_fpr_r.regR;
			switch (instruction.operation)
			{
			case IMLOperation::OP_FPR_NEGATE:
				SetFPR(regR, m_builder.CreateFNeg(FPR(regR)));
				return true;
			case IMLOperation::OP_FPR_LOAD_ONE:
				SetFPR(regR, llvm::ConstantFP::get(m_f64, 1.0));
				return true;
			case IMLOperation::OP_FPR_ABS:
				SetFPR(regR, m_builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, FPR(regR)));
				return true;
			case IMLOperation::OP_FPR_NEGATIVE_ABS:
				SetFPR(regR, m_builder.CreateFNeg(m_builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, FPR(regR))));
				return true;
			case IMLOperation::OP_FPR_ROUND_TO_SINGLE_PRECISION_BOTTOM:
				SetFPR(regR, m_builder.CreateFPExt(m_builder.CreateFPTrunc(FPR(regR), m_f32), m_f64));
				return true;
			case IMLOperation::OP_FPR_EXPAND_F32_TO_F64:
				SetFPR(regR, m_builder.CreateFPExt(m_builder.CreateBitCast(LowBits32(FPR(regR)), m_f32), m_f64));
				return true;
			default:
				return Reject("unsupported float operation");
			}
		}

		bool TranslateCall(IMLInstruction& instruction)
		{
			auto& op = instruction.op_call_imm;
			std::vector<llvm::Type*> parameterTypes;
			std::vector<llvm::Value*> arguments;
			for (const IMLReg& reg : {op.regParam0, op.regParam1, op.regParam2})
			{
				if (!reg.IsValid())
					break;
				const bool isFloat = reg.GetBaseFormat() == IMLRegFormat::F64;
				parameterTypes.emplace_back(isFloat ? m_f64 : RegType(reg));
				arguments.emplace_back(isFloat ? FPR(reg) : GPR(reg));
			}
			llvm::Type* returnType = m_builder.getVoidTy();
			if (op.regReturn.IsValid())
				returnType = op.regReturn.GetBaseFormat() == IMLRegFormat::F64 ? m_f64 : RegType(op.regReturn);
			// called by name, the addresses change with every start of the app and translations are cached
			const char* helperName = GetHelperName(op.callAddress);
			if (!helperName)
				return Reject("unknown helper function");
			auto callee = m_module.getOrInsertFunction(helperName, llvm::FunctionType::get(returnType, parameterTypes, false));
			llvm::Value* result = m_builder.CreateCall(callee, arguments);
			if (op.regReturn.IsValid())
			{
				if (op.regReturn.GetBaseFormat() == IMLRegFormat::F64)
					SetFPR(op.regReturn, result);
				else
					SetGPR(op.regReturn, result);
			}
			return true;
		}

		ppcImlGenContext_t& m_iml;
		llvm::LLVMContext& m_context;
		llvm::Module& m_module;
		llvm::IRBuilder<> m_builder;
		llvm::Type *m_i8, *m_i16, *m_i32, *m_i64, *m_f32, *m_f64, *m_ptr;
		llvm::Function* m_function{};
		llvm::Value* m_hCPU{};
		llvm::Value* m_memoryBase{};
		llvm::BasicBlock* m_exitBlock{};
		llvm::PHINode* m_exitAddress{};
		llvm::AllocaInst* m_cyclesSlot{};
		std::unordered_map<IMLSegment*, llvm::BasicBlock*> m_segmentBlocks;
		std::unordered_map<uint32, llvm::BasicBlock*> m_entryLoadBlocks; // by guest address
		std::unordered_map<IMLRegID, Slot> m_slots;
		std::optional<IMLRegID> m_stackPointerRegId;
		const char* m_rejectReason = "";
	};

	// Puts all sections of a translated function into shared memory that is writable and executable, so the functions are
	// packed densely like the code of the native backend instead of each one taking separate pages. Memory is never reused.
	class PackedMemoryManager : public llvm::jitlink::JITLinkMemoryManager
	{
		static constexpr size_t BLOCK_SIZE = 4 * 1024 * 1024;
		static constexpr size_t MIN_ALIGNMENT = 64; // cache line

		class Allocation : public InFlightAlloc
		{
		  public:
			Allocation(std::vector<std::pair<uint8*, size_t>> ranges, llvm::orc::shared::AllocActions actions)
				: m_ranges(std::move(ranges)), m_actions(std::move(actions)) {}

			void finalize(OnFinalizedFunction onFinalized) override
			{
				for (auto& [address, size] : m_ranges)
					llvm::sys::Memory::InvalidateInstructionCache(address, size);
				auto deallocActions = llvm::orc::shared::runFinalizeActions(m_actions);
				if (!deallocActions)
				{
					onFinalized(deallocActions.takeError());
					return;
				}
				onFinalized(FinalizedAlloc(llvm::orc::ExecutorAddr::fromPtr(m_ranges.empty() ? (void*)this : m_ranges.front().first)));
			}

			void abandon(OnAbandonedFunction onAbandoned) override
			{
				onAbandoned(llvm::Error::success());
			}

		  private:
			std::vector<std::pair<uint8*, size_t>> m_ranges;
			llvm::orc::shared::AllocActions m_actions;
		};

	  public:
		void allocate(const llvm::jitlink::JITLinkDylib* dylib, llvm::jitlink::LinkGraph& graph, OnAllocatedFunction onAllocated) override
		{
			std::unique_lock lock(m_mutex);
			llvm::jitlink::BasicLayout layout(graph);
			std::vector<std::pair<uint8*, size_t>> ranges;
			for (auto& [group, segment] : layout.segments())
			{
				const size_t size = segment.ContentSize + segment.ZeroFillSize;
				uint8* memory = Reserve(size, std::max<size_t>(segment.Alignment.value(), MIN_ALIGNMENT));
				if (!memory)
				{
					onAllocated(llvm::make_error<llvm::StringError>("Can't map executable memory", llvm::inconvertibleErrorCode()));
					return;
				}
				segment.Addr = llvm::orc::ExecutorAddr::fromPtr(memory);
				segment.WorkingMem = reinterpret_cast<char*>(memory);
				ranges.emplace_back(memory, size);
				if ((static_cast<uint32>(group.getMemProt()) & static_cast<uint32>(llvm::orc::MemProt::Exec)) != 0)
					m_lastCode = {memory, size};
			}
			if (auto error = layout.apply())
			{
				onAllocated(std::move(error));
				return;
			}
			onAllocated(std::make_unique<Allocation>(std::move(ranges), std::move(layout.graphAllocActions())));
		}

		void deallocate(std::vector<FinalizedAlloc> allocations, OnDeallocatedFunction onDeallocated) override
		{
			for (auto& allocation : allocations)
				allocation.release();
			onDeallocated(llvm::Error::success());
		}

		std::pair<uint8*, size_t> GetLastCode()
		{
			std::unique_lock lock(m_mutex);
			return m_lastCode;
		}

	  private:
		uint8* Reserve(size_t size, size_t alignment)
		{
			size_t offset = (m_blockUsed + alignment - 1) & ~(alignment - 1);
			if (!m_block || offset + size > m_blockSize)
			{
				const size_t blockSize = std::max(BLOCK_SIZE, size + alignment);
				void* block = mmap(nullptr, blockSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (block == MAP_FAILED)
					return nullptr;
				m_block = static_cast<uint8*>(block);
				m_blockSize = blockSize;
				offset = 0;
			}
			m_blockUsed = offset + size;
			return m_block + offset;
		}

		std::mutex m_mutex;
		uint8* m_block = nullptr;
		size_t m_blockUsed = 0;
		size_t m_blockSize = 0;
		std::pair<uint8*, size_t> m_lastCode{};
	};

	constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ull;

	uint64 HashBytes(uint64 hash, const void* data, size_t size)
	{
		const uint8* bytes = static_cast<const uint8*>(data);
		for (size_t i = 0; i < size; i++)
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;
		return hash;
	}

	// The build hash only changes with commits, bump this whenever the translation changes
	constexpr uint32 CACHE_VERSION = 4;
	constexpr uint32 CACHE_MAGIC = 0x564c4c43; // CLLV

	// the IML of a function only depends on its guest code and entry points
	uint64 ComputeCacheKey(const ppcImlGenContext_t& ppcImlGenContext)
	{
		uint64 hash = HashBytes(FNV_OFFSET_BASIS, &CACHE_VERSION, sizeof(CACHE_VERSION));
		for (const PPCFunctionBoundaryTracker::PPCRange_t& range : ppcImlGenContext.boundaryTracker->GetRanges())
		{
			hash = HashBytes(hash, &range.startAddress, sizeof(range.startAddress));
			hash = HashBytes(hash, &range.length, sizeof(range.length));
			hash = HashBytes(hash, memory_getPointerFromVirtualOffset(range.startAddress), range.length);
		}
		for (IMLSegment* segment : ppcImlGenContext.segmentList2)
		{
			if (segment->isEnterable)
				hash = HashBytes(hash, &segment->enterPPCAddress, sizeof(segment->enterPPCAddress));
		}
		return hash;
	}

	// Object files of translated functions, stored per title so they only have to be translated once.
	// Each record is key, size, checksum and the object file. Records are only appended.
	class ObjectCache
	{
	  public:
		void Open(TitleId titleId)
		{
			if (m_isOpen && m_titleId == titleId)
				return;
			m_isOpen = true;
			m_titleId = titleId;
			m_data.clear();
			m_objects.clear();
			m_file.reset();

			std::error_code ec;
			fs::create_directories(ActiveSettings::GetCachePath("llvm"), ec);
			const fs::path path = ActiveSettings::GetCachePath("llvm/{:016x}.bin", titleId);
			const uint64 buildHash = HashBytes(FNV_OFFSET_BASIS, BUILD_VERSION_STRING, strlen(BUILD_VERSION_STRING));
			size_t validSize = 0;
			if (auto data = FileStream::LoadIntoMemory(path))
			{
				m_data = std::move(*data);
				validSize = Parse(buildHash);
			}
			if (validSize == 0)
			{
				m_data.clear();
				m_objects.clear();
				m_file.reset(FileStream::createFile2(path));
				if (!m_file)
					return;
				m_file->writeU32(CACHE_MAGIC);
				m_file->writeU32(CACHE_VERSION);
				m_file->writeU64(buildHash);
				m_file->Flush();
				return;
			}
			// drop a record that was cut off when the app was closed while writing it
			if (validSize != m_data.size())
				fs::resize_file(path, validSize, ec);
			m_file.reset(FileStream::openFile2(path, true));
			if (!m_file)
				return;
			m_file->SetPosition(validSize);
			cemuLog_log(LogType::Force, "LLVM: {} cached functions for title {:016x}", m_objects.size(), titleId);
		}

		std::optional<std::span<const uint8>> Find(uint64 key) const
		{
			auto it = m_objects.find(key);
			if (it == m_objects.end())
				return std::nullopt;
			return std::span<const uint8>(m_data.data() + it->second.first, it->second.second);
		}

		void Store(uint64 key, std::span<const uint8> object)
		{
			if (m_objects.contains(key))
				return;
			m_objects[key] = {m_data.size(), object.size()};
			m_data.insert(m_data.end(), object.begin(), object.end());
			if (!m_file)
				return;
			m_file->writeU64(key);
			m_file->writeU32((uint32)object.size());
			m_file->writeU64(HashBytes(FNV_OFFSET_BASIS, object.data(), object.size()));
			m_file->writeData(object.data(), (sint32)object.size());
			m_file->Flush();
		}

	  private:
		// returns the size of the valid part of the file, 0 if it can't be used
		size_t Parse(uint64 buildHash)
		{
			constexpr size_t HEADER_SIZE = 4 + 4 + 8;
			constexpr size_t RECORD_HEADER_SIZE = 8 + 4 + 8;
			if (m_data.size() < HEADER_SIZE)
				return 0;
			uint32 magic, version;
			uint64 fileBuildHash;
			memcpy(&magic, m_data.data(), 4);
			memcpy(&version, m_data.data() + 4, 4);
			memcpy(&fileBuildHash, m_data.data() + 8, 8);
			if (magic != CACHE_MAGIC || version != CACHE_VERSION || fileBuildHash != buildHash)
				return 0;
			size_t offset = HEADER_SIZE;
			while (offset + RECORD_HEADER_SIZE <= m_data.size())
			{
				uint64 key, checksum;
				uint32 size;
				memcpy(&key, m_data.data() + offset, 8);
				memcpy(&size, m_data.data() + offset + 8, 4);
				memcpy(&checksum, m_data.data() + offset + 12, 8);
				const size_t objectOffset = offset + RECORD_HEADER_SIZE;
				if (objectOffset + size > m_data.size() || HashBytes(FNV_OFFSET_BASIS, m_data.data() + objectOffset, size) != checksum)
					break;
				m_objects[key] = {objectOffset, size};
				offset = objectOffset + size;
			}
			return offset;
		}

		bool m_isOpen = false;
		TitleId m_titleId = 0;
		std::vector<uint8> m_data;
		std::unordered_map<uint64, std::pair<size_t, size_t>> m_objects;
		std::unique_ptr<FileStream> m_file;
	};

	struct LLVMBackend
	{
		std::unique_ptr<llvm::orc::LLJIT> jit;
		std::optional<llvm::orc::JITTargetMachineBuilder> targetMachineBuilder;
		PackedMemoryManager* memoryManager;
		// guards everything below, translating new functions happens without holding it
		std::mutex mutex;
		ObjectCache cache;
		struct LoadedFunction
		{
			PPCRecompilerLLVM_Function function;
			const void* code;
			size_t codeSize;
		};
		std::unordered_map<uint64, LoadedFunction> loadedFunctions;
		uint32 translatedCount = 0;
		std::unordered_map<std::string, uint32> rejectReasons;
		double totalMilliseconds = 0.0;
	};

	LLVMBackend* s_backend = nullptr;
	bool s_enabled = false;
	bool s_statsEnabled = false;

	void Initialize()
	{
		std::error_code ec;
		if (!fs::exists(ActiveSettings::GetUserDataPath("llvm.enable"), ec))
			return;
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		auto targetMachineBuilder = llvm::orc::JITTargetMachineBuilder::detectHost();
		if (!targetMachineBuilder)
		{
			cemuLog_log(LogType::Force, "LLVM: Can't detect the host: {}", llvm::toString(targetMachineBuilder.takeError()));
			return;
		}
		targetMachineBuilder->setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
		// the translated code references Cemu's data and functions, which can be anywhere in the address space
		targetMachineBuilder->setRelocationModel(llvm::Reloc::PIC_);
		auto targetMachine = targetMachineBuilder->createTargetMachine();
		if (!targetMachine)
		{
			cemuLog_log(LogType::Force, "LLVM: Can't create the target machine: {}", llvm::toString(targetMachine.takeError()));
			return;
		}
		PackedMemoryManager* memoryManager = nullptr;
		auto jit = llvm::orc::LLJITBuilder()
					   .setJITTargetMachineBuilder(*targetMachineBuilder)
					   .setObjectLinkingLayerCreator([&](llvm::orc::ExecutionSession& session, llvm::jitlink::JITLinkMemoryManager&) -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
						   auto manager = std::make_unique<PackedMemoryManager>();
						   memoryManager = manager.get();
						   return std::make_unique<llvm::orc::ObjectLinkingLayer>(session, std::move(manager));
					   })
					   .create();
		if (!jit)
		{
			cemuLog_log(LogType::Force, "LLVM: Can't create the JIT: {}", llvm::toString(jit.takeError()));
			return;
		}
		// ORC reports the cause of failed lookups here, by default it goes to stderr which isn't visible on Android
		(*jit)->getExecutionSession().setErrorReporter([](llvm::Error error) {
			cemuLog_log(LogType::Force, "LLVM: {}", llvm::toString(std::move(error)));
		});
		llvm::orc::SymbolMap helperSymbols;
		for (const HelperFunction& helper : s_helperFunctions)
			helperSymbols[(*jit)->mangleAndIntern(helper.name)] = {llvm::orc::ExecutorAddr::fromPtr(helper.address), llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
		helperSymbols[(*jit)->mangleAndIntern("g_PPCRecompilerLLVM_generation")] = {llvm::orc::ExecutorAddr::fromPtr(&g_PPCRecompilerLLVM_generation), llvm::JITSymbolFlags::Exported};
		if (auto error = (*jit)->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(helperSymbols))))
		{
			cemuLog_log(LogType::Force, "LLVM: Can't define the helper functions: {}", llvm::toString(std::move(error)));
			return;
		}
		s_backend = new LLVMBackend();
		s_backend->targetMachineBuilder = std::move(*targetMachineBuilder);
		s_backend->jit = std::move(*jit);
		s_backend->memoryManager = memoryManager;
		s_enabled = true;
		cemuLog_log(LogType::Force, "LLVM: Enabled for {} ({})", llvm::sys::getProcessTriple(), llvm::sys::getHostCPUName().str());
		if (fs::exists(ActiveSettings::GetUserDataPath("llvm.stats"), ec))
		{
			s_statsEnabled = true;
			std::thread([]() {
				uint64 previous[LLVM_STAT_COUNT]{};
				while (true)
				{
					std::this_thread::sleep_for(std::chrono::seconds(10));
					uint64 delta[LLVM_STAT_COUNT];
					for (int i = 0; i < LLVM_STAT_COUNT; i++)
					{
						const uint64 value = g_PPCRecompilerLLVM_stats[i];
						delta[i] = value - previous[i];
						previous[i] = value;
					}
					cemuLog_log(LogType::Force, "LLVM stats: {} entries, exits jump {} leave {} call {} return {}, {} resolves ({} translated)",
								delta[LLVM_STAT_ENTRIES], delta[0], delta[1], delta[2], delta[3], delta[LLVM_STAT_RESOLVES], delta[LLVM_STAT_RESOLVED]);
				}
			}).detach();
		}
	}

	// functions are translated on several threads, and a TargetMachine can't be shared between them
	llvm::TargetMachine* GetThreadTargetMachine()
	{
		static thread_local std::unique_ptr<llvm::TargetMachine> s_targetMachine;
		if (!s_targetMachine)
		{
			auto targetMachine = s_backend->targetMachineBuilder->createTargetMachine();
			if (!targetMachine)
			{
				cemuLog_log(LogType::Force, "LLVM: Can't create the target machine: {}", llvm::toString(targetMachine.takeError()));
				return nullptr;
			}
			s_targetMachine = std::move(*targetMachine);
		}
		return s_targetMachine.get();
	}

	void Optimize(llvm::Module& module, llvm::TargetMachine& targetMachine)
	{
		llvm::LoopAnalysisManager loopAnalysisManager;
		llvm::FunctionAnalysisManager functionAnalysisManager;
		llvm::CGSCCAnalysisManager cgsccAnalysisManager;
		llvm::ModuleAnalysisManager moduleAnalysisManager;
		llvm::PassBuilder passBuilder(&targetMachine);
		passBuilder.registerModuleAnalyses(moduleAnalysisManager);
		passBuilder.registerCGSCCAnalyses(cgsccAnalysisManager);
		passBuilder.registerFunctionAnalyses(functionAnalysisManager);
		passBuilder.registerLoopAnalyses(loopAnalysisManager);
		passBuilder.crossRegisterProxies(loopAnalysisManager, functionAnalysisManager, cgsccAnalysisManager, moduleAnalysisManager);
		llvm::ModulePassManager passManager = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
		passManager.run(module, moduleAnalysisManager);
	}
}

uint64 g_PPCRecompilerLLVM_stats[LLVM_STAT_COUNT];

bool PPCRecompilerLLVM_IsStatsEnabled()
{
	return s_statsEnabled;
}

bool PPCRecompilerLLVM_IsEnabled()
{
	static std::once_flag s_initialized;
	std::call_once(s_initialized, Initialize);
	return s_enabled;
}

namespace
{
	bool CompileToObject(llvm::Module& module, llvm::TargetMachine& targetMachine, llvm::SmallVector<char, 0>& object)
	{
		llvm::raw_svector_ostream objectStream(object);
		llvm::legacy::PassManager passManager;
		if (targetMachine.addPassesToEmitFile(passManager, objectStream, nullptr, llvm::CodeGenFileType::ObjectFile))
			return false;
		passManager.run(module);
		return true;
	}

	// expects the backend mutex to be held
	bool LoadObject(uint64 key, std::span<const uint8> object)
	{
		const std::string name = fmt::format("ppc_{:016x}", key);
		auto buffer = llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(reinterpret_cast<const char*>(object.data()), object.size()), name);
		if (auto error = s_backend->jit->addObjectFile(std::move(buffer)))
		{
			cemuLog_log(LogType::Force, "LLVM: Can't load {}: {}", name, llvm::toString(std::move(error)));
			return false;
		}
		auto symbol = s_backend->jit->lookup(name);
		if (!symbol)
		{
			cemuLog_log(LogType::Force, "LLVM: Can't link {}: {}", name, llvm::toString(symbol.takeError()));
			return false;
		}
		auto [code, codeSize] = s_backend->memoryManager->GetLastCode();
		s_backend->loadedFunctions[key] = {symbol->toPtr<PPCRecompilerLLVM_Function>(), code, codeSize};
		return true;
	}

	bool FindLoaded(uint64 key, const ppcImlGenContext_t& ppcImlGenContext, PPCRecompilerLLVM_Result& result)
	{
		auto it = s_backend->loadedFunctions.find(key);
		if (it == s_backend->loadedFunctions.end())
			return false;
		// entry indices follow the order of the enterable segments, like in FunctionTranslator
		result.entryAddresses.clear();
		for (IMLSegment* segment : ppcImlGenContext.segmentList2)
		{
			if (segment->isEnterable)
				result.entryAddresses.emplace_back(segment->enterPPCAddress);
		}
		result.function = it->second.function;
		result.code = it->second.code;
		result.codeSize = it->second.codeSize;
		return true;
	}

	bool Compile(ppcImlGenContext_t& ppcImlGenContext, uint32 functionAddress, uint64 key, llvm::SmallVector<char, 0>& object)
	{
		llvm::TargetMachine* targetMachine = GetThreadTargetMachine();
		if (!targetMachine)
			return false;
		BenchmarkTimer timer;
		timer.Start();
		auto context = std::make_unique<llvm::LLVMContext>();
		auto module = std::make_unique<llvm::Module>("ppc", *context);
		module->setDataLayout(targetMachine->createDataLayout());
		module->setTargetTriple(targetMachine->getTargetTriple());
		FunctionTranslator translator(ppcImlGenContext, *context, *module);
		std::vector<uint32> entryAddresses;
		if (!translator.Translate(fmt::format("ppc_{:016x}", key), entryAddresses))
		{
			std::unique_lock lock(s_backend->mutex);
			s_backend->rejectReasons[translator.GetRejectReason()]++;
			return false;
		}
		Optimize(*module, *targetMachine);
		if (ActiveSettings::DumpRecompilerFunctionsEnabled())
		{
			std::string ir;
			llvm::raw_string_ostream irStream(ir);
			module->print(irStream, nullptr);
			if (FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.ll", functionAddress))))
			{
				fs->writeData(ir.data(), ir.size());
				delete fs;
			}
		}
		if (!CompileToObject(*module, *targetMachine, object))
		{
			cemuLog_log(LogType::Force, "LLVM: Can't emit code for 0x{:08x}", functionAddress);
			return false;
		}
		timer.Stop();

		std::unique_lock lock(s_backend->mutex);
		s_backend->totalMilliseconds += timer.GetElapsedMilliseconds();
		s_backend->translatedCount++;
		if ((s_backend->translatedCount % 1000) == 0)
		{
			std::string rejected;
			for (auto& [reason, count] : s_backend->rejectReasons)
				rejected += fmt::format(" {}: {},", reason, count);
			cemuLog_log(LogType::Force, "LLVM: {} functions translated, {:.1f}ms on average. Left to the native backend:{}",
						s_backend->translatedCount, s_backend->totalMilliseconds / s_backend->translatedCount, rejected);
		}
		return true;
	}
}

bool PPCRecompilerLLVM_LoadCached(ppcImlGenContext_t& ppcImlGenContext, PPCRecompilerLLVM_Result& result)
{
	if (!PPCRecompilerLLVM_IsEnabled())
		return false;
	const uint64 key = ComputeCacheKey(ppcImlGenContext);
	std::unique_lock lock(s_backend->mutex);
	s_backend->cache.Open(CafeSystem::GetForegroundTitleId());
	if (FindLoaded(key, ppcImlGenContext, result))
		return true;
	auto object = s_backend->cache.Find(key);
	return object && LoadObject(key, *object) && FindLoaded(key, ppcImlGenContext, result);
}

void PPCRecompilerLLVM_Prepare(ppcImlGenContext_t& ppcImlGenContext, const PPCRecFunction_t& function)
{
	if (!PPCRecompilerLLVM_IsEnabled())
		return;
	const uint64 key = ComputeCacheKey(ppcImlGenContext);
	{
		std::unique_lock lock(s_backend->mutex);
		s_backend->cache.Open(CafeSystem::GetForegroundTitleId());
		if (s_backend->cache.Find(key))
			return;
	}
	llvm::SmallVector<char, 0> object;
	if (!Compile(ppcImlGenContext, function.ppcAddress, key, object))
		return;
	std::unique_lock lock(s_backend->mutex);
	s_backend->cache.Store(key, std::span<const uint8>(reinterpret_cast<const uint8*>(object.data()), object.size()));
}
