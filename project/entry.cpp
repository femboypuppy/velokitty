#include <pch/pch.hpp>

#include <cstdio>
#include <TlHelp32.h>

#include <utilities/logging/logging.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/security/security.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/threadpool/threadpool.hpp>
#include <utilities/steam/steam.hpp>
#include <utilities/unload.hpp>

#include <core/hooks/hooks.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <core/rendering/rendering.hpp>

#include <utilities/diag.hpp>

namespace {

	std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_exception_filter{};
	PVOID g_vectored_exception_handler{};

	LONG WINAPI diag_unhandled_exception_filter( EXCEPTION_POINTERS* info );

	// Not DEV-only. A Source 2 fatal error ends in TerminateProcess, and without this hook a shipped build
	// dies with nothing in the log but the last thing it happened to be doing -- which is how "it just
	// closes" crashes stayed invisible.
	hooking::jmp g_terminate_process_hook{};

#if defined( DEV )
	hooking::jmp g_minidump_hook{};

	BOOL WINAPI diag_minidump_write_detour(
		HANDLE process,
		DWORD process_id,
		HANDLE file,
		unsigned long dump_type,
		diag::minidump_exception_information* exception,
		void* user_stream,
		void* callback )
	{
		const auto original =
			g_minidump_hook.original<diag::minidump_write_fn>( );
		if ( !diag::g_writing_minidump &&
			exception &&
			!exception->client_pointers &&
			exception->exception_pointers )
		{
			diag::record_crash(
				exception->exception_pointers,
				"game fatal handler (Source 2 caught the exception)" );
		}

		return original
			? original(
				process,
				process_id,
				file,
				dump_type,
				exception,
				user_stream,
				callback )
			: FALSE;
	}

	bool install_game_crash_capture( )
	{
		if ( !diag::g_minidump_write )
		{
			return false;
		}

		if ( !hooking::manager::create( {
				{
					&g_minidump_hook,
					reinterpret_cast<void*>( diag_minidump_write_detour ),
					"dbghelp!MiniDumpWriteDump",
					reinterpret_cast<std::uintptr_t>( diag::g_minidump_write )
				}
			} ) )
		{
			diag::write(
				diag::level::warning,
				"failed to hook the Source 2 minidump path; "
				"only self/unhandled crashes will be captured" );
			return false;
		}

		diag::write(
			diag::level::info,
			"Source 2 fatal minidump path hooked" );
		return true;
	}
#endif

	BOOL WINAPI diag_terminate_process_detour(
		HANDLE process,
		UINT exit_code )
	{
		const auto original =
			g_terminate_process_hook.original<decltype( &TerminateProcess )>( );

		if ( GetProcessId( process ) == GetCurrentProcessId( ) )
		{
			// The line goes out first and on its own. capture_snapshot routes through record_crash, which is
			// one-shot and spawns a worker -- if a real fault already claimed it, or the loader is too far
			// gone to start a thread, at least the reason the process died is on disk.
			diag::writef(
				diag::level::fatal,
				"TerminateProcess requested for CS2; exit_code=0x%08X, phase=%s",
				exit_code,
				diag::g_exception_phase );

			char stage[ 96 ]{};
			_snprintf_s(
				stage,
				sizeof( stage ),
				_TRUNCATE,
				"TerminateProcess requested for CS2; exit_code=0x%08X",
				exit_code );
			diag::capture_snapshot( stage );
		}

		return original ? original( process, exit_code ) : FALSE;
	}

	bool install_termination_capture( )
	{
		const auto kernel32 = GetModuleHandleW( L"kernel32.dll" );
		const auto terminate_process = kernel32
			? GetProcAddress( kernel32, "TerminateProcess" )
			: nullptr;
		if ( !terminate_process ||
			!hooking::manager::create( {
				{
					&g_terminate_process_hook,
					reinterpret_cast<void*>( diag_terminate_process_detour ),
					"kernel32!TerminateProcess",
					reinterpret_cast<std::uintptr_t>( terminate_process )
				}
			} ) )
		{
			diag::write(
				diag::level::warning,
				"failed to hook forced process termination" );
			return false;
		}

		diag::write(
			diag::level::info,
			"forced process termination capture hooked" );
		return true;
	}

	LONG CALLBACK diag_vectored_exception_filter( EXCEPTION_POINTERS* info )
	{
		if ( !info || !info->ExceptionRecord ||
			!diag::is_serious_exception( info->ExceptionRecord->ExceptionCode ) )
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if ( diag::probe_active( ) )
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		// The host or Steam may replace the single process-wide last-chance
		// filter after injection. Re-arm it at first chance and preserve the
		// displaced handler so the host still receives the crash after us.
		const auto displaced_filter =
			SetUnhandledExceptionFilter( diag_unhandled_exception_filter );
		if ( displaced_filter != diag_unhandled_exception_filter )
		{
			g_previous_exception_filter.store(
				displaced_filter,
				std::memory_order_release );
		}

		// A guarded boundary owns this fault: diag::guard_filter logs it with its label and the phase, and
		// the __except below us swallows it. Recording a crash here would burn the one-shot slot reserved
		// for the fault that actually kills the process on something we are about to survive. A stack
		// overflow is the exception -- the guard refuses to swallow it, so it still deserves the record.
		if ( diag::guard_active( ) &&
			info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW )
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if ( diag::is_module_address( info->ExceptionRecord->ExceptionAddress ) )
		{
			diag::record_crash(
				info,
				diag::g_exception_scope_depth
					? diag::g_exception_phase
					: "first-chance fault in velocity DLL" );
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if ( diag::g_exception_scope_depth == 0 )
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const auto code = info->ExceptionRecord->ExceptionCode;
		const auto instruction =
			reinterpret_cast<std::uintptr_t>( info->ExceptionRecord->ExceptionAddress );
		const auto accessed =
			info->ExceptionRecord->NumberParameters > 1
				? info->ExceptionRecord->ExceptionInformation[ 1 ]
				: 0;

		HMODULE fault_module{};
		char module_path[ MAX_PATH ]{ "unknown" };
		std::uintptr_t module_base{};
		if ( instruction &&
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>( instruction ), &fault_module ) )
		{
			module_base = reinterpret_cast<std::uintptr_t>( fault_module );
			GetModuleFileNameA( fault_module, module_path, MAX_PATH );
		}

		const auto* module_name = strrchr( module_path, '\\' );
		module_name = module_name ? module_name + 1 : module_path;

		char buf[ 384 ]{};
		_snprintf_s(
			buf, sizeof( buf ), _TRUNCATE,
			"FEATURE EXCEPTION [%s] 0x%08lX at %s+0x%llX (0x%p), accessed 0x%p",
			diag::g_exception_phase,
			code,
			module_name,
			module_base
				? static_cast<unsigned long long>( instruction - module_base )
				: 0ull,
			info->ExceptionRecord->ExceptionAddress,
			reinterpret_cast<void*>( accessed ) );

		// error, not debug: the faulting instruction is in the game but the pointer that took it there
		// came from us, and this is the only record of it. error flushes the log immediately, so the line
		// survives even when the process goes down a few instructions later. It deliberately does not go
		// through record_crash -- that claims a one-shot slot meant for the fault that actually kills us.
		diag::write( diag::level::error, buf );

		return EXCEPTION_CONTINUE_SEARCH;
	}

	LONG WINAPI diag_unhandled_exception_filter( EXCEPTION_POINTERS* info )
	{
		if ( !info || !info->ExceptionRecord )
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const auto instruction =
			reinterpret_cast<std::uintptr_t>( info->ExceptionRecord->ExceptionAddress );
		const auto accessed =
			info->ExceptionRecord->NumberParameters > 1
				? info->ExceptionRecord->ExceptionInformation[ 1 ]
				: 0;

		HMODULE fault_module{};
		char module_path[ MAX_PATH ]{ "unknown" };
		std::uintptr_t module_base{};
		if ( instruction &&
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>( instruction ), &fault_module ) )
		{
			module_base = reinterpret_cast<std::uintptr_t>( fault_module );
			GetModuleFileNameA( fault_module, module_path, MAX_PATH );
		}

		const auto* module_name = strrchr( module_path, '\\' );
		module_name = module_name ? module_name + 1 : module_path;

		char buf[ 384 ]{};
		_snprintf_s(
			buf, sizeof( buf ), _TRUNCATE,
			"UNHANDLED EXCEPTION 0x%08lX at %s+0x%llX (0x%p), accessed 0x%p",
			info->ExceptionRecord->ExceptionCode,
			module_name,
			module_base
				? static_cast<unsigned long long>( instruction - module_base )
				: 0ull,
			info->ExceptionRecord->ExceptionAddress,
			reinterpret_cast<void*>( accessed ) );
		diag::write( diag::level::fatal, buf );
		diag::record_crash( info, "unhandled exception" );

		const auto previous_filter =
			g_previous_exception_filter.load( std::memory_order_acquire );
		if ( previous_filter &&
			previous_filter != diag_unhandled_exception_filter )
		{
			return previous_filter( info );
		}

		return EXCEPTION_CONTINUE_SEARCH;
	}

#if defined( DEV )
	#define INIT_FAIL( msg ) \
		do { \
			diag::write( diag::level::error, msg ); \
			return 0; \
		} while ( 0 )

	#define INIT_WARN( msg ) diag::write( diag::level::warning, msg )
#else
	#define INIT_FAIL( msg ) \
		do { \
			diag::write( diag::level::error, msg ); \
			MessageBoxA( nullptr, xs( msg ), xs( "..." ), MB_ICONERROR ); \
			return 0; \
		} while ( 0 )

	#define INIT_WARN( msg ) INIT_FAIL( msg )
#endif

	DWORD WINAPI init_thread_impl( LPVOID param )
	{
		const auto module_handle = static_cast<HMODULE>( param );

		diag::step( "stage: thread start" );
		diag::initialize_crash_dumps( );

		g_previous_exception_filter.store(
			SetUnhandledExceptionFilter( diag_unhandled_exception_filter ),
			std::memory_order_release );
		g_vectored_exception_handler =
			AddVectoredExceptionHandler( 1, diag_vectored_exception_filter );
		if ( !g_vectored_exception_handler )
		{
			diag::writef(
				diag::level::error,
				"failed to install vectored exception handler; win32_error=%lu",
				GetLastError( ) );
		}
		else
		{
			diag::write( diag::level::info, "crash handlers installed" );
		}

		diag::step( "stage: coinit" );
		const auto coinit_result =
			CoInitializeEx( nullptr, COINIT_MULTITHREADED );
		if ( FAILED( coinit_result ) )
		{
			diag::writef(
				diag::level::warning,
				"CoInitializeEx failed; hresult=0x%08lX",
				coinit_result );
		}

		diag::step( "stage: config" );
		config::initialize( );
		settings::finalize_binds( );

		diag::step( "stage: regions" );
		security::regions::add_module( module_handle );

		diag::step( "stage: logging" );
		{
			if ( !logging::console::initialize( ) )
			{
#if defined( DEV )
				INIT_WARN( "failed to initialize console logging." );
#else
				INIT_FAIL( "failed to initialize console logging." );
#endif
			}

			if ( !logging::popup::initialize( ) )
			{
#if defined( DEV )
				INIT_WARN( "failed to initialize popup logging." );
#else
				INIT_FAIL( "failed to initialize popup logging." );
#endif
			}
		}

		diag::step( "stage: integrity" );
		{
			if ( !security::integrity::initialize( ) )
			{
				INIT_FAIL( "failed to initialize integrity checks." );
			}

#if defined( DEV )
			install_game_crash_capture( );
#endif
			install_termination_capture( );

			if ( !threadpool::initialize( ) )
			{
				INIT_FAIL( "failed to initialize thread pool." );
			}

			if ( !steam::http::initialize( ) )
			{
				INIT_FAIL( "failed to initialize steam http." );
			}

			if ( !steam::friends::initialize( ) )
			{
				INIT_FAIL( "failed to initialize steam friends." );
			}

			if ( !steam::user::initialize( ) )
			{
				INIT_FAIL( "failed to initialize steam user." );
			}

			if ( !steam::utils::initialize( ) )
			{
				INIT_FAIL( "failed to initialize steam utils." );
			}
		}

		diag::step( "stage: addresses" );
		{
			if ( !addresses::modules::initialize( ) )
			{
				INIT_FAIL( "failed to initialize module addresses." );
			}

			if ( !addresses::globals::initialize( ) )
			{
				INIT_FAIL( "failed to initialize global addresses." );
			}

			if ( !addresses::functions::initialize( ) )
			{
				INIT_FAIL( "failed to initialize function addresses." );
			}
		}

		diag::step( "stage: systems" );
		{
			if ( !systems::materials::initialize( ) )
			{
				INIT_FAIL( "failed to initialize materials system." );
			}

			if ( !systems::events::initialize( ) )
			{
				INIT_FAIL( "failed to initialize event system." );
			}

			if ( !systems::g_icons.initialize( ) )
			{
				INIT_FAIL( "failed to initialize vpk parse system." );
			}

			if ( !systems::g_model_preview.initialize( ) )
			{
				INIT_FAIL( "failed to initialize model preview system." );
			}
		}

		diag::step( "stage: econ" );
		{
			if ( !features::changer::g_econ_item_system.initialize( ) )
			{
				INIT_FAIL( "failed to initialize econ item system." );
			}
		}

		diag::step( "stage: hooks" );
		{
			if ( !hooks::utility::initialize( ) )
			{
				INIT_FAIL( "failed to initialize utility hooks." );
			}

			if ( !hooks::cheat::initialize( ) )
			{
				INIT_FAIL( "failed to initialize cheat hooks." );
			}
		}

		diag::step( "stage: cvars" );
		{
			if ( !addresses::globals::cvar->unlock_all( ) )
			{
				INIT_FAIL( "failed to unlock hidden cvars." );
			}
		}

		diag::step( "stage: skyboxes" );
		features::world::g_scene.discover_skyboxes( );

		diag::step( "stage: done" );
		return 1;
	}

	DWORD diag_exception_filter( EXCEPTION_POINTERS* info )
	{
		char buf[ 128 ]{};
		_snprintf_s( buf, sizeof( buf ), _TRUNCATE, "EXCEPTION 0x%08lX at 0x%p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress );
		diag::write( diag::level::fatal, buf );
		diag::record_crash( info, "initialization thread" );
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// The crash is contained so the game keeps running, but on a ship build that used to mean no menu and
	// no message at all. Separate from init_thread because xs() builds an object, which __try forbids.
	void report_init_crash( )
	{
#if !defined( DEV )
		MessageBoxA(
			nullptr,
			xs( "initialization crashed. details are in velocity_init.log next to the dll." ),
			xs( "..." ),
			MB_ICONERROR );
#endif
	}

	DWORD WINAPI init_thread( LPVOID param )
	{
		__try
		{
			return init_thread_impl( param );
		}
		__except ( diag_exception_filter( GetExceptionInformation( ) ) )
		{
			report_init_crash( );
			return 0;
		}
	}

	HMODULE g_module{};
	std::atomic<bool> g_unload_requested{};

	// Set once the unload thread has torn everything down and is about to free the module, so
	// DLL_PROCESS_DETACH does not run the teardown a second time -- it calls into the game, and detach runs
	// under the loader lock.
	std::atomic<bool> g_torn_down{};

	// The exception handlers are the one piece of teardown that is pure Win32, so both the unload thread and
	// detach run it. Idempotent.
	void remove_exception_handlers( )
	{
		if ( g_vectored_exception_handler )
		{
			RemoveVectoredExceptionHandler( g_vectored_exception_handler );
			g_vectored_exception_handler = nullptr;
		}

		const auto previous_filter =
			g_previous_exception_filter.exchange(
				nullptr,
				std::memory_order_acq_rel );
		const auto current_filter =
			SetUnhandledExceptionFilter( previous_filter );
		if ( current_filter != diag_unhandled_exception_filter )
		{
			SetUnhandledExceptionFilter( current_filter );
		}
	}

	struct thread_basic_information
	{
		LONG exit_status;
		void* teb_base;
		void* process_id;
		void* thread_id;
		ULONG_PTR affinity_mask;
		LONG priority;
		LONG base_priority;
	};

	// Filled while a thread is suspended, so nothing that touches the heap or a lock may run in between.
	// Static for exactly that reason -- a suspended thread can be holding the allocator's lock.
	std::uintptr_t g_stack_words[ 0x2000 ]{};

	[[nodiscard]] bool looks_like_return_address( std::uintptr_t value, std::uintptr_t base, std::size_t size )
	{
		if ( value < base + 8 || value >= base + size )
		{
			return false;
		}

		const auto* at = reinterpret_cast<const std::uint8_t*>( value );
		return at[ -5 ] == 0xE8 || at[ -6 ] == 0xFF || at[ -2 ] == 0xFF || at[ -3 ] == 0xFF;
	}

	/// True while any other thread is executing inside the module or has a return address into it on its
	/// stack. That is the condition FreeLibrary must not be called under: the thread would come back to
	/// unmapped code. Also true when the check itself cannot be completed, because the safe answer to "I
	/// do not know" is to leave the module mapped.
	[[nodiscard]] bool module_in_use( std::uintptr_t base, std::size_t size )
	{
		using nt_query_information_thread_fn = LONG( NTAPI* )( HANDLE, int, void*, ULONG, ULONG* );

		const auto ntdll = GetModuleHandleW( L"ntdll.dll" );
		const auto query = ntdll
			? reinterpret_cast<nt_query_information_thread_fn>( GetProcAddress( ntdll, "NtQueryInformationThread" ) )
			: nullptr;
		if ( !query )
		{
			return true;
		}

		const auto snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
		if ( snapshot == INVALID_HANDLE_VALUE )
		{
			return true;
		}

		const auto process_id = GetCurrentProcessId( );
		const auto self = GetCurrentThreadId( );
		auto in_use = false;

		THREADENTRY32 entry{ sizeof( THREADENTRY32 ) };
		for ( auto ok = Thread32First( snapshot, &entry ); ok && !in_use; ok = Thread32Next( snapshot, &entry ) )
		{
			if ( entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self )
			{
				continue;
			}

			const auto thread = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID );
			if ( !thread )
			{
				continue;
			}

			thread_basic_information info{};
			ULONG returned{};
			std::uintptr_t stack_base{};
			if ( query( thread, 0, &info, sizeof( info ), &returned ) >= 0 && info.teb_base )
			{
				stack_base = reinterpret_cast<std::uintptr_t>( reinterpret_cast<NT_TIB*>( info.teb_base )->StackBase );
			}

			CONTEXT context{};
			context.ContextFlags = CONTEXT_CONTROL;
			std::size_t words{};
			auto got_context = false;

			if ( SuspendThread( thread ) != static_cast<DWORD>( -1 ) )
			{
				got_context = GetThreadContext( thread, &context ) != FALSE;

				const auto sp = static_cast<std::uintptr_t>( context.Rsp );
				if ( got_context && stack_base > sp )
				{
					const auto bytes = std::min<std::size_t>( stack_base - sp, sizeof( g_stack_words ) );
					std::memcpy( g_stack_words, reinterpret_cast<const void*>( sp ), bytes );
					words = bytes / sizeof( std::uintptr_t );
				}

				ResumeThread( thread );
			}

			CloseHandle( thread );

			if ( got_context && context.Rip >= base && context.Rip < base + size )
			{
				in_use = true;
				break;
			}

			for ( std::size_t i = 0; i < words; ++i )
			{
				if ( looks_like_return_address( g_stack_words[ i ], base, size ) )
				{
					in_use = true;
					break;
				}
			}
		}

		CloseHandle( snapshot );
		return in_use;
	}

	// Terminates the module's static objects -- the same call DLL_PROCESS_DETACH makes on a development
	// build. On its own function so a destructor that faults costs the cleanup, not the game.
	void run_crt_terminators( HMODULE module_handle )
	{
		__try
		{
			_CRT_INIT( module_handle, DLL_PROCESS_DETACH, nullptr );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
		}
	}

	// How long the game gets to leave our code after the hooks come off before unload gives up and leaves the
	// module mapped and inert. Nothing about unload is time critical, and a stuck thread is the one case
	// where freeing the module would take the game down with it.
	constexpr DWORD k_unload_drain_timeout_ms{ 10000 };
	constexpr DWORD k_unload_drain_poll_ms{ 200 };

	[[nodiscard]] bool teardown_and_drain( )
	{
		diag::exception_scope unload_scope{ "unload" };

		// Hands the mouse back to the game if the menu had grabbed it, which it has: the page that starts
		// unload is only reachable with the menu open.
		diag::guard( "unload: menu", [ ] { rendering::g_menu.shutdown( ); } );
		diag::guard( "unload: events", [ ] { systems::events::shutdown( ); } );

		// A worker thread of ours: it has to be gone before the drain below, or the module never reads as idle.
		diag::guard( "unload: discord", [ ] { features::misc::g_discord_rpc.shutdown( ); } );

		// Hooks first, resources second: anything the game calls into us for has to stop before what it uses
		// is released. Every hook is reset through the registry, so none can be missed. Trampolines are kept
		// alive across the reset; a thread that was already inside a detour still returns through one.
		hooking::manager::retain_trampolines( true );
		hooking::manager::reset_all( );
		diag::step( "unload: hooks removed" );

		const auto module_base = reinterpret_cast<std::uintptr_t>( g_module );
		const auto module_size = static_cast<std::size_t>(
			reinterpret_cast<const IMAGE_NT_HEADERS*>(
				module_base + reinterpret_cast<const IMAGE_DOS_HEADER*>( module_base )->e_lfanew )->OptionalHeader.SizeOfImage );

		auto waited{ 0ul };
		while ( module_in_use( module_base, module_size ) )
		{
			if ( waited >= k_unload_drain_timeout_ms )
			{
				diag::write( diag::level::warning, "unload: a thread is still inside the module; leaving it mapped" );
				return false;
			}

			Sleep( k_unload_drain_poll_ms );
			waited += k_unload_drain_poll_ms;
		}

		diag::writef( diag::level::info, "unload: module idle after %lu ms", waited );

		diag::guard( "unload: chams", [ ]
		{
			features::esp::player::g_chams.bt( ).shutdown( );
			features::esp::player::g_chams.os( ).shutdown( );
		} );
		diag::guard( "unload: aimwhere users", [ ] { features::misc::g_aimwhere_users.shutdown( ); } );
		diag::guard( "unload: weather", [ ] { features::world::g_weather.release( ); } );
		diag::guard( "unload: dlight", [ ] { features::misc::g_dlight.on_level_shutdown( ); } );
		diag::guard( "unload: material clones", [ ] { systems::materials::clear_clones( ); } );
		diag::guard( "unload: renderer", [ ] { rendering::g_context.shutdown( ); } );

		hooking::manager::release_retained( );
		return true;
	}

	/// How many LoadLibrary references the loader holds on this module, or 0 when it cannot be read.
	///
	/// Injecting the same path again does not re-run startup, it only adds a reference, so a session that
	/// was injected three times needs three frees before the image leaves the address space; a single
	/// FreeLibraryAndExitThread left it mapped and inert. The count lives in the loader's dependency-graph
	/// node (LDR_DATA_TABLE_ENTRY +0x98, LDR_DDAG_NODE +0x18 on x64 Windows 10/11). Those are undocumented
	/// offsets, so the result is sanity-checked and anything implausible is reported as unknown, which
	/// falls back to the single free.
	[[nodiscard]] ULONG loader_reference_count( HMODULE module_handle )
	{
		ULONG count{};

		__try
		{
			const auto* peb = reinterpret_cast<const std::uint8_t*>( __readgsqword( 0x60 ) );
			const auto* ldr = *reinterpret_cast<const std::uint8_t* const*>( peb + 0x18 );
			const auto* head = reinterpret_cast<const LIST_ENTRY*>( ldr + 0x10 );

			auto guard_steps{ 0 };
			for ( auto* link = head->Flink; link != head && guard_steps < 4096; link = link->Flink, ++guard_steps )
			{
				const auto* entry = reinterpret_cast<const std::uint8_t*>( link );
				if ( *reinterpret_cast<void* const*>( entry + 0x30 ) != module_handle )
				{
					continue;
				}

				const auto* node = *reinterpret_cast<const std::uint8_t* const*>( entry + 0x98 );
				if ( node )
				{
					count = *reinterpret_cast<const ULONG*>( node + 0x18 );
				}

				break;
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			count = 0;
		}

		return ( count >= 1 && count <= 64 ) ? count : 0;
	}

	DWORD WINAPI unload_thread_impl( LPVOID param )
	{
		const auto module_handle = static_cast<HMODULE>( param );

		diag::step( "stage: unload begin" );

		// A function of its own so the scope guard inside it is gone before FreeLibraryAndExitThread, which
		// never returns and so would never run a destructor.
		if ( !teardown_and_drain( ) )
		{
			g_unload_requested.store( false, std::memory_order_release );
			return 0;
		}

		diag::step( "stage: unload done" );

		const auto references = loader_reference_count( module_handle );
		diag::writef( diag::level::info, "unload: loader holds %lu reference(s) on the module", references );

		remove_exception_handlers( );
		diag::shutdown( );
		g_torn_down.store( true, std::memory_order_release );
		run_crt_terminators( module_handle );

		// Every reference but one is dropped here, where this thread's own code is still safely mapped; the
		// last one goes with FreeLibraryAndExitThread, and detach runs then. Under a manual map the module
		// is not in the loader's list, so the frees are no-ops and the (now inert) image stays resident.
		for ( ULONG released{ 1 }; released < references; ++released )
		{
			FreeLibrary( module_handle );
		}

		FreeLibraryAndExitThread( module_handle, 0 );
	}

	DWORD WINAPI unload_thread( LPVOID param )
	{
		__try
		{
			return unload_thread_impl( param );
		}
		__except ( diag_exception_filter( GetExceptionInformation( ) ) )
		{
			return 0;
		}
	}
} // namespace

namespace unload {

	void request( )
	{
		if ( !g_module || g_unload_requested.exchange( true, std::memory_order_acq_rel ) )
		{
			return;
		}

		const auto thread = CreateThread( nullptr, 0, unload_thread, g_module, 0, nullptr );
		if ( !thread )
		{
			g_unload_requested.store( false, std::memory_order_release );
			diag::writef( diag::level::error, "failed to create unload thread; win32_error=%lu", GetLastError( ) );
			return;
		}

		CloseHandle( thread );
	}

	bool requested( )
	{
		return g_unload_requested.load( std::memory_order_acquire );
	}

} // namespace unload

extern "C" int __stdcall entry( HMODULE module_handle, DWORD reason, LPVOID reserved )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		_CRT_INIT( module_handle, reason, reserved );
		DisableThreadLibraryCalls( module_handle );

		g_module = module_handle;
		diag::set_module( module_handle );
		diag::step( "stage: dll attach" );
#if defined( DEV )
		diag::step( "build: development diagnostics" );
#else
		diag::step( "build: ship" );
#endif

		diag::step( "stage: crt done, spawning thread" );

		const auto thread = CreateThread( nullptr, 0, init_thread, module_handle, 0, nullptr );
		if ( !thread )
		{
			diag::writef(
				diag::level::error,
				"failed to create initialization thread; win32_error=%lu",
				GetLastError( ) );
			return 0;
		}

		CloseHandle( thread );
		return 1;
	}
	else if ( reason == DLL_PROCESS_DETACH )
	{
		// Unhook our own exception handlers on every build, dev or not. They are the only teardown that
		// is unconditionally safe here: pure Win32, no call back into the game. Leaving them installed
		// while the module's pages go away means any later fault -- and ExitProcess produces them -- lands
		// in freed code with our handler at the front of the chain.
		remove_exception_handlers( );


		// Everything below calls back into the game. Nothing unloads this module while the process runs,
		// so detach only ever fires from ExitProcess -- where the loader has already suspended every other
		// thread, possibly mid-way through the scene lock we would need. Feature state is released from
		// level_shutdown instead, which runs on the game thread with the world still intact.
#if defined( DEV )
		if ( !g_torn_down.load( std::memory_order_acquire ) )
		{
			g_terminate_process_hook.reset( );
			g_minidump_hook.reset( );

			features::esp::player::g_chams.bt( ).shutdown( );
			features::esp::player::g_chams.os( ).shutdown( );

			features::world::g_weather.release( );
			rendering::g_menu.shutdown( );

			systems::events::shutdown( );
			hooks::utility::shutdown( );
			hooks::cheat::shutdown( );
			CoUninitialize( );
		}
#endif

		// The unload thread already ran both of these, in that order, before it freed the module.
		if ( !g_torn_down.load( std::memory_order_acquire ) )
		{
			diag::shutdown( );

#if defined( DEV )
			_CRT_INIT( module_handle, reason, reserved );
#endif
		}
	}

	return 1;
}
