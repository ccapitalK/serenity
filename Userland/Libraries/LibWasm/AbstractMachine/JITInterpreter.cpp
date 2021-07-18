/*
 * Copyright (c) 2021, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __serenity__
// #if 1

#include <LibC/mallocdefs.h>
#include <LibJIT/X86Assembler.h>
#include <LibWasm/AbstractMachine/JITInterpreter.h>
#include <LibX86/Instruction.h>
#include <Libraries/LibC/sys/mman.h>
#include <Libraries/LibSystem/syscall.h>
#include <unistd.h>

namespace Wasm {

GuardedStackSpace::GuardedStackSpace(size_t length)
    : m_length(PAGE_ROUND_UP(length))
{
    // Grab region of address space padded with guard pages
    auto const prot = PROT_READ | PROT_WRITE;
    auto const flags = MAP_ANONYMOUS | MAP_PRIVATE;
    auto const padded_region_length = m_length + 2 * PAGE_SIZE;
    u8* region = static_cast<u8*>(serenity_mmap(nullptr, padded_region_length, prot, flags, 0, 0, PAGE_SIZE, "Wasm JIT stack"));
    VERIFY(region != MAP_FAILED);
    m_memory = region + PAGE_SIZE;

    // Advise that the guard pages don't need backing frames
    madvise(region, PAGE_SIZE, MADV_SET_VOLATILE);
    madvise(m_memory + m_length, PAGE_SIZE, MADV_SET_VOLATILE);

    // Mark the guard pages inaccessible
    mprotect(region, PAGE_SIZE, PROT_NONE);
    mprotect(m_memory + m_length, PAGE_SIZE, PROT_NONE);
}

GuardedStackSpace::~GuardedStackSpace()
{
    munmap(m_memory - PAGE_SIZE, m_length + 2 * PAGE_SIZE);
}

JITInterpreter::JITInterpreter()
    : m_instruction_buf("Wasm JIT")
    , m_assembler(m_instruction_buf)
{
}

void JITInterpreter::interpret(Configuration& configuration)
{
    auto entry_point = m_instruction_buf.get_current_offset();

    auto const start_of_stack = m_stack_space.span().data();
    auto stack_as_u64s = m_stack_space.span_u64().data();

    // Copy function parameters to start of stack space
    auto const num_locals = configuration.frame().locals().size();
    VERIFY(num_locals <= 128);
    for (auto i = 0u; i < num_locals; ++i) {
        auto& local = configuration.frame().locals()[i];
        if (local.type().kind() == ValueType::I32) {
            stack_as_u64s[i] = local.value().get<i32>();
        } else if (local.type().kind() == ValueType::I64) {
            stack_as_u64s[i] = local.value().get<i64>();
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    // Prelude
    m_assembler.prelude();
    (void) start_of_stack;
    auto const start_of_user_stack = bit_cast<u32>(start_of_stack + 8 * configuration.frame().locals().size());
    m_assembler.move<32>(JIT::RegisterIndex(stack_register), JIT::Immediate(start_of_user_stack));
    m_assembler.move<32>(JIT::RegisterIndex(param_register), JIT::Immediate(bit_cast<u32>(start_of_stack)));

    for (auto const& ins : configuration.frame().expression().instructions()) {
        generate_instruction(ins);
    }

    //epilogue
    m_assembler.epilogue();
    m_assembler.ret();

    m_instruction_buf.finalize();
    m_instruction_buf.dump_encoded_instructions();
    m_instruction_buf.enter_at_offset(entry_point);

    for (auto i = 0u; i < configuration.frame().arity(); ++i) {
        auto type = configuration.frame().result_types()[i].kind();
        if (type == ValueType::I32 || type == ValueType::I64) {
            configuration.stack().push(Value { ValueType(type), stack_as_u64s[num_locals + i] });
        } else {
            VERIFY_NOT_REACHED();
        }
    }
}

bool JITInterpreter::did_trap() const
{
    return false;
}

String JITInterpreter::trap_reason() const
{
    return "This shouldn't be reachable";
}

void JITInterpreter::clear_trap()
{
}

void JITInterpreter::generate_instruction(Instruction const& instruction)
{
    auto grab_two_args_from_stack = [this](JIT::RegisterIndex reg1, JIT::RegisterIndex reg2) {
        m_assembler.add_register32_imm32(stack_register, -8);
        m_assembler.move<32>(reg1, JIT::DereferencedRegisterIndex(stack_register));
        m_assembler.add_register32_imm32(stack_register, -8);
        m_assembler.move<32>(reg2, JIT::DereferencedRegisterIndex(stack_register));
    };
    auto push_value_back_to_stack = [this](JIT::RegisterIndex reg) {
        m_assembler.move<32>(JIT::DereferencedRegisterIndex(stack_register), reg);
        m_assembler.add_register32_imm32(stack_register, 8);
    };
    switch (instruction.opcode().value()) {
    case Instructions::i32_const.value(): {
        m_assembler.move<32>(JIT::DereferencedRegisterIndex(stack_register), JIT::Immediate(instruction.arguments().get<i32>()));
        m_assembler.add_register32_imm32(stack_register, 8);
        break;
    }
    case Instructions::local_get.value(): {
        // TODO: Implement [mm + i] addressing
        auto index = instruction.arguments().get<LocalIndex>().value();
        if (index != 0)
            m_assembler.add_register32_imm32(param_register, 8 * index);
        m_assembler.move<32>(JIT::RegisterIndex(scratch_register1), JIT::DereferencedRegisterIndex(param_register));
        if (index != 0)
            m_assembler.add_register32_imm32(param_register, -8 * index);
        m_assembler.move<32>(JIT::DereferencedRegisterIndex(stack_register), JIT::RegisterIndex(scratch_register1));
        m_assembler.add_register32_imm32(stack_register, 8);
        break;
    }
    case Instructions::i32_add.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.add_register32_reg32(scratch_register1, scratch_register2);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    case Instructions::i32_sub.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.sub_register32_reg32(scratch_register1, scratch_register2);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    case Instructions::i32_mul.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.push_register32(stack_register);
        m_assembler.mul_register32(scratch_register2);
        m_assembler.pop_register32(stack_register);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    case Instructions::i32_and.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.and_register32_reg32(scratch_register1, scratch_register2);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    case Instructions::i32_or.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.or_register32_reg32(scratch_register1, scratch_register2);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    case Instructions::i32_xor.value(): {
        grab_two_args_from_stack(scratch_register1, scratch_register2);
        m_assembler.xor_register32_reg32(scratch_register1, scratch_register2);
        push_value_back_to_stack(scratch_register1);
        break;
    }
    default: {
        VERIFY_NOT_REACHED();
    }
    }
}

}

#endif