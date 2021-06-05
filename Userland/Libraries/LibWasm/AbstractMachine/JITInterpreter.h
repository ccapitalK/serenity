/*
 * Copyright (c) 2021, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJIT/InstructionBuffer.h>
#include <LibJIT/X86Assembler.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibX86/Instruction.h>

namespace Wasm {

class GuardedStackSpace {
public:
    GuardedStackSpace(size_t length = 8096);
    ~GuardedStackSpace();

    Span<u8> span() const { return {m_memory, m_length}; }
    Span<u64> span_u64() const { return {bit_cast<u64*>(m_memory), m_length / 8}; }

private:
    u8* m_memory { nullptr };
    size_t m_length;
};

class JITInterpreter : public Interpreter {
public:
    JITInterpreter();
    virtual ~JITInterpreter() override {}
    virtual void interpret(Configuration&) override;
    virtual bool did_trap() const override;
    virtual void clear_trap() override;
    virtual String trap_reason() const override;

private:
    void generate_instruction(Instruction const&);

    JIT::InstructionBuffer m_instruction_buf;
    JIT::X86Assembler m_assembler;
    GuardedStackSpace m_stack_space;

    static constexpr X86::RegisterIndex32 scratch_register1 = X86::RegisterEAX;
    static constexpr X86::RegisterIndex32 scratch_register2 = X86::RegisterEBX;
    static constexpr X86::RegisterIndex32 param_register = X86::RegisterECX;
    static constexpr X86::RegisterIndex32 stack_register = X86::RegisterEDX;
};

}
