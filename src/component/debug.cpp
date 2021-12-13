#include <stdinc.hpp>
#include "loader/component_loader.hpp"

#include "game/structs.hpp"
#include "game/game.hpp"

#include "debug.hpp"
#include "gsc.hpp"
#include "json.hpp"

#include "scheduler.hpp"
#include "scripting.hpp"
#include "command.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace debug
{
    namespace
    {
        const game::dvar_t* developer_script = nullptr;

        void print_error(unsigned int opcode)
        {
            const auto error = reinterpret_cast<const char*>(SELECT(0x2E27C70, 0x2DF7F70));
            const auto fs_pos = *reinterpret_cast<char**>(SELECT(0x2E23C08, 0x2DF3F08));

            if (opcode == 0x29 || opcode == 0x2A)
            {
                std::string name{};
                for (auto i = 0; i < 4; i++)
                {
                    const auto ptr = *reinterpret_cast<void**>(fs_pos + i);
                    name = opcode == 0x2A 
                        ? gsc::find_builtin_method_name(ptr) 
                        : gsc::find_builtin_name(ptr);

                    if (!name.empty())
                    {
                        break;
                    }
                }

                printf("******* script runtime error *******\n");
                printf("in call to builtin %s '%s': %s\n", opcode == 0x2A ? "method" : "function", name.data(), error);
                printf(debug::get_call_stack().data());
                printf("************************************\n");
            }
            else
            {
                printf("******* script runtime error *******\n");
                printf("executing opcode 0x%lX: %s\n", opcode, error);
                printf(debug::get_call_stack().data());
                printf("************************************\n");
            }
        }

        __declspec(naked) void vm_exeucte_error_stub_zm()
        {
            __asm
            {
                pushad
                push eax
                call print_error
                pop eax
                popad

                add eax, 0xFFFFFFE5
                mov [ebp + 0x6C], esi
                mov [ebp + 0x44], edx

                push 0x8F77C0
                retn
            }
        }

        __declspec(naked) void vm_exeucte_error_stub_mp()
        {
            __asm
            {
                pushad
                push eax
                call print_error
                pop eax
                popad

                add eax, 0xFFFFFFE5
                mov[ebp + 0x6C], esi
                mov[ebp + 0x44], edx

                push 0x8F8A60
                retn
            }
        }
    }

    std::vector<scripting::thread> get_all_threads()
    {
        std::vector<scripting::thread> threads;

        for (auto i = 1; i < 0x6000; i++)
        {
            const auto type = game::scr_VarGlob->objectVariableValue[i].w.type & 0x7F;
            if (type == game::SCRIPT_THREAD || type == game::SCRIPT_TIME_THREAD || type == game::SCRIPT_NOTIFY_THREAD)
            {
                threads.push_back(i);
            }
        }

        return threads;
    }

    std::string get_call_stack()
    {
        std::string info{};
        const auto line = [&info](const std::string& text)
        {
            info.append(text);
            info.append("\r\n");
        };

        const auto fs_pos = *reinterpret_cast<char**>(SELECT(0x2E23C08, 0x2DF3F08));

        for (auto frame = game::scr_VmPub->function_frame; frame != game::scr_VmPub->function_frame_start; --frame)
        {
            const auto function = scripting::find_function(frame == game::scr_VmPub->function_frame ? fs_pos : frame->fs.pos);
            const scripting::object local_vars{frame->fs.localId};

            line(utils::string::va("    at %s", function.data()));
            if (local_vars.get_keys().size())
            {
                line(utils::string::va("      local vars: %s", json::gsc_to_string(local_vars).data()));
            }
        }

        return info;
    }

    class component final : public component_interface
    {
    public:
        void post_unpack() override
        {
            developer_script = game::Dvar_FindVar("developer_script");
            utils::hook::jump(SELECT(0x8F8A57, 0x8F77B7), SELECT(vm_exeucte_error_stub_mp, vm_exeucte_error_stub_zm));

            gsc::function::add("crash", [](const gsc::function_args& args) -> scripting::script_value
            {
                if (!developer_script->current.enabled)
                {
                    return {};
                }

                *reinterpret_cast<int*>(0) = 1;
                return {};
            });

            gsc::function::add("breakpoint", [](const gsc::function_args& args) -> scripting::script_value
            {
                if (!developer_script->current.enabled)
                {
                    return {};
                }

                std::string msg;
                if (args.size() >= 1)
                {
                    msg = args[0].to_string();
                }

                const auto text = utils::string::va("%s\n %s", msg.data(), get_call_stack().data());
                MessageBoxA(nullptr, text, "GSC Breakpoint", MB_ICONERROR);
                return {};
            });

            gsc::function::add("assert", [](const gsc::function_args& args) -> scripting::script_value
            {
                if (!developer_script->current.enabled )
                {
                    return {};
                }

                const auto assertion = args[0].as<bool>();
                if (!assertion)
                {
                    throw std::runtime_error("assertion failed");
                }

                return {};
            });

            gsc::function::add("killthread", [](const gsc::function_args& args) -> scripting::script_value
            {
                const auto function = args[0].as<scripting::function>();
                const auto threads = get_all_threads();

                auto entity_id = 0;
                if (args.size() >= 2)
                {
                    entity_id = args[1].as<scripting::entity>().get_entity_id();
                }

                for (const auto& thread : threads)
                {
                    if (thread.get_start_pos() == function.get_pos() && (!entity_id || (entity_id == thread.get_self())))
                    {
                        thread.kill();
                        return 1;
                    }
                }

                return 0;
            });

            gsc::function::add("killallthreads", [](const gsc::function_args& args) -> scripting::script_value
            {
                const auto function = args[0].as<scripting::function>();
                const auto threads = get_all_threads();

                auto entity_id = 0;
                if (args.size() >= 2)
                {
                    entity_id = args[1].as<scripting::entity>().get_entity_id();
                }

                auto count = 0;
                for (const auto& thread : threads)
                {
                    if (thread.get_start_pos() == function.get_pos() && (!entity_id || (entity_id == thread.get_self())))
                    {
                        thread.kill();
                        count++;
                    }
                }

                return count;
            });
        }
    };
}

REGISTER_COMPONENT(debug::component)