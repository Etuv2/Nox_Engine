#include "CrashHandler.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace {

	LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
		const DWORD code = info->ExceptionRecord->ExceptionCode;
		std::fprintf(stderr, "\n[CrashHandler] Unhandled exception 0x%08lX at %p\n", code, info->ExceptionRecord->ExceptionAddress);
		if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
			std::fprintf(stderr, "[CrashHandler] Access violation %s address %p\n",
				info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
				reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
		}

		HANDLE process = GetCurrentProcess();
		HANDLE thread = GetCurrentThread();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(process, nullptr, TRUE);

		CONTEXT context = *info->ContextRecord;
		STACKFRAME64 frame = {};
		frame.AddrPC.Offset = context.Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = context.Rsp;
		frame.AddrStack.Mode = AddrModeFlat;

		alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
		for (int depth = 0; depth < 48; ++depth) {
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
				SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
				break;
			}
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;
			DWORD64 displacement = 0;
			const char* name = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) ? symbol->Name : "?";
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line)) {
				std::fprintf(stderr, "  #%02d %s  (%s:%lu)\n", depth, name, line.FileName, line.LineNumber);
			}
			else {
				std::fprintf(stderr, "  #%02d %s  [0x%llx]\n", depth, name, static_cast<unsigned long long>(frame.AddrPC.Offset));
			}
		}
		std::fflush(stderr);
		SymCleanup(process);
		return EXCEPTION_EXECUTE_HANDLER;
	}

} // namespace

namespace CrashHandler {
	void Install() {
		SetUnhandledExceptionFilter(OnUnhandledException);
	}
}
#else
namespace CrashHandler {
	void Install() {}
}
#endif
