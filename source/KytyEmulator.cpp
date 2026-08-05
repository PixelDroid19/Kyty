#include "Kyty/Core/Core.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Scripts/Scripts.h"

#include "Emulator/Agent/AgentSubsystem.h"
#include "Emulator/Audio.h"
#include "Emulator/Common.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Ports/AudioPausePort.h"
#include "Emulator/Ports/ControllerInputPort.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Network.h"
#include "Emulator/Profiler.h"
#include "Emulator/SystemContentPort.h"
#include "Emulator/Validation/DomainValidators.h"

#include <cstdlib>
#include <cstring>

namespace Kyty::Emulator {

#ifdef KYTY_EMU_ENABLED

namespace LuaFunc {

// The CLI's Lua host adapts its script value to the neutral configuration
// source; the runtime never depends on the script engine.
class LuaConfigSource final: public Config::ConfigSource
{
public:
	explicit LuaConfigSource(const Scripts::ScriptVar& cfg): m_cfg(cfg) {}

	[[nodiscard]] bool Has(const String& key) const override
	{
		return !m_cfg.At(key).IsNil();
	}

	[[nodiscard]] int64_t GetInteger(const String& key) const override
	{
		return m_cfg.At(key).ToInteger();
	}

	[[nodiscard]] bool GetBool(const String& key) const override
	{
		return m_cfg.At(key).ToBool();
	}

	[[nodiscard]] String GetString(const String& key) const override
	{
		return m_cfg.At(key).ToString();
	}

private:
	const Scripts::ScriptVar& m_cfg;
};

static bool get_system_content_param_string(const char* name, char* value, size_t value_size)
{
	return Loader::SystemContentParamSfoGetString(name, value, value_size);
}

static void load_symbols(const String& id, Loader::RuntimeLinker* rt)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(rt == nullptr);
	if (!Libs::Init(id, rt->Symbols()))
	{
		EXIT("Unknown library: %s\n", id.C_Str());
	}
}

static void load_symbols_all(Loader::RuntimeLinker* rt)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(rt == nullptr);

	Libs::InitAll(rt->Symbols());
}

static void print_system_info()
{
	Core::SystemInfo info = Core::GetSystemInfo();

	KYTY_LOG_INFO("ProcessorName = %s\n", info.ProcessorName.C_Str());
}

static void kyty_close()
{
	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();
	Libs::LibKernel::ApplicationHeap::Reset();

	KYTY_LOG_INFO("done!\n");

	if (auto* subsystems = Core::SubsystemsList::Active(); subsystems != nullptr)
	{
		subsystems->ShutdownAll();
	}
}

static void Init(const Scripts::ScriptVar& cfg)
{
	EXIT_IF(!Core::Thread::IsMainThread());

	SystemContentPort::Install({Loader::SystemContentGetMetadata, Loader::SystemContentGetIconPath, get_system_content_param_string});

	auto* slist = Core::SubsystemsList::Instance();

	auto* audio       = Libs::Audio::AudioSubsystem::Instance();
	auto* agent       = Emulator::Agent::AgentToolsSubsystem::Instance();
	auto* config      = Config::ConfigSubsystem::Instance();
	auto* controller  = Libs::Controller::ControllerSubsystem::Instance();
	auto* core        = Core::CoreSubsystem::Instance();
	auto* file_system = Kernel::FileSystem::FileSystemSubsystem::Instance();
	auto* graphics    = Libs::Graphics::GraphicsSubsystem::Instance();
	auto* log         = Log::LogSubsystem::Instance();
	auto* memory      = Kernel::Memory::MemorySubsystem::Instance();
	auto* network     = Libs::Network::NetworkSubsystem::Instance();
	auto* profiler    = Profiler::ProfilerSubsystem::Instance();
	auto* pthread     = Kernel::PthreadSubsystem::Instance();
	auto* scripts     = Scripts::ScriptsSubsystem::Instance();
	auto* timer       = Loader::Timer::TimerSubsystem::Instance();

	slist->Add(config, {core, scripts});
	if (!slist->InitAll(true))
	{
		EXIT("Failed to initialize '%s' subsystem: %s\n", slist->GetFailName(), slist->GetFailMsg());
		return;
	}

	Config::Load(LuaConfigSource(cfg));

	slist->Add(audio, {core, log, pthread, memory});
	slist->Add(agent, {core, controller, graphics});
	slist->Add(controller, {core, log, config});
	slist->Add(file_system, {core, log, pthread});
	slist->Add(graphics, {core, log, pthread, memory, config, profiler, controller});
	slist->Add(log, {core, config});
	slist->Add(memory, {core, log});
	slist->Add(network, {core, log, pthread});
	slist->Add(profiler, {core, config});
	slist->Add(pthread, {core, log, timer});
	slist->Add(timer, {core, log});

	if (!slist->InitAll(true))
	{
		EXIT("Failed to initialize '%s' subsystem: %s\n", slist->GetFailName(), slist->GetFailMsg());
		return;
	}

	// Composition root wiring: the HLE implementations of the host-facing
	// input and audio-pause bridges are installed here, after their
	// subsystems exist and before any window can forward events.
	::Kyty::Emulator::Ports::ControllerInputPort::Install(
	    {&Libs::Controller::ControllerConnect, &Libs::Controller::ControllerDisconnect, &Libs::Controller::ControllerButton,
	     &Libs::Controller::ControllerAxis});
	::Kyty::Emulator::Ports::AudioPausePort::Install(&Libs::Audio::AudioOut::AudioOutSetHostPaused);
}

KYTY_SCRIPT_FUNC(kyty_load_cfg_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar cfg = Scripts::ArgGetVar(0);

	Config::Load(LuaConfigSource(cfg));

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_init_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar cfg = Scripts::ArgGetVar(0);

	Init(cfg);

	print_system_info();

	int ok = atexit(kyty_close);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_load_elf_func)
{
	if (Scripts::ArgGetVarCount() != 1 && Scripts::ArgGetVarCount() != 2 && Scripts::ArgGetVarCount() != 3)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar elf = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	const auto real_path = Kernel::FileSystem::GetRealFilename(elf.ToString());
	auto*      program   = Loader::ProgramLoader::Load(rt, real_path);

	// Thin orchestration: after the primary executable is loaded, the lifecycle
	// coordinator may build a deterministic adjacent-module plan (feature-gated).
	// run_guest.lua stays a thin mount/param/load/symbols/execute entry point.
	if (program != nullptr && program->elf != nullptr && !program->elf->IsShared() &&
	    Loader::GuestExecutableLocator::IsPrimaryExecutableName(real_path))
	{
		Loader::ModuleLifecycleCoordinator::AfterPrimaryLoaded(rt, real_path);
	}

	if (Scripts::ArgGetVarCount() >= 2)
	{
		if (Scripts::ArgGetVar(1).ToInteger() == 1)
		{
			program->dbg_print_reloc = true;
		}
	}

	if (Scripts::ArgGetVarCount() >= 3)
	{
		auto save_name = Scripts::ArgGetVar(2).ToString();

		rt->SaveProgram(program, Kernel::FileSystem::GetRealFilename(save_name));
	}

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_save_main_elf_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar elf = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->SaveMainProgram(Kernel::FileSystem::GetRealFilename(elf.ToString()));

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_load_symbols_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count < 1)
	{
		EXIT("invalid args\n");
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	for (int i = 0; i < count; i++)
	{
		Scripts::ScriptVar id = Scripts::ArgGetVar(i);
		load_symbols(id.ToString(), rt);
	}

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_load_symbols_all_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count != 0)
	{
		EXIT("invalid args\n");
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	// Building HLE and package symbol databases is a bounded bootstrap phase.
	// Keep it synchronous for linker correctness, but bypass the debug allocator
	// tracker: recording a stack and hashmap entry per export made this phase
	// monopolize the UI/guest launch thread for seconds.
	Core::MemTrackerSuspendScope tracker_suspend;
	load_symbols_all(rt);
	Loader::ModuleLifecycleCoordinator::AfterHleSymbolsRegistered(rt);

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_load_param_sfo_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count != 1)
	{
		EXIT("invalid args\n");
	}

	auto file_name = Scripts::ArgGetVar(0).ToString();

	if (!file_name.IsEmpty())
	{
		Loader::SystemContentLoadParamSfo(Scripts::ArgGetVar(0).ToString());
	}

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_load_param_json_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Loader::SystemContentLoadParamJson(Scripts::ArgGetVar(0).ToString());

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_dbg_dump_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar dbg_dir = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->DbgDump(dbg_dir.ToString());

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_dbg_dump_symbols_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar dbg_dir = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->DbgDumpSymbols(dbg_dir.ToString());

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_execute_func)
{
	if (Scripts::ArgGetVarCount() != 0)
	{
		EXIT("invalid args\n");
	}

	int thread_model = 1;

	if (thread_model == 0)
	{
		Core::Thread t([](void* /*unused*/) { Libs::Graphics::WindowRun(); }, nullptr);
		t.Detach();
		auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
		rt->Execute();
	} else
	{
		Core::Thread t(
		    [](void* /*unused*/)
		    {
			    auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
			    rt->Execute();
		    },
		    nullptr);
		t.Detach();
		Libs::Graphics::WindowRun();
		t.Join();
	}

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_mount_func)
{
	if (Scripts::ArgGetVarCount() != 2)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar folder = Scripts::ArgGetVar(0);
	Scripts::ScriptVar point  = Scripts::ArgGetVar(1);

	// validate → policy for game-run guest root (before filesystem mutation)
	const String folder_s   = folder.ToString();
	const auto   folder_str = folder_s.utf8_str();
	const String point_s    = point.ToString();
	const bool   bringup    = (std::getenv("KYTY_BRINGUP_MODE") != nullptr || std::getenv("KYTY_BRINGUP_FEATURES") != nullptr ||
	                           std::getenv("KYTY_BRINGUP_SUBSYSTEMS") != nullptr || std::getenv("KYTY_BRINGUP_BURST_LIMIT") != nullptr ||
	                           std::getenv("KYTY_BRINGUP_BURST_WINDOW_MS") != nullptr);
	const char*  allow_env  = std::getenv("KYTY_BRINGUP_ALLOW_DIAGNOSTIC");
	const bool   allow_diag = (allow_env != nullptr && std::strcmp(allow_env, "1") == 0);
	const bool   removed_permissive_env = (std::getenv("KYTY_STUB_MISSING") != nullptr || std::getenv("KYTY_GFX_PERMISSIVE") != nullptr);

	Emulator::Validation::GameRunRequest greq {};
	greq.guest_root                     = folder_str.GetData();
	greq.bringup_env_present            = bringup;
	greq.allow_diagnostic_override      = allow_diag;
	greq.removed_permissive_env_present = removed_permissive_env;
	const auto validated                = Emulator::Validation::ValidateGameRunRequest(greq);
	if (!validated.Ok())
	{
		EXIT("game-run validation failed: %s (%s)\n", validated.error.reason, validated.error.code);
	}

	Kernel::FileSystem::Mount(folder_s, point_s);

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_shader_disable)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	auto id = Scripts::ArgGetVar(0).ToString().ToUint64(16);

	Libs::Graphics::ShaderDisable(id);

	return 0;
}

KYTY_SCRIPT_FUNC(kyty_shader_printf)
{
	if (Scripts::ArgGetVarCount() != 4)
	{
		EXIT("invalid args\n");
	}

	auto id     = Scripts::ArgGetVar(0).ToString().ToUint64(16);
	auto pc     = Scripts::ArgGetVar(1).ToInteger();
	auto format = Scripts::ArgGetVar(2).ToString();
	auto args   = Scripts::ArgGetVar(3);

	if (!args.IsTable())
	{
		EXIT("invalid args\n");
	}

	Libs::Graphics::ShaderDebugPrintf p;
	p.pc     = pc;
	p.format = format;

	for (const auto& t: args.GetPairs())
	{
		const auto& arg_t = t.GetValue();

		if (!arg_t.IsTable())
		{
			EXIT("invalid arg\n");
		}

		auto type = arg_t.GetValue(0).ToString();
		auto arg  = arg_t.GetValue(1).ToString();

		Libs::Graphics::ShaderOperand op;

		if (arg.StartsWith('s', String::Case::Insensitive) && Core::Char::IsDecimal(arg.At(1)))
		{
			op.type        = Libs::Graphics::ShaderOperandType::Sgpr;
			op.register_id = arg.RemoveFirst(1).ToInt32();
			op.size        = 1;
		} else if (arg.StartsWith('v', String::Case::Insensitive) && Core::Char::IsDecimal(arg.At(1)))
		{
			op.type        = Libs::Graphics::ShaderOperandType::Vgpr;
			op.register_id = arg.RemoveFirst(1).ToInt32();
			op.size        = 1;
		} else
		{
			EXIT("unknown arg: %s\n", arg.C_Str());
		}

		p.args.Add(op);

		Libs::Graphics::ShaderDebugPrintf::Type st = Libs::Graphics::ShaderDebugPrintf::Type::Int;

		if (type.EqualNoCase(U"int"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Int;
		} else if (type.EqualNoCase(U"uint"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Uint;
		} else if (type.EqualNoCase(U"float"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Float;
		} else
		{
			EXIT("unknown type: %s\n", arg.C_Str());
		}

		p.types.Add(st);
	}

	Libs::Graphics::ShaderInjectDebugPrintf(id, p);

	return 0;
}

void kyty_help() {}

} // namespace LuaFunc

void kyty_reg()
{
	Scripts::RegisterFunc("kyty_init", LuaFunc::kyty_init_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_cfg", LuaFunc::kyty_load_cfg_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_elf", LuaFunc::kyty_load_elf_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_save_main_elf", LuaFunc::kyty_save_main_elf_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_symbols", LuaFunc::kyty_load_symbols_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_symbols_all", LuaFunc::kyty_load_symbols_all_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_param_sfo", LuaFunc::kyty_load_param_sfo_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_load_param_json", LuaFunc::kyty_load_param_json_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_dbg_dump", LuaFunc::kyty_dbg_dump_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_dbg_dump_symbols", LuaFunc::kyty_dbg_dump_symbols_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_execute", LuaFunc::kyty_execute_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_mount", LuaFunc::kyty_mount_func, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_shader_disable", LuaFunc::kyty_shader_disable, LuaFunc::kyty_help);
	Scripts::RegisterFunc("kyty_shader_printf", LuaFunc::kyty_shader_printf, LuaFunc::kyty_help);
}

#else
void kyty_reg() {}
#endif // KYTY_EMU_ENABLED

} // namespace Kyty::Emulator
