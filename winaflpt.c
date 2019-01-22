/*
WinAFL - Intel PT instrumentation and presistence via debugger code 
------------------------------------------------

Written and maintained by Ivan Fratric <ifratric@google.com>

Copyright 2016 Google Inc. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define  _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdbool.h>
#include "windows.h"
#include "psapi.h"
#include "dbghelp.h"

#include "libipt.h"
#include "ipttool.h"

#include "pt_cpu.h"
#include "pt_cpuid.h"
#include "intel-pt.h"

#include "types.h"
#include "config.h"
#include "debug.h"
#include "alloc-inl.h"

#include "winaflpt.h"

u64 get_cur_time(void);
char *argv_to_cmd(char** argv);

#define TRACE_BUFFER_SIZE_STR L"1048576" //should be a power of 2
#define MAX_TRACE_SIZE (64 * 1024 * 1024)

#define COVERAGE_BB 0
#define COVERAGE_EDGE 1

#define CALLCONV_MICROSOFT_X64 0
#define CALLCONV_THISCALL 1
#define CALLCONV_FASTCALL 2
#define CALLCONV_CDECL 3

#define BREAKPOINT_UNKNOWN 0
#define BREAKPOINT_ENTRYPOINT 1
#define BREAKPOINT_MODULELOADED 2
#define BREAKPOINT_FUZZMETHOD 3

#define WINAFL_LOOP_EXCEPTION 0x0AF1

#define DEBUGGER_PROCESS_EXIT 0
#define DEBUGGER_FUZZMETHOD_REACHED 1
#define DEBUGGER_FUZZMETHOD_END 2
#define DEBUGGER_CRASHED 3
#define DEBUGGER_HANGED 4

static HANDLE child_handle, child_thread_handle;
static HANDLE devnul_handle = INVALID_HANDLE_VALUE;
static int fuzz_iterations_current;

static DWORD fuzz_thread_id;

static DEBUG_EVENT dbg_debug_event;
static DWORD dbg_continue_status;
static bool dbg_continue_needed;
static uint64_t dbg_timeout_time;

static bool child_entrypoint_reached;
static bool collecting_trace;

static unsigned char *trace_buffer;
static size_t trace_buffer_size;
static size_t last_ring_buffer_offset;

extern u8 *trace_bits;
static uint64_t previous_offset;

extern HANDLE child_handle, child_thread_handle;
extern int fuzz_iterations_current;

extern HANDLE devnul_handle;
extern u8 sinkhole_stds;

extern u64 mem_limit;
extern u64 cpu_aff;

static FILE *debug_log = NULL;

#define USAGE_CHECK(condition, message) if(!(condition)) FATAL("%s\n", message);

enum {
	/* 00 */ FAULT_NONE,
	/* 01 */ FAULT_TMOUT,
	/* 02 */ FAULT_CRASH,
	/* 03 */ FAULT_ERROR,
	/* 04 */ FAULT_NOINST,
	/* 05 */ FAULT_NOBITS
};

typedef struct _target_module_t {
	char module_name[MAX_PATH];
	struct _target_module_t *next;
} target_module_t;

typedef struct _winafl_option_t {
	bool debug_mode;
	int coverage_kind;
	target_module_t *target_modules;
	char fuzz_module[MAX_PATH];
	char fuzz_method[MAX_PATH];
	unsigned long fuzz_offset;
	int fuzz_iterations;
	int num_fuz_args;
	int callconv;
	bool thread_coverage;

	void **func_args;
	void *sp;
	void *fuzz_address;
} winafl_option_t;
static winafl_option_t options;

struct winafl_breakpoint {
	void *address;
	int type;
	unsigned char original_opcode;
	char module_name[MAX_PATH];
	void *module_base;
	struct winafl_breakpoint *next;
};
struct winafl_breakpoint *breakpoints;

static void
winaflpt_options_init(int argc, const char *argv[])
{
	int i;
	const char *token;
	target_module_t *target_modules;
	/* default values */
	options.debug_mode = false;
	options.coverage_kind = COVERAGE_BB;
	options.target_modules = NULL;
	options.fuzz_module[0] = 0;
	options.fuzz_method[0] = 0;
	options.fuzz_offset = 0;
	options.fuzz_iterations = 1000;
	options.func_args = NULL;
	options.num_fuz_args = 0;
	options.thread_coverage = true;
#ifdef _WIN64
	options.callconv = CALLCONV_MICROSOFT_X64;
#else
	options.callconv = CALLCONV_CDECL;
#endif
	breakpoints = NULL;
	for (i = 0; i < argc; i++) {
		token = argv[i];
		if (strcmp(token, "-thread_coverage") == 0)
			options.thread_coverage = true;
		else if (strcmp(token, "-debug") == 0)
			options.debug_mode = true;
		else if (strcmp(token, "-covtype") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing coverage type");
			token = argv[++i];
			if (strcmp(token, "bb") == 0) options.coverage_kind = COVERAGE_BB;
			else if (strcmp(token, "edge") == 0) options.coverage_kind = COVERAGE_EDGE;
			else USAGE_CHECK(false, "invalid coverage type");
		}
		else if (strcmp(token, "-coverage_module") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing module");
			target_modules = options.target_modules;
			options.target_modules = (target_module_t *)malloc(sizeof(target_module_t));
			options.target_modules->next = target_modules;
			strncpy(options.target_modules->module_name, argv[++i], MAX_PATH);
		}
		else if (strcmp(token, "-target_module") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing module");
			strncpy(options.fuzz_module, argv[++i], MAX_PATH);
		}
		else if (strcmp(token, "-target_method") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing method");
			strncpy(options.fuzz_method, argv[++i], MAX_PATH);
		}
		else if (strcmp(token, "-fuzz_iterations") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing number of iterations");
			options.fuzz_iterations = atoi(argv[++i]);
		}
		else if (strcmp(token, "-nargs") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing number of arguments");
			options.num_fuz_args = atoi(argv[++i]);
		}
		else if (strcmp(token, "-target_offset") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing offset");
			options.fuzz_offset = strtoul(argv[++i], NULL, 0);
		}
		else if (strcmp(token, "-call_convention") == 0) {
			USAGE_CHECK((i + 1) < argc, "missing calling convention");
			++i;
			if (strcmp(argv[i], "stdcall") == 0)
				options.callconv = CALLCONV_CDECL;
			else if (strcmp(argv[i], "fastcall") == 0)
				options.callconv = CALLCONV_FASTCALL;
			else if (strcmp(argv[i], "thiscall") == 0)
				options.callconv = CALLCONV_THISCALL;
			else if (strcmp(argv[i], "ms64") == 0)
				options.callconv = CALLCONV_MICROSOFT_X64;
			else
				printf("Unknown calling convention, using default value instead.\n");
		}
		else {
			FATAL("UNRECOGNIZED OPTION: \"%s\"\n", token);
		}
	}

	if (options.fuzz_module[0] && (options.fuzz_offset == 0) && (options.fuzz_method[0] == 0)) {
		FATAL("If fuzz_module is specified, then either fuzz_method or fuzz_offset must be as well");
	}

	if (options.num_fuz_args) {
		options.func_args = (void **)malloc(options.num_fuz_args * sizeof(void *));
	}
}


inline static uint64_t sext(uint64_t val, uint8_t sign) {
	uint64_t signbit, mask;

	signbit = 1ull << (sign - 1);
	mask = ~0ull << sign;

	return val & signbit ? val | mask : val & ~mask;
}

// process a sinle IPT packet and update AFL map
inline static void process_packet(struct pt_packet *packet) {
	if (packet->type != ppt_tip) {
		return;
	}

	uint64_t ip;
	switch (packet->payload.ip.ipc) {
	case pt_ipc_update_16:
		ip = packet->payload.ip.ip & 0xFFFF;
		break;
	case pt_ipc_update_32:
		ip = packet->payload.ip.ip & 0xFFFFFFFF;
		break;
	case pt_ipc_update_48:
		ip = packet->payload.ip.ip & 0xFFFFFFFFFFFF;
		break;
	case pt_ipc_sext_48:
		ip = sext(packet->payload.ip.ip, 48);
		break;
	case pt_ipc_full:
		ip = packet->payload.ip.ip;
		break;
	default:
		return;
	}

	// todo implement coverage_module filter and 
	// subtract module start from IP
	// for the default MAP_SIZE of 65536 the below works
	// due to module alignment but it won't for larger MAP_SIZE

	switch (options.coverage_kind) {
	case COVERAGE_BB:
		trace_bits[ip % MAP_SIZE]++;
		break;
	case COVERAGE_EDGE:
		ip = ip % MAP_SIZE;
		trace_bits[ip ^ previous_offset]++;
		previous_offset = ip >> 1;
		break;
	}
	// printf("ip: %p   %d\n", (void *)ip, (int)packet->payload.ip.ipc);
}

// analyze collected PT trace
void analyze_trace_buffer(unsigned char *trace_data, size_t trace_size) {
	// printf("analyzing trace\n");

	struct pt_packet_decoder *decoder;
	struct pt_config ptc;
	struct pt_packet packet;

	pt_config_init(&ptc);
	pt_cpu_read(&ptc.cpu);
	pt_cpu_errata(&ptc.errata, &ptc.cpu);
	ptc.begin = trace_data;
	ptc.end = trace_data + trace_size;

	decoder = pt_pkt_alloc_decoder(&ptc);
	if (!decoder) {
		FATAL("Error allocating decoder\n");
	}

	for (;;) {
		if (pt_pkt_sync_forward(decoder) < 0) {
			// printf("No more sync packets\n");
			break;
		}

		for (;;) {
			if (pt_pkt_next(decoder, &packet, sizeof(packet)) < 0) {
				// printf("Error reding packet\n");
				break;
			}

			process_packet(&packet);
		}
	}

	pt_pkt_free_decoder(decoder);
}

// appends new data to the trace_buffer
void append_trace_data(unsigned char *trace_data, size_t trace_size) {
	size_t space_left = MAX_TRACE_SIZE - trace_buffer_size;

	if (!space_left) {
		// stop collecting trace if the trace buffer is full;
		printf("Warning: Trace buffer is full\n");
		collecting_trace = 0;
		return;
	}

	if (trace_size > space_left) {
		trace_size = space_left;
	}

	if (trace_size == 0) return;

	memcpy(trace_buffer + trace_buffer_size, trace_data, trace_size);
	trace_buffer_size += trace_size;
}

// parse PIPT_TRACE_DATA, extract trace bits and add them to the trace_buffer
int collect_trace(PIPT_TRACE_DATA pTraceData)
{
	PIPT_TRACE_HEADER traceHeader;
	DWORD dwTraceSize;

	dwTraceSize = pTraceData->TraceSize;

	traceHeader = (PIPT_TRACE_HEADER)pTraceData->TraceData;

	while (dwTraceSize > (unsigned)(FIELD_OFFSET(IPT_TRACE_HEADER, Trace))) {
		if (traceHeader->ThreadId == fuzz_thread_id) {

			// printf("current ring offset: %u\n", traceHeader->RingBufferOffset);

			// append trace to trace_buffer
			if (traceHeader->RingBufferOffset > last_ring_buffer_offset) {
				append_trace_data(traceHeader->Trace + last_ring_buffer_offset, traceHeader->RingBufferOffset - last_ring_buffer_offset);
			} else if(traceHeader->RingBufferOffset < last_ring_buffer_offset) {
				append_trace_data(traceHeader->Trace + last_ring_buffer_offset, traceHeader->TraceSize - last_ring_buffer_offset);
				append_trace_data(traceHeader->Trace, traceHeader->RingBufferOffset);
			}

			last_ring_buffer_offset = traceHeader->RingBufferOffset;
		}

		dwTraceSize -= (FIELD_OFFSET(IPT_TRACE_HEADER, Trace) + traceHeader->TraceSize);

		traceHeader = (PIPT_TRACE_HEADER)(traceHeader->Trace +
			traceHeader->TraceSize);
	}

	return 0;
}

// returns an array of handles for all modules loaded in the target process
DWORD get_all_modules(HMODULE **modules) {
	DWORD module_handle_storage_size = 1024 * sizeof(HMODULE);
	HMODULE *module_handles = (HMODULE *)malloc(module_handle_storage_size);
	DWORD hmodules_size;
	while (true) {
		if (!EnumProcessModules(child_handle, module_handles, module_handle_storage_size, &hmodules_size)) {
			FATAL("EnumProcessModules failed, %x\n", GetLastError());
		}
		if (hmodules_size <= module_handle_storage_size) break;
		module_handle_storage_size *= 2;
		module_handles = (HMODULE *)realloc(module_handles, module_handle_storage_size);
	}
	*modules = module_handles;
	return hmodules_size / sizeof(HMODULE);
}

// parses PE headers and gets the module entypoint
void *get_entrypoint(void *base_address) {
	unsigned char headers[4096];
	size_t num_read = 0;
	if (!ReadProcessMemory(child_handle, base_address, headers, 4096, &num_read) || (num_read != 4096)) {
		FATAL("Error reading target memory\n");
	}
	DWORD pe_offset;
	pe_offset = *((DWORD *)(headers + 0x3C));
	char *pe = headers + pe_offset;
	DWORD signature = *((DWORD *)pe);
	if (signature != 0x00004550) {
		FATAL("PE signature error\n");
	}
	pe = pe + 0x18;
	WORD magic = *((WORD *)pe);
	if ((magic != 0x10b) && (magic != 0x20b)) {
		FATAL("Unknown PE magic value\n");
	} 
	DWORD entrypoint_offset = *((DWORD *)(pe + 16));
	return (char *)base_address + entrypoint_offset;
}

// adds a breakpoint at a specified address
// type, module_name and module_base are all additional information
// that can be accessed later when the breakpoint gets hit
void add_breakpoint(void *address, int type, char *module_name, void *module_base) {
	struct winafl_breakpoint *new_breakpoint = (struct winafl_breakpoint *)malloc(sizeof(struct winafl_breakpoint));
	size_t rwsize = 0;
	if(!ReadProcessMemory(child_handle, address, &(new_breakpoint->original_opcode), 1, &rwsize) || (rwsize != 1)) {
		FATAL("Error reading target memory\n");
	}
	rwsize = 0;	
	unsigned char cc = 0xCC;
	if (!WriteProcessMemory(child_handle, address, &cc, 1, &rwsize) || (rwsize != 1)) {
		FATAL("Error writing target memory\n");
	}
	FlushInstructionCache(child_handle, address, 1);
	new_breakpoint->address = address;
	new_breakpoint->type = type;
	if (module_name) {
		strcpy(new_breakpoint->module_name, module_name);
	} else {
		new_breakpoint->module_name[0] = 0;
	}
	new_breakpoint->module_base = module_base;
	new_breakpoint->next = breakpoints;
	breakpoints = new_breakpoint;
}


// damn it Windows, why don't you have a GetProcAddress
// that works on another process
DWORD get_proc_offset(char *data, char *name) {
	DWORD pe_offset;
	pe_offset = *((DWORD *)(data + 0x3C));
	char *pe = data + pe_offset;
	DWORD signature = *((DWORD *)pe);
	if (signature != 0x00004550) {
		return 0;
	}
	pe = pe + 0x18;
	WORD magic = *((WORD *)pe);
	DWORD exporttableoffset;
	if (magic == 0x10b) {
		exporttableoffset = *(DWORD *)(pe + 96);
	} else if (magic == 0x20b) {
		exporttableoffset = *(DWORD *)(pe + 112);
	} else {
		return 0;
	}

	if (!exporttableoffset) return 0;
	char *exporttable = data + exporttableoffset;

	DWORD numentries = *(DWORD *)(exporttable + 24);
	DWORD addresstableoffset = *(DWORD *)(exporttable + 28);
	DWORD nameptrtableoffset = *(DWORD *)(exporttable + 32);
	DWORD ordinaltableoffset = *(DWORD *)(exporttable + 36);
	DWORD *nameptrtable = (DWORD *)(data + nameptrtableoffset);
	WORD *ordinaltable = (WORD *)(data + ordinaltableoffset);
	DWORD *addresstable = (DWORD *)(data + addresstableoffset);

	DWORD i;
	for (i = 0; i < numentries; i++) {
		char *nameptr = data + nameptrtable[i];
		if (strcmp(name, nameptr) == 0) break;
	}

	if (i == numentries) return 0;

	WORD oridnal = ordinaltable[i];
	DWORD offset = addresstable[oridnal];

	return offset;
}

// attempt to obtain the fuzz_offset in various ways
char *get_fuzz_method_offset(HMODULE module) {
	// if fuzz_offset is defined, use that
	if (options.fuzz_offset) {
		return (char *)module + options.fuzz_offset;
	}

	// try the exported symbols next
	MODULEINFO module_info;
	GetModuleInformation(child_handle, module, &module_info, sizeof(module_info));
	BYTE *modulebuf = (BYTE *)malloc(module_info.SizeOfImage);
	size_t num_read;
	if (!ReadProcessMemory(child_handle, (LPCVOID)module, modulebuf, module_info.SizeOfImage, &num_read) || (num_read != module_info.SizeOfImage)) {
		FATAL("Error reading target memory\n");
	}
	DWORD fuzz_offset = get_proc_offset(modulebuf, options.fuzz_method);
	free(modulebuf);
	if (fuzz_offset) {
		return (char *)module + fuzz_offset;
	}

	// finally, try the debug symbols
	char *fuzz_method = NULL;
	char base_name[MAX_PATH];
	GetModuleBaseNameA(child_handle, module, (LPSTR)(&base_name), sizeof(base_name));

	char module_path[MAX_PATH];
	if(!GetModuleFileNameExA(child_handle, module, module_path, sizeof(module_path))) return NULL;
	
	ULONG64 buffer[(sizeof(SYMBOL_INFO) +
		MAX_SYM_NAME * sizeof(TCHAR) +
		sizeof(ULONG64) - 1) /
		sizeof(ULONG64)];
	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = MAX_SYM_NAME;
	SymInitialize(child_handle, NULL, false);
	SymLoadModuleEx(child_handle, NULL, module_path, base_name, (DWORD64)module, module_info.SizeOfImage, NULL, 0);
	if (SymFromName(child_handle, options.fuzz_method, pSymbol)) {
		fuzz_method =(char *)pSymbol->Address;
	}
	SymCleanup(child_handle);

	return fuzz_method;
}

// called when a potentialy interesting module gets loaded
void on_module_loaded(HMODULE module, char *module_name) {
	// printf("In OnModuleLoaded, name: %s, base: %p\n", module_name, (void *)module);
	if (_stricmp(module_name, options.fuzz_module) == 0) {
		char * fuzz_address = get_fuzz_method_offset(module);
		if (!fuzz_address) {
			FATAL("Error determining target method address\n");
		}

		// printf("Fuzz method address: %p\n", fuzz_address);
		options.fuzz_address = fuzz_address;

		add_breakpoint(fuzz_address, BREAKPOINT_FUZZMETHOD, NULL, 0);
	}
}

// called when the target method is called *for the first time only*
void on_target_method(DWORD thread_id) {
	// printf("in OnTargetMethod\n");

	fuzz_thread_id = thread_id;

	size_t numrw = 0;

	CONTEXT lcContext;
	lcContext.ContextFlags = CONTEXT_ALL;
	HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
	GetThreadContext(thread_handle, &lcContext);

	// read out and save the params
#ifdef _WIN64
	options.sp = (void *)lcContext.Rsp;
#else
	options.sp = (void *)lcContext.Esp;
#endif

	switch (options.callconv) {
#ifdef _WIN64
	case CALLCONV_MICROSOFT_X64:
		if (options.num_fuz_args > 0) options.func_args[0] = (void *)lcContext.Rcx;
		if (options.num_fuz_args > 1) options.func_args[1] = (void *)lcContext.Rdx;
		if (options.num_fuz_args > 2) options.func_args[2] = (void *)lcContext.R8;
		if (options.num_fuz_args > 3) options.func_args[3] = (void *)lcContext.R9;
		if (options.num_fuz_args > 4) {
			ReadProcessMemory(child_handle, (LPCVOID)(lcContext.Rsp + 5 * sizeof(void *)), options.func_args + 4, (options.num_fuz_args - 4) * sizeof(void *), &numrw);
		}
		break;
#else
	case CALLCONV_CDECL:
		if (options.num_fuz_args > 0) {
			ReadProcessMemory(child_handle, (LPCVOID)(lcContext.Esp + sizeof(void *)), options.func_args, (options.num_fuz_args) * sizeof(void *), &numrw);
		}
		break;
	case CALLCONV_FASTCALL:
		if (options.num_fuz_args > 0) options.func_args[0] = (void *)lcContext.Ecx;
		if (options.num_fuz_args > 1) options.func_args[1] = (void *)lcContext.Edx;
		if (options.num_fuz_args > 3) {
			ReadProcessMemory(child_handle, (LPCVOID)(lcContext.Esp + sizeof(void *)), options.func_args + 2, (options.num_fuz_args - 2) * sizeof(void *), &numrw);
		}
		break;
	case CALLCONV_THISCALL:
		if (options.num_fuz_args > 0) options.func_args[0] = (void *)lcContext.Ecx;
		if (options.num_fuz_args > 3) {
			ReadProcessMemory(child_handle, (LPCVOID)(lcContext.Esp + sizeof(void *)), options.func_args + 1, (options.num_fuz_args - 1) * sizeof(void *), &numrw);
		}
		break;
#endif
	default:
		break;
	}

	// todo store any target-specific additional context here

	// modify the return address on the stack so that an exception is triggered
	// when the target function finishes executing
	// another option would be to allocate a block of executable memory
	// and point return address over there, but this is quicker
	size_t return_address = WINAFL_LOOP_EXCEPTION;
	WriteProcessMemory(child_handle, options.sp, &return_address, sizeof(void *), &numrw);

	CloseHandle(thread_handle);
}

// called every time the target method returns
void on_target_method_ended(DWORD thread_id) {
	// printf("in OnTargetMethodEnded\n");

	size_t numrw = 0;

	CONTEXT lcContext;
	lcContext.ContextFlags = CONTEXT_ALL;
	HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
	GetThreadContext(thread_handle, &lcContext);

	// restore params
#ifdef _WIN64
	lcContext.Rip = (size_t)options.fuzz_address;
	lcContext.Rsp = (size_t)options.sp;
#else
	lcContext.Eip = (size_t)options.fuzz_address;
	lcContext.Esp = (size_t)options.sp;
#endif

	switch (options.callconv) {
#ifdef _WIN64
	case CALLCONV_MICROSOFT_X64:
		if (options.num_fuz_args > 0) lcContext.Rcx = (size_t)options.func_args[0];
		if (options.num_fuz_args > 1) lcContext.Rdx = (size_t)options.func_args[1];
		if (options.num_fuz_args > 2) lcContext.R8 = (size_t)options.func_args[2];
		if (options.num_fuz_args > 3) lcContext.R9 = (size_t)options.func_args[3];
		if (options.num_fuz_args > 4) {
			WriteProcessMemory(child_handle, (LPVOID)(lcContext.Rsp + 5 * sizeof(void *)), options.func_args + 4, (options.num_fuz_args - 4) * sizeof(void *), &numrw);
		}
		break;
#else
	case CALLCONV_CDECL:
		if (options.num_fuz_args > 0) {
			WriteProcessMemory(child_handle, (LPVOID)(lcContext.Esp + sizeof(void *)), options.func_args, (options.num_fuz_args) * sizeof(void *), &numrw);
		}
		break;
	case CALLCONV_FASTCALL:
		if (options.num_fuz_args > 0) lcContext.Ecx = (size_t)options.func_args[0];
		if (options.num_fuz_args > 1) lcContext.Edx = (size_t)options.func_args[1];
		if (options.num_fuz_args > 3) {
			WriteProcessMemory(child_handle, (LPVOID)(lcContext.Esp + sizeof(void *)), options.func_args + 2, (options.num_fuz_args - 2) * sizeof(void *), &numrw);
		}
		break;
	case CALLCONV_THISCALL:
		if (options.num_fuz_args > 0) lcContext.Ecx = (size_t)options.func_args[0];
		if (options.num_fuz_args > 3) {
			WriteProcessMemory(child_handle, (LPVOID)(lcContext.Esp + sizeof(void *)), options.func_args + 1, (options.num_fuz_args - 1) * sizeof(void *), &numrw);
		}
		break;
#endif
	default:
		break;
	}

	// todo restore any target-specific additional context here

	SetThreadContext(thread_handle, &lcContext);
	CloseHandle(thread_handle);
}

// called when process entrypoint gets reached
void on_entrypoint() {
	// printf("Entrypoint\n");

	HMODULE *module_handles = NULL;
	DWORD num_modules = get_all_modules(&module_handles);
	for (DWORD i = 0; i < num_modules; i++) {
		char base_name[MAX_PATH];
		GetModuleBaseNameA(child_handle, module_handles[i], (LPSTR)(&base_name), sizeof(base_name));
		if(options.debug_mode) fprintf(debug_log, "Module loaded: %s\n", base_name);
		on_module_loaded(module_handles[i], base_name);
	}
	if(module_handles) free(module_handles);

	child_entrypoint_reached = true;
}

// called when the debugger hits a breakpoint
int handle_breakpoint(void *address, DWORD thread_id) {
	int ret = BREAKPOINT_UNKNOWN;
	size_t rwsize = 0;
	struct winafl_breakpoint *previous = NULL;
	struct winafl_breakpoint *current = breakpoints;
	while (current) {
		if (current->address == address) {
			// unlink the breakpoint
			if (previous) previous->next = current->next;
			else breakpoints = current->next;
			// restore address
			if (!WriteProcessMemory(child_handle, address, &current->original_opcode, 1, &rwsize) || (rwsize != 1)) {
				FATAL("Error writing child memory\n");
			}
			FlushInstructionCache(child_handle, address, 1);
			// restore context
			CONTEXT lcContext;
			lcContext.ContextFlags = CONTEXT_ALL;
			HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
			GetThreadContext(thread_handle, &lcContext);
#ifdef _WIN64
			lcContext.Rip--;
#else
			lcContext.Eip--;
#endif
			SetThreadContext(thread_handle, &lcContext);
			CloseHandle(thread_handle);
			// handle breakpoint
			switch (current->type) {
			case BREAKPOINT_ENTRYPOINT:
				on_entrypoint();
				break;
			case BREAKPOINT_MODULELOADED:
				on_module_loaded((HMODULE)current->module_base, current->module_name);
				break;
			case BREAKPOINT_FUZZMETHOD:
				on_target_method(thread_id);
				break;
			default:
				break;
			}
			// return the brekpoint type
			ret = current->type;
			// delete the breakpoint object
			free(current);
			//done
			break;
		}
		previous = current;
		current = current->next;
	}
	return ret;
}

// standard debugger loop that listens to relevant events in the target process
int debug_loop()
{
	LPDEBUG_EVENT DebugEv = &dbg_debug_event;
	DWORD wait_time;

	for (;;)
	{

		if (collecting_trace) wait_time = 0;
		else wait_time = 100;

		BOOL wait_ret = WaitForDebugEvent(DebugEv, wait_time);

		// printf("time: %lld\n", get_cur_time_us());

		if (collecting_trace) {
			PIPT_TRACE_DATA trace_data = GetIptTrace(child_handle);
			if (!trace_data) {
				printf("Error getting ipt trace\n");
			} else {
				collect_trace(trace_data);
				HeapFree(GetProcessHeap(), 0, trace_data);
			}
		}

		if (wait_ret) {
			dbg_continue_needed = true;
		} else {
			dbg_continue_needed = false;
		}

		if (get_cur_time() > dbg_timeout_time) return DEBUGGER_HANGED;

		if (!wait_ret) {
			//printf("WaitForDebugEvent returned 0\n");
			continue;
		}

		dbg_continue_status = DBG_CONTINUE;

		// printf("eventCode: %x\n", DebugEv->dwDebugEventCode);

		switch (DebugEv->dwDebugEventCode)
		{
		case EXCEPTION_DEBUG_EVENT:
			// printf("exception code: %x\n", DebugEv->u.Exception.ExceptionRecord.ExceptionCode);

			switch (DebugEv->u.Exception.ExceptionRecord.ExceptionCode)
			{
			case EXCEPTION_BREAKPOINT:
			case 0x4000001f: //STATUS_WX86_BREAKPOINT
			{
				void *address = DebugEv->u.Exception.ExceptionRecord.ExceptionAddress;
				// printf("Breakpoint at address %p\n", address);
				int breakpoint_type = handle_breakpoint(address, DebugEv->dwThreadId);
				if (breakpoint_type == BREAKPOINT_UNKNOWN) {
					dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
				} else if (breakpoint_type == BREAKPOINT_FUZZMETHOD) {
					dbg_continue_status = DBG_CONTINUE;
					return DEBUGGER_FUZZMETHOD_REACHED;
				} else {
					dbg_continue_status = DBG_CONTINUE;
				}
				break;
			}

			case EXCEPTION_ACCESS_VIOLATION: {
				if ((size_t)DebugEv->u.Exception.ExceptionRecord.ExceptionAddress == WINAFL_LOOP_EXCEPTION) {
					on_target_method_ended(DebugEv->dwThreadId);
					dbg_continue_status = DBG_CONTINUE;
					return DEBUGGER_FUZZMETHOD_END;
				} else {
					dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
					return DEBUGGER_CRASHED;
				}
				break;
			}

			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_STACK_OVERFLOW:
			case STATUS_HEAP_CORRUPTION:
			case STATUS_STACK_BUFFER_OVERRUN:
			case STATUS_FATAL_APP_EXIT:
				dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
				return DEBUGGER_CRASHED;
				break;

			default:
				dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
				break;
			}

			break;

		case CREATE_THREAD_DEBUG_EVENT:
			break;

		case CREATE_PROCESS_DEBUG_EVENT: {
			// add a brekpoint to the process entrypoint
			void *entrypoint = get_entrypoint(DebugEv->u.CreateProcessInfo.lpBaseOfImage);
			add_breakpoint(entrypoint, BREAKPOINT_ENTRYPOINT, NULL, 0);
			CloseHandle(DebugEv->u.CreateProcessInfo.hFile);
			break;
		}

		case EXIT_THREAD_DEBUG_EVENT:
			break;

		case EXIT_PROCESS_DEBUG_EVENT:
			return DEBUGGER_PROCESS_EXIT;

		case LOAD_DLL_DEBUG_EVENT: {
			// Don't do anything until the processentrypoint is reached.
			// Before that time we can't do much anyway, a lot of calls are going to fail
			// Modules loaded before entrypoint is reached are going to be enumerated at that time
			if (child_entrypoint_reached) {
				char filename[MAX_PATH];
				GetFinalPathNameByHandleA(DebugEv->u.LoadDll.hFile, (LPSTR)(&filename), sizeof(filename), 0);
				char *base_name = strrchr(filename, '\\');
				if (base_name) base_name += 1;
				else base_name = filename;
				// printf("Module loaded: %s %p\n", base_name, DebugEv->u.LoadDll.lpBaseOfDll);
				if (options.debug_mode) fprintf(debug_log, "Module loaded: %s\n", base_name);
				// module isn't fully loaded yet. Instead of processing it now,
				// add a breakpoint to the module's entrypoint
				if (_stricmp(base_name, options.fuzz_module) == 0) {
					void *entrypoint = get_entrypoint(DebugEv->u.LoadDll.lpBaseOfDll);
					add_breakpoint(entrypoint, BREAKPOINT_MODULELOADED, base_name, DebugEv->u.LoadDll.lpBaseOfDll);
				}
			}
			CloseHandle(DebugEv->u.LoadDll.hFile);
			break;
		}

		case UNLOAD_DLL_DEBUG_EVENT:
			break;

		case OUTPUT_DEBUG_STRING_EVENT:
			break;

		case RIP_EVENT:
			break;
		}

		ContinueDebugEvent(DebugEv->dwProcessId,
			DebugEv->dwThreadId,
			dbg_continue_status);
	}
}

// a simpler debugger loop that just waits for the process to exit
void wait_process_exit()
{
	LPDEBUG_EVENT DebugEv = &dbg_debug_event;

	for (;;)
	{
		dbg_continue_status = DBG_CONTINUE;

		if (!WaitForDebugEvent(DebugEv, 100)) {
			continue;
		}

		//printf("eventCode: %x\n", DebugEv->dwDebugEventCode);

		switch (DebugEv->dwDebugEventCode)
		{
		case EXCEPTION_DEBUG_EVENT:
			dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
			break;

		case CREATE_PROCESS_DEBUG_EVENT:
			CloseHandle(DebugEv->u.CreateProcessInfo.hFile);
			break;

		case EXIT_PROCESS_DEBUG_EVENT:
			return;

		case LOAD_DLL_DEBUG_EVENT:
			CloseHandle(DebugEv->u.LoadDll.hFile);
			break;

		default:
			break;
		}

		ContinueDebugEvent(DebugEv->dwProcessId,
			DebugEv->dwThreadId,
			dbg_continue_status);
	}
}

// starts the target process
void start_process(char *cmd) {
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HANDLE hJob = NULL;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit;

	if (sinkhole_stds && devnul_handle == INVALID_HANDLE_VALUE) {
		devnul_handle = CreateFile(
			"nul",
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (devnul_handle == INVALID_HANDLE_VALUE) {
			PFATAL("Unable to open the nul device.");
		}
	}
	BOOL inherit_handles = TRUE;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// todo the below is duplicating code from afl-fuzz.c a lot
	// this should be taken out to a separate function
	if (sinkhole_stds) {
		si.hStdOutput = si.hStdError = devnul_handle;
		si.dwFlags |= STARTF_USESTDHANDLES;
	}
	else {
		inherit_handles = FALSE;
	}

	if (mem_limit || cpu_aff) {
		hJob = CreateJobObject(NULL, NULL);
		if (hJob == NULL) {
			FATAL("CreateJobObject failed, GLE=%d.\n", GetLastError());
		}

		ZeroMemory(&job_limit, sizeof(job_limit));
		if (mem_limit) {
			job_limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
			job_limit.ProcessMemoryLimit = mem_limit * 1024 * 1024;
		}

		if (cpu_aff) {
			job_limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_AFFINITY;
			job_limit.BasicLimitInformation.Affinity = (DWORD_PTR)cpu_aff;
		}

		if (!SetInformationJobObject(
			hJob,
			JobObjectExtendedLimitInformation,
			&job_limit,
			sizeof(job_limit)
		)) {
			FATAL("SetInformationJobObject failed, GLE=%d.\n", GetLastError());
		}
	}

	if (!CreateProcessA(NULL, cmd, NULL, NULL, inherit_handles, DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
		FATAL("CreateProcess failed, GLE=%d.\n", GetLastError());
	}

	child_handle = pi.hProcess;
	child_thread_handle = pi.hThread;
	child_entrypoint_reached = false;

	if (mem_limit || cpu_aff) {
		if (!AssignProcessToJobObject(hJob, child_handle)) {
			FATAL("AssignProcessToJobObject failed, GLE=%d.\n", GetLastError());
		}
	}

	fuzz_iterations_current = 0;
	collecting_trace = false;

	BOOL wow64current, wow64remote;
	if (!IsWow64Process(child_handle, &wow64remote)) {
		FATAL("IsWow64Process failed");
	}
	if (!IsWow64Process(GetCurrentProcess(), &wow64current)) {
		FATAL("IsWow64Process failed");
	}
	if (wow64current != wow64remote) {
		FATAL("Use 64-bit WinAFL build to fuzz 64-bit targets and 32-bit build to fuzz 32-bit targets");
	}
}

// called to resume the target process if it is waiting on a debug event
void resumes_process() {
	ContinueDebugEvent(dbg_debug_event.dwProcessId,
		dbg_debug_event.dwThreadId,
		dbg_continue_status);
}

void kill_process() {
	TerminateProcess(child_handle, 0);

	if(dbg_continue_needed) resumes_process();

	wait_process_exit();

	CloseHandle(child_handle);
	CloseHandle(child_thread_handle);

	child_handle = NULL;
	child_thread_handle = NULL;
}

int run_target_pt(char **argv, uint32_t timeout) {
	int debugger_status;
	int ret;

	if (!child_handle) {

		char *cmd = argv_to_cmd(argv);
		start_process(cmd);
		ck_free(cmd);

		// wait until the target method is reached
		dbg_timeout_time = get_cur_time() + timeout;
		debugger_status = debug_loop();

		if (debugger_status != DEBUGGER_FUZZMETHOD_REACHED) {
			switch (debugger_status) {
			case DEBUGGER_CRASHED:
				FATAL("Process crashed before reaching the target method\n");
				break;
			case DEBUGGER_HANGED:
				FATAL("Process hanged before reaching the target method\n");
				break;
			case DEBUGGER_PROCESS_EXIT:
				FATAL("Process exited before reaching the target method\n");
				break;
			default:
				FATAL("An unknown problem occured before reaching the target method\n");
				break;
			}
		}
	}

	if(options.debug_mode) fprintf(debug_log, "iteration %d\n", fuzz_iterations_current);

	// start tracing
	IPT_OPTIONS ipt_options;
	memset(&ipt_options, 0, sizeof(IPT_OPTIONS));
	ipt_options.OptionVersion = 1;
	ConfigureBufferSize((PWCHAR)(TRACE_BUFFER_SIZE_STR), &ipt_options);
	ConfigureTraceFlags((PWCHAR)(L"0"), &ipt_options);
	if (!StartProcessIptTracing(child_handle, ipt_options)) {
		FATAL("ipt tracing error\n");
	}

	collecting_trace = true;
	trace_buffer_size = 0;
	last_ring_buffer_offset = 0;

	memset(trace_bits, 0, MAP_SIZE);
	previous_offset = 0;

	dbg_timeout_time = get_cur_time() + timeout;

	resumes_process();
	debugger_status = debug_loop();

	collecting_trace = false;

	// end tracing
	if (!StopProcessIptTracing(child_handle)) {
		printf("Error stopping ipt trace\n");
	}

	analyze_trace_buffer(trace_buffer, trace_buffer_size);

	if (debugger_status == DEBUGGER_PROCESS_EXIT) {
		CloseHandle(child_handle);
		CloseHandle(child_thread_handle);
		child_handle = NULL;
		child_thread_handle = NULL;
		ret = FAULT_TMOUT; //treat it as a hang
	} else if (debugger_status == DEBUGGER_HANGED) {
		kill_process();
		ret = FAULT_TMOUT;
	} else if (debugger_status == DEBUGGER_CRASHED) {
		kill_process();
		ret = FAULT_CRASH;
	} else if (debugger_status == DEBUGGER_FUZZMETHOD_END) {
		ret = FAULT_NONE;
	}

	fuzz_iterations_current++;
	if (fuzz_iterations_current == options.fuzz_iterations && child_handle != NULL) {
		kill_process();
	}

	return ret;
}

int pt_init(int argc, char **argv) {
	child_handle = NULL;
	child_thread_handle = NULL;

	int lastoption = -1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			lastoption = i;
			break;
		}
	}

	if (lastoption <= 0) return 0;

	winaflpt_options_init(lastoption - 1, argv + 1);
	trace_buffer = (unsigned char *)malloc(MAX_TRACE_SIZE);

	if (!EnableAndValidateIptServices()) {
		FATAL("No IPT\n");
	} else {
		printf("IPT service enebled\n");
	}

	if (options.debug_mode) {
		debug_log = fopen("debug.log", "w");
		if (!debug_log) {
			FATAL("Can't open debug log for writing");
		}
	}

	return lastoption;
}

void debug_target_pt(char **argv) {
	trace_bits = (u8 *)malloc(MAP_SIZE);

	for (int i = 0; i < options.fuzz_iterations; i++) {
		int ret = run_target_pt(argv, 0xFFFFFFFF);
		switch (ret) {
		case FAULT_NONE:
			if(debug_log) fprintf(debug_log, "Iteration finished normally\n");
			break;
		case FAULT_CRASH:
			if (debug_log) fprintf(debug_log, "Target crashed\n");
			break;
		case FAULT_TMOUT:
			if (debug_log) fprintf(debug_log, "Target hanged\n");
			break;
		}
	}

	if (debug_log) {
		fprintf(debug_log, "Coverage map (hex): \n");
		size_t map_pos = 0;
		while (1) {
			for (int i = 0; i < 16; i++) {
				if (map_pos == MAP_SIZE) break;
				fprintf(debug_log, "%02X", trace_bits[map_pos]);
				map_pos++;
			}
			fprintf(debug_log, "\n");
			if (map_pos == MAP_SIZE) break;
		}
	}

	if (debug_log) fclose(debug_log);
}
