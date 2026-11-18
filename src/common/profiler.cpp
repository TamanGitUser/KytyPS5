#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <string>
#include <tracy/Tracy.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Profiler {

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
	if (tracy::ProfilerAvailable()) {
		tracy::SetThreadName(name);
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Keep names visible to Windows host-thread sampling even when Tracy is disabled.
	using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_thread_description = reinterpret_cast<SetThreadDescriptionFn>(
	    GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
	if (set_thread_description != nullptr) {
		const int length =
		    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
		if (length > 1) {
			std::wstring description(static_cast<size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1,
			                    description.data(), length);
			description.pop_back();
			(void)set_thread_description(GetCurrentThread(), description.c_str());
		}
	}
#endif
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
