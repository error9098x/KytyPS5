#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <cstring>
#include <iterator>
#include <fstream>
#include <thread>
#include <utility>
#include <vector>

namespace Emulator {

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

#if defined(__APPLE__)
	static constexpr auto platform_name = "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr auto platform_name = "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static constexpr auto platform_name = "Linux";
#else
	static constexpr auto platform_name = "Unknown";
#endif

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, platform_name, info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

// Opt-in Unreal config overrides for Silent Hill: The Short Message (PPSA10112, UE4), all off by default:
//   KYTY_UE_STREAMING_POOL_MB=<n>   r.Streaming.PoolSize=<n> MiB (texture streaming pool; the game's scalability
//                                   sets 400/600/800/1000, 3000 at Cinematic)
//   KYTY_UE_TEXTURE_QUALITY=<0..3>  sg.TextureQuality=<n> (scalability group; 0 = lowest: mip bias, pool 400)
//   KYTY_UE_CVARS="a=b;c=d"         any further [SystemSettings] cvars (e.g. r.Streaming.MipBias=3)
// The game stats and merges its user config layer at boot (/download0/shpc/saved/ps5/engine.ini, i.e.
// _DownloadData/PPSA10112/shpc/saved/ps5/engine.ini). We keep a marked [SystemSettings] block there:
// SetBySystemSettingsIni outranks SetByScalability and SetByGameSetting, so the values hold at every quality level
// (including the Cinematic 3000 MiB pool). The block is rewritten on every boot and removed when no variable is set,
// so the override never outlives the opt-in. Smaller pool / lower texture quality = less texture memory in guest
// RAM and in the GPU caches, at the cost of blurrier textures.
// Note: a boot stall first blamed on this file (2026-10-12) was the host CoreAudio failing to start
// (AudioQueueStart -66681), not the ini; see harness/LOG.md.
static void ApplyUeConfigOverrides(const std::string& title_id) {
	if (title_id != "PPSA10112") {
		return;
	}
	std::vector<std::string> lines;
	if (const char* env = std::getenv("KYTY_UE_STREAMING_POOL_MB"); env != nullptr) {
		const long mb = std::strtol(env, nullptr, 10);
		if (mb < 64 || mb > 16384) {
			LOGF("UeConfig: KYTY_UE_STREAMING_POOL_MB=%s out of range (64..16384), ignored\n", env);
		} else {
			lines.push_back("r.Streaming.PoolSize=" + std::to_string(mb));
		}
	}
	if (const char* env = std::getenv("KYTY_UE_TEXTURE_QUALITY"); env != nullptr) {
		const long q = std::strtol(env, nullptr, 10);
		if (q < 0 || q > 3 || env[0] == '\0') {
			LOGF("UeConfig: KYTY_UE_TEXTURE_QUALITY=%s out of range (0..3), ignored\n", env);
		} else {
			lines.push_back("sg.TextureQuality=" + std::to_string(q));
		}
	}
	if (const char* env = std::getenv("KYTY_UE_CVARS"); env != nullptr) {
		std::string all(env);
		size_t      pos = 0;
		while (pos <= all.size()) {
			auto end = all.find(';', pos);
			if (end == std::string::npos) {
				end = all.size();
			}
			std::string item = all.substr(pos, end - pos);
			const auto  a    = item.find_first_not_of(" \t");
			const auto  b    = item.find_last_not_of(" \t");
			item             = a == std::string::npos ? std::string() : item.substr(a, b - a + 1);
			if (!item.empty()) {
				if (item.find('=') == std::string::npos || item.find_first_of("\r\n[]") != std::string::npos) {
					LOGF("UeConfig: KYTY_UE_CVARS item '%s' is not name=value, ignored\n", item.c_str());
				} else {
					lines.push_back(item);
				}
			}
			pos = end + 1;
		}
	}
	static constexpr const char* kBegin = "; KYTY_UE_CONFIG begin (managed by Kyty, removed when unset)";
	static constexpr const char* kEnd   = "; KYTY_UE_CONFIG end";
	// Older builds used these markers; strip their block too.
	static constexpr const char* kOldBegin = "; KYTY_UE_STREAMING_POOL_MB begin";
	static constexpr const char* kOldEnd   = "; KYTY_UE_STREAMING_POOL_MB end";
	const std::filesystem::path dir  = std::filesystem::path("_DownloadData") / title_id / "shpc" / "saved" / "ps5";
	const std::filesystem::path path = dir / "engine.ini";
	std::string                 text;
	bool                        existed = false;
	{
		std::ifstream in(path, std::ios::binary);
		if (in) {
			existed = true;
			text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
	}
	for (const auto& [mb, me]: {std::pair {kBegin, kEnd}, std::pair {kOldBegin, kOldEnd}}) {
		const auto b = text.find(mb);
		if (b == std::string::npos) {
			continue;
		}
		auto e = text.find(me, b);
		e      = e == std::string::npos ? text.size() : e + std::strlen(me);
		if (e < text.size() && text[e] == '\n') {
			e++;
		}
		text.erase(b, e - b);
	}
	if (!lines.empty()) {
		if (!text.empty() && text.back() != '\n') {
			text += '\n';
		}
		text += std::string(kBegin) + "\n[SystemSettings]\n";
		for (const auto& l: lines) {
			text += l + "\n";
		}
		text += std::string(kEnd) + "\n";
	}
	if (!existed && lines.empty()) {
		return;
	}
	std::error_code ec;
	if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
		std::filesystem::remove(path, ec);
		return;
	}
	std::filesystem::create_directories(dir, ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
	if (!out) {
		LOGF("UeConfig: failed to write %s\n", Common::PathToString(path).c_str());
		return;
	}
	for (const auto& l: lines) {
		LOGF("UeConfig: %s [SystemSettings] %s (engine.ini user config layer)\n", title_id.c_str(), l.c_str());
	}
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	ApplyUeConfigOverrides(title_id);
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static bool ClearDirectoryContents(const std::filesystem::path& dir) {
	bool ok = true;

	for (const auto& entry: Common::File::GetDirEntries(dir)) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}

		auto path = dir / entry.name;

		if (entry.is_file) {
			Common::File::RemoveReadonly(path);
			ok = Common::File::DeleteFile(path) && ok;
		} else {
			ok = ClearDirectoryContents(path) && ok;
			ok = Common::File::DeleteDirectory(path) && ok;
		}
	}

	return ok;
}

static void ClearDebugTextureFolder() {
	const std::string debug_texture_folder = "_Textures";

	if (!Common::File::IsDirectoryExisting(debug_texture_folder)) {
		Common::File::CreateDirectories(debug_texture_folder);
		return;
	}

	if (!ClearDirectoryContents(debug_texture_folder)) {
		LOGF_COLOR(Log::Color::BrightYellow, "TextureDump: failed to completely clear %s\n",
		           debug_texture_folder.c_str());
	}
}

static void Init(const Config::ConfigOptions& cfg, const std::filesystem::path& param_json,
                 Common::Subsystems& subsystems) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(cfg);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
		if (const auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
		    flexible_memory_size != 0) {
			Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);
		}
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
	subsystems.Initialize<Libs::Graphics::Lifecycle>();
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

static void Execute(const std::filesystem::path& game_patch) {
	auto           patch_path = game_patch;
	Common::Thread guest_thread(
	    [](void* param) {
		    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		    rt->Execute(*static_cast<const std::filesystem::path*>(param));
	    },
	    &patch_path);
	Libs::Graphics::WindowRun();
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	const auto         param_json = options.app0_dir / "sce_sys" / "param.json";
	Common::Subsystems subsystems(true);
	Init(options.config, param_json, subsystems);

	ClearDebugTextureFolder();

	PrintSystemInfo();
	std::string title_id;
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) && !title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	MountSandboxDirs();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	LoadElf(options.elf);

	Execute(options.game_patch);
}

} // namespace Emulator
