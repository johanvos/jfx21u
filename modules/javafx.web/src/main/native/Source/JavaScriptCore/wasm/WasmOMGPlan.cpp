/*
 * Copyright (C) 2017-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmOMGPlan.h"
#include "JSToWasm.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "JITCompilation.h"
#include "LinkBuffer.h"
#include "NativeCalleeRegistry.h"
#include "WasmCallee.h"
#include "WasmIRGeneratorHelpers.h"
#include "WasmNameSection.h"
#include "WasmOMGIRGenerator.h"
#include "WasmTypeDefinitionInlines.h"
#include <wtf/DataLog.h>
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace Wasm {

namespace WasmOMGPlanInternal {
static constexpr bool verbose = false;
}

OMGPlan::OMGPlan(VM& vm, Ref<Module>&& module, uint32_t functionIndex, std::optional<bool> hasExceptionHandlers, MemoryMode mode, CompletionTask&& task)
    : Base(vm, const_cast<ModuleInformation&>(module->moduleInformation()), WTFMove(task))
    , m_module(WTFMove(module))
    , m_calleeGroup(*m_module->calleeGroupFor(mode))
    , m_hasExceptionHandlers(hasExceptionHandlers)
    , m_functionIndex(functionIndex)
{
    ASSERT(Options::useOMGJIT());
    setMode(mode);
    ASSERT(m_calleeGroup->runnable());
    ASSERT(m_calleeGroup.ptr() == m_module->calleeGroupFor(m_mode));
    dataLogLnIf(WasmOMGPlanInternal::verbose, "Starting OMG plan for ", functionIndex, " of module: ", RawPointer(&m_module.get()));
}

FunctionAllowlist& OMGPlan::ensureGlobalOMGAllowlist()
{
    static LazyNeverDestroyed<FunctionAllowlist> omgAllowlist;
    static std::once_flag initializeAllowlistFlag;
    std::call_once(initializeAllowlistFlag, [] {
        const char* functionAllowlistFile = Options::omgAllowlist();
        omgAllowlist.construct(functionAllowlistFile);
    });
    return omgAllowlist;
}

void OMGPlan::dumpDisassembly(CompilationContext& context, LinkBuffer& linkBuffer, unsigned functionIndex, const TypeDefinition& signature, unsigned functionIndexSpace)
{
    dataLogLnIf(context.procedure->shouldDumpIR() || shouldDumpDisassemblyFor(CompilationMode::OMGMode), "Generated OMG code for WebAssembly OMG function[", functionIndex, "] ", signature.toString().ascii().data(), " name ", makeString(IndexOrName(functionIndexSpace, m_moduleInformation->nameSection->get(functionIndexSpace))).ascii().data());
    if (UNLIKELY(shouldDumpDisassemblyFor(CompilationMode::OMGMode))) {
        auto* disassembler = context.procedure->code().disassembler();

        const char* b3Prefix = "b3    ";
        const char* airPrefix = "Air        ";
        const char* asmPrefix = "asm              ";

        B3::Value* prevOrigin = nullptr;
        auto forEachInst = scopedLambda<void(B3::Air::Inst&)>([&] (B3::Air::Inst& inst) {
            if (inst.origin && inst.origin != prevOrigin && context.procedure->code().shouldPreserveB3Origins()) {
                if (String string = inst.origin->compilerConstructionSite(); !string.isNull())
                    dataLogLn("\033[1;37m", string, "\033[0m");
                dataLog(b3Prefix);
                inst.origin->deepDump(context.procedure.get(), WTF::dataFile());
                dataLogLn();
                prevOrigin = inst.origin;
            }
        });

        disassembler->dump(context.procedure->code(), WTF::dataFile(), linkBuffer, airPrefix, asmPrefix, forEachInst);
        linkBuffer.didAlreadyDisassemble();
    }
}

void OMGPlan::work(CompilationEffort)
{
    ASSERT(m_calleeGroup->runnable());
    ASSERT(m_calleeGroup.ptr() == m_module->calleeGroupFor(mode()));
    const FunctionData& function = m_moduleInformation->functions[m_functionIndex];

    const uint32_t functionIndexSpace = m_functionIndex + m_module->moduleInformation().importFunctionCount();
    ASSERT(functionIndexSpace < m_module->moduleInformation().functionIndexSpaceSize());

    TypeIndex typeIndex = m_moduleInformation->internalFunctionTypeIndices[m_functionIndex];
    const TypeDefinition& signature = TypeInformation::get(typeIndex).expand();

    Ref<OMGCallee> callee = OMGCallee::create(functionIndexSpace, m_moduleInformation->nameSection->get(functionIndexSpace));

    Vector<UnlinkedWasmToWasmCall> unlinkedCalls;
    CompilationContext context;
    auto parseAndCompileResult = parseAndCompileOMG(context, callee.get(), function, signature, unlinkedCalls, m_calleeGroup.get(), m_moduleInformation.get(), m_mode, CompilationMode::OMGMode, m_functionIndex, m_hasExceptionHandlers, UINT32_MAX);

    if (UNLIKELY(!parseAndCompileResult)) {
        Locker locker { m_lock };
        fail(makeString(parseAndCompileResult.error(), "when trying to tier up "_s, m_functionIndex), Plan::Error::Parse);
        return;
    }

    Entrypoint omgEntrypoint;
    LinkBuffer linkBuffer(*context.wasmEntrypointJIT, callee.ptr(), LinkBuffer::Profile::WasmOMG, JITCompilationCanFail);
    if (UNLIKELY(linkBuffer.didFailToAllocate())) {
        Locker locker { m_lock };
        Base::fail(makeString("Out of executable memory while tiering up function at index "_s, m_functionIndex), Plan::Error::OutOfMemory);
        return;
    }

    InternalFunction* internalFunction = parseAndCompileResult->get();
    Vector<CodeLocationLabel<ExceptionHandlerPtrTag>> exceptionHandlerLocations;
    computeExceptionHandlerLocations(exceptionHandlerLocations, internalFunction, context, linkBuffer);

    computePCToCodeOriginMap(context, linkBuffer);

    dumpDisassembly(context, linkBuffer, m_functionIndex, signature, functionIndexSpace);
    omgEntrypoint.compilation = makeUnique<Compilation>(
        FINALIZE_CODE_IF(context.procedure->shouldDumpIR(), linkBuffer, JITCompilationPtrTag, nullptr, "WebAssembly OMG function[%i] %s name %s", m_functionIndex, signature.toString().ascii().data(), makeString(IndexOrName(functionIndexSpace, m_moduleInformation->nameSection->get(functionIndexSpace))).ascii().data()),
        WTFMove(context.wasmEntrypointByproducts));

    omgEntrypoint.calleeSaveRegisters = WTFMove(internalFunction->entrypoint.calleeSaveRegisters);

    CodePtr<WasmEntryPtrTag> entrypoint;
    {
        ASSERT(m_calleeGroup.ptr() == m_module->calleeGroupFor(mode()));
        callee->setEntrypoint(WTFMove(omgEntrypoint), WTFMove(unlinkedCalls), WTFMove(internalFunction->stackmaps), WTFMove(internalFunction->exceptionHandlers), WTFMove(exceptionHandlerLocations));
        entrypoint = callee->entrypoint();

        if (context.pcToCodeOriginMap)
            NativeCalleeRegistry::singleton().addPCToCodeOriginMap(callee.ptr(), WTFMove(context.pcToCodeOriginMap));

        // We want to make sure we publish our callee at the same time as we link our callsites. This enables us to ensure we
        // always call the fastest code. Any function linked after us will see our new code and the new callsites, which they
        // will update. It's also ok if they publish their code before we reset the instruction caches because after we release
        // the lock our code is ready to be published too.
        Locker locker { m_calleeGroup->m_lock };

        m_calleeGroup->setOMGCallee(locker, m_functionIndex, callee.copyRef());

        for (auto& call : callee->wasmToWasmCallsites()) {
            CodePtr<WasmEntryPtrTag> entrypoint;
            Wasm::Callee* calleeCallee = nullptr;
            if (call.functionIndexSpace < m_module->moduleInformation().importFunctionCount())
                entrypoint = m_calleeGroup->m_wasmToWasmExitStubs[call.functionIndexSpace].code();
            else {
                calleeCallee = &m_calleeGroup->wasmEntrypointCalleeFromFunctionIndexSpace(locker, call.functionIndexSpace);
                entrypoint = m_calleeGroup->wasmEntrypointCalleeFromFunctionIndexSpace(locker, call.functionIndexSpace).entrypoint().retagged<WasmEntryPtrTag>();
            }

            MacroAssembler::repatchNearCall(call.callLocation, CodeLocationLabel<WasmEntryPtrTag>(entrypoint));
            MacroAssembler::repatchPointer(call.calleeLocation, CalleeBits::boxNativeCalleeIfExists(calleeCallee));
        }

        m_calleeGroup->callsiteCollection().addCallsites(locker, m_calleeGroup.get(), callee->wasmToWasmCallsites());
        m_calleeGroup->callsiteCollection().updateCallsitesToCallUs(locker, m_calleeGroup, CodeLocationLabel<WasmEntryPtrTag>(entrypoint), m_functionIndex, functionIndexSpace);

        {
            if (BBQCallee* bbqCallee = m_calleeGroup->bbqCallee(locker, m_functionIndex)) {
                Locker locker { bbqCallee->tierUpCount()->getLock() };
                bbqCallee->setReplacement(callee.copyRef());
                bbqCallee->tierUpCount()->setCompilationStatusForOMG(mode(), TierUpCount::CompilationStatus::Compiled);
            }
            if (Options::useWasmIPInt() && m_calleeGroup->m_ipintCallees) {
                IPIntCallee& ipintCallee = m_calleeGroup->m_ipintCallees->at(m_functionIndex).get();
                Locker locker { ipintCallee.tierUpCounter().m_lock };
                ipintCallee.setReplacement(callee.copyRef(), mode());
                ipintCallee.tierUpCounter().setCompilationStatus(mode(), IPIntTierUpCounter::CompilationStatus::Compiled);
            }
            if (!Options::useWasmIPInt() && m_calleeGroup->m_llintCallees) {
                LLIntCallee& llintCallee = m_calleeGroup->m_llintCallees->at(m_functionIndex).get();
                Locker locker { llintCallee.tierUpCounter().m_lock };
                llintCallee.setReplacement(callee.copyRef(), mode());
                llintCallee.tierUpCounter().setCompilationStatus(mode(), LLIntTierUpCounter::CompilationStatus::Compiled);
            }
        }
    }

    // Replace the LLInt interpreted entry callee. Note that we can do this after we publish our
    // callee because calling into the LLInt should still work.
    auto* jsEntrypointCallee = m_calleeGroup->m_jsEntrypointCallees.get(m_functionIndex);
    if (jsEntrypointCallee && jsEntrypointCallee->compilationMode() == CompilationMode::JITLessJSEntrypointMode && !static_cast<JITLessJSEntrypointCallee*>(jsEntrypointCallee)->hasReplacement()) {
        ASSERT(!m_entrypoint);
        Locker locker { m_lock };
        TypeIndex typeIndex = m_moduleInformation->internalFunctionTypeIndices[m_functionIndex];
        const TypeDefinition& signature = TypeInformation::get(typeIndex).expand();

        auto callee = JSEntrypointJITCallee::create();
        context.jsEntrypointJIT = makeUnique<CCallHelpers>();
        Vector<UnlinkedWasmToWasmCall> newCall;
        auto jsToWasmInternalFunction = createJSToWasmWrapper(*context.jsEntrypointJIT, callee.get(), nullptr, signature, &newCall, m_moduleInformation.get(), m_mode, m_functionIndex);
        auto linkBuffer = makeUnique<LinkBuffer>(*context.jsEntrypointJIT, &callee.get(), LinkBuffer::Profile::WasmBBQ, JITCompilationCanFail);

        if (linkBuffer->isValid()) {
            jsToWasmInternalFunction->entrypoint.compilation = makeUnique<Compilation>(
                FINALIZE_WASM_CODE(*linkBuffer, JITCompilationPtrTag, nullptr, "(ipint upgrade edition) JS->WebAssembly entrypoint[%i] %s", m_functionIndex, signature.toString().ascii().data()),
                nullptr);

            for (auto& call : newCall) {
                CodePtr<WasmEntryPtrTag> entrypoint;
                if (call.functionIndexSpace < m_moduleInformation->importFunctionCount())
                    entrypoint = m_calleeGroup->m_wasmToWasmExitStubs[call.functionIndexSpace].code();
                else
                    entrypoint = m_calleeGroup->wasmEntrypointCalleeFromFunctionIndexSpace(locker, call.functionIndexSpace).entrypoint().retagged<WasmEntryPtrTag>();

                MacroAssembler::repatchNearCall(call.callLocation, CodeLocationLabel<WasmEntryPtrTag>(entrypoint));
            }

            callee->setEntrypoint(WTFMove(jsToWasmInternalFunction->entrypoint));
            // Note that we can compile the same function with multiple memory modes, which can cause this
            // race. That's fine, both stubs should do the same thing.
            static_cast<JITLessJSEntrypointCallee*>(jsEntrypointCallee)->setReplacement(callee.ptr());
            m_entrypoint = WTFMove(callee);
        }
    }

    dataLogLnIf(WasmOMGPlanInternal::verbose, "Finished OMG ", m_functionIndex);
    Locker locker { m_lock };
    complete();
}

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY_OMGJIT)
