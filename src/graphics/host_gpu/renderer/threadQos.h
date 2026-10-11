#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADQOS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADQOS_H_

// KYTY_THREAD_QOS=1 (opt-in): put the host threads on the frame's critical path on the
// performance cores (QOS_CLASS_USER_INTERACTIVE) and hold a latency-critical NSProcessInfo
// activity so App Nap / timer coalescing never throttles the emulator. No effect when unset or
// on other platforms.

#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <pthread/qos.h>
#endif

namespace Libs::Graphics {

inline bool ThreadQosEnabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_THREAD_QOS");
		return v != nullptr && *v != '\0' && *v != '0';
	}();
	return enabled;
}

inline void BeginLatencyCriticalActivity() {
#if defined(__APPLE__)
	static const bool done = [] {
		// Foundation is already loaded by SDL/MoltenVK; resolve the ObjC runtime dynamically so
		// no new link dependency is needed.
		dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY);
		using GetClass = void* (*)(const char*);
		using RegSel   = void* (*)(const char*);
		auto* get_class = reinterpret_cast<GetClass>(dlsym(RTLD_DEFAULT, "objc_getClass"));
		auto* reg_sel   = reinterpret_cast<RegSel>(dlsym(RTLD_DEFAULT, "sel_registerName"));
		void* msg_send  = dlsym(RTLD_DEFAULT, "objc_msgSend");
		if (get_class == nullptr || reg_sel == nullptr || msg_send == nullptr) {
			std::printf("ThreadQos: objc runtime not found, App Nap not disabled\n");
			return false;
		}
		void* process_info_class = get_class("NSProcessInfo");
		void* string_class       = get_class("NSString");
		if (process_info_class == nullptr || string_class == nullptr) {
			std::printf("ThreadQos: Foundation classes not found\n");
			return false;
		}
		auto* send0 = reinterpret_cast<void* (*)(void*, void*)>(msg_send);
		auto* send_str = reinterpret_cast<void* (*)(void*, void*, const char*)>(msg_send);
		auto* send_act =
		    reinterpret_cast<void* (*)(void*, void*, unsigned long long, void*)>(msg_send);
		void* info   = send0(process_info_class, reg_sel("processInfo"));
		void* reason = send_str(string_class, reg_sel("stringWithUTF8String:"), "Kyty emulation");
		// NSActivityUserInitiated | NSActivityLatencyCritical
		constexpr unsigned long long options = 0x00FFFFFFULL | (1ULL << 20) | 0xFF00000000ULL;
		void* activity = send_act(info, reg_sel("beginActivityWithOptions:reason:"), options, reason);
		if (activity != nullptr) {
			send0(activity, reg_sel("retain")); // held for the life of the process
		}
		std::printf("ThreadQos: latency-critical activity %s\n", activity != nullptr ? "on" : "failed");
		return activity != nullptr;
	}();
	(void)done;
#endif
}

inline void ApplyCriticalThreadQos(const char* name) {
	if (!ThreadQosEnabled()) {
		return;
	}
#if defined(__APPLE__)
	const int r = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	std::printf("ThreadQos: %s -> USER_INTERACTIVE (%d)\n", name, r);
	BeginLatencyCriticalActivity();
#else
	(void)name;
#endif
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADQOS_H_
