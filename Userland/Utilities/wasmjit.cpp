/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/FileStream.h>
#include <LibLine/Editor.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/JITInterpreter.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <signal.h>
#include <unistd.h>

RefPtr<Line::Editor> g_line_editor;
static auto g_stdout = Core::OutputFileStream::standard_error();
static Wasm::Printer g_printer { g_stdout };
static bool g_continue { false };
static void (*old_signal)(int);
static Wasm::JITInterpreter g_interpreter;

static void sigint_handler(int)
{
    if (!g_continue) {
        signal(SIGINT, old_signal);
        kill(getpid(), SIGINT);
    }
    g_continue = false;
}

static Optional<Wasm::Module> parse(StringView const& filename)
{
    auto result = Core::File::open(filename, Core::OpenMode::ReadOnly);
    if (result.is_error()) {
        warnln("Failed to open {}: {}", filename, result.error());
        return {};
    }

    auto stream = Core::InputFileStream(result.release_value());
    auto parse_result = Wasm::Module::parse(stream);
    if (parse_result.is_error()) {
        warnln("Something went wrong, either the file is invalid, or there's a bug with LibWasm!");
        warnln("The parse error was {}", Wasm::parse_error_to_string(parse_result.error()));
        return {};
    }
    return parse_result.release_value();
}

static void print_link_error(Wasm::LinkError const& error)
{
    for (auto const& missing : error.missing_imports)
        warnln("Missing import '{}'", missing);
}

int main(int argc, char* argv[])
{
    char const* filename = nullptr;
    bool print = false;
    bool attempt_instantiate = false;
    bool debug = false;
    bool export_all_imports = false;
    bool shell_mode = false;
    String exported_function_to_execute;
    Vector<u64> values_to_push;
    Vector<String> modules_to_link_in;

    Core::ArgsParser parser;
    parser.add_positional_argument(filename, "File name to parse", "file");
    parser.add_option(debug, "Open a debugger", "debug", 'd');
    parser.add_option(print, "Print the parsed module", "print", 'p');
    parser.add_option(attempt_instantiate, "Attempt to instantiate the module", "instantiate", 'i');
    parser.add_option(exported_function_to_execute, "Attempt to execute the named exported function from the module (implies -i)", "execute", 'e', "name");
    parser.add_option(export_all_imports, "Export noop functions corresponding to imports", "export-noop", 0);
    parser.add_option(shell_mode, "Launch a REPL in the module's context (implies -i)", "shell", 's');
    parser.add_option(Core::ArgsParser::Option {
        .requires_argument = true,
        .help_string = "Extra modules to link with, use to resolve imports",
        .long_name = "link",
        .short_name = 'l',
        .value_name = "file",
        .accept_value = [&](char const* str) {
            if (auto v = StringView { str }; !v.is_empty()) {
                modules_to_link_in.append(v);
                return true;
            }
            return false;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .requires_argument = true,
        .help_string = "Supply arguments to the function (default=0) (expects u64, casts to required type)",
        .long_name = "arg",
        .short_name = 0,
        .value_name = "u64",
        .accept_value = [&](char const* str) -> bool {
            if (auto v = StringView { str }.to_uint<u64>(); v.has_value()) {
                values_to_push.append(v.value());
                return true;
            }
            return false;
        },
    });
    parser.parse(argc, argv);

    if (shell_mode) {
        debug = true;
        attempt_instantiate = true;
    }

    if (!shell_mode && debug && exported_function_to_execute.is_empty()) {
        warnln("Debug what? (pass -e fn)");
        return 1;
    }

    if (debug || shell_mode) {
        old_signal = signal(SIGINT, sigint_handler);
    }

    if (!exported_function_to_execute.is_empty())
        attempt_instantiate = true;

    auto parse_result = parse(filename);
    if (!parse_result.has_value())
        return 1;

    if (print && !attempt_instantiate) {
        auto out_stream = Core::OutputFileStream::standard_output();
        Wasm::Printer printer(out_stream);
        printer.print(parse_result.value());
    }

    if (attempt_instantiate) {
        Wasm::AbstractMachine machine;
        Core::EventLoop main_loop;
        if (debug) {
            g_line_editor = Line::Editor::construct();
        }

        // First, resolve the linked modules
        NonnullOwnPtrVector<Wasm::ModuleInstance> linked_instances;
        Vector<Wasm::Module> linked_modules;
        for (auto& name : modules_to_link_in) {
            auto parse_result = parse(name);
            if (!parse_result.has_value()) {
                warnln("Failed to parse linked module '{}'", name);
                return 1;
            }
            linked_modules.append(parse_result.release_value());
            Wasm::Linker linker { linked_modules.last() };
            for (auto& instance : linked_instances)
                linker.link(instance);
            auto link_result = linker.finish();
            if (link_result.is_error()) {
                warnln("Linking imported module '{}' failed", name);
                print_link_error(link_result.error());
                return 1;
            }
            auto instantiation_result = machine.instantiate(linked_modules.last(), link_result.release_value());
            if (instantiation_result.is_error()) {
                warnln("Instantiation of imported module '{}' failed: {}", name, instantiation_result.error().error);
                return 1;
            }
            linked_instances.append(instantiation_result.release_value());
        }

        Wasm::Linker linker { parse_result.value() };
        for (auto& instance : linked_instances)
            linker.link(instance);

        if (export_all_imports) {
            HashMap<Wasm::Linker::Name, Wasm::ExternValue> exports;
            for (auto& entry : linker.unresolved_imports()) {
                if (!entry.type.has<Wasm::TypeIndex>())
                    continue;
                auto type = parse_result.value().type(entry.type.get<Wasm::TypeIndex>());
                auto address = machine.store().allocate(Wasm::HostFunction(
                    [name = entry.name, type = type](auto&, auto& arguments) -> Wasm::Result {
                        StringBuilder argument_builder;
                        bool first = true;
                        for (auto& argument : arguments) {
                            DuplexMemoryStream stream;
                            Wasm::Printer { stream }.print(argument);
                            if (first)
                                first = false;
                            else
                                argument_builder.append(", "sv);
                            argument_builder.append(StringView(stream.copy_into_contiguous_buffer()).trim_whitespace());
                        }
                        dbgln("[wasm runtime] Stub function {} was called with the following arguments: {}", name, argument_builder.to_string());
                        Vector<Wasm::Value> result;
                        result.ensure_capacity(type.results().size());
                        for (auto& result_type : type.results())
                            result.append(Wasm::Value { result_type, 0ull });
                        return Wasm::Result { move(result) };
                    },
                    type));
                exports.set(entry, *address);
            }

            linker.link(exports);
        }

        auto link_result = linker.finish();
        if (link_result.is_error()) {
            warnln("Linking main module failed");
            print_link_error(link_result.error());
            return 1;
        }
        auto result = machine.instantiate(parse_result.value(), link_result.release_value());
        if (result.is_error()) {
            warnln("Module instantiation failed: {}", result.error().error);
            return 1;
        }
        auto module_instance = result.release_value();

        auto launch_repl = [&] {
            Wasm::Configuration config { machine.store() };
            Wasm::Expression expression { {} };
            config.set_frame(Wasm::Frame {
                *module_instance,
                Vector<Wasm::Value> {},
                expression,
                {},
            });
            Wasm::Instruction instr { Wasm::Instructions::nop };
            Wasm::InstructionPointer ip { 0 };
            g_continue = false;
        };

        auto stream = Core::OutputFileStream::standard_output();
        auto print_func = [&](auto const& address) {
            Wasm::FunctionInstance* fn = machine.store().get(address);
            stream.write(String::formatted("- Function with address {}, ptr = {}\n", address.value(), fn).bytes());
            if (fn) {
                stream.write(String::formatted("    wasm function? {}\n", fn->has<Wasm::WasmFunction>()).bytes());
                fn->visit(
                    [&](Wasm::WasmFunction const& func) {
                        Wasm::Printer printer { stream, 3 };
                        stream.write("    type:\n"sv.bytes());
                        printer.print(func.type());
                        stream.write("    code:\n"sv.bytes());
                        printer.print(func.code());
                    },
                    [](Wasm::HostFunction const&) {});
            }
        };
        if (print) {
            // Now, let's dump the functions!
            for (auto& address : module_instance->functions()) {
                print_func(address);
            }
        }

        if (shell_mode) {
            launch_repl();
            return 0;
        }

        if (!exported_function_to_execute.is_empty()) {
            Optional<Wasm::FunctionAddress> run_address;
            Vector<Wasm::Value> values;
            for (auto& entry : module_instance->exports()) {
                if (entry.name() == exported_function_to_execute) {
                    if (auto addr = entry.value().get_pointer<Wasm::FunctionAddress>())
                        run_address = *addr;
                }
            }
            if (!run_address.has_value()) {
                warnln("No such exported function, sorry :(");
                return 1;
            }

            auto instance = machine.store().get(*run_address);
            VERIFY(instance);

            if (instance->has<Wasm::HostFunction>()) {
                warnln("Exported function is a host function, cannot run that yet");
                return 1;
            }

            for (auto& param : instance->get<Wasm::WasmFunction>().type().parameters()) {
                if (values_to_push.is_empty())
                    values.append(Wasm::Value { param, 0ull });
                else
                    values.append(Wasm::Value { param, values_to_push.take_last() });
            }

            if (print) {
                outln("Executing ");
                print_func(*run_address);
                outln();
            }

            auto result = machine.invoke(g_interpreter, run_address.value(), move(values));

            if (result.is_trap())
                warnln("Execution trapped!");
            if (!result.values().is_empty())
                warnln("Returned:");
            for (auto& value : result.values()) {
                Wasm::Printer printer { stream };
                g_stdout.write("  -> "sv.bytes());
                g_printer.print(value);
            }
        }
    }

    return 0;
}
