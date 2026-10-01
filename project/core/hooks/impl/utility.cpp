#include <pch/pch.hpp>
#include <filesystem>
#include <fstream>

#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/hooking/hooking.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/diag.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <core/resources/particles/effects.hpp>
#include <core/resources/particles/weather.hpp>
#include <protection/game_addresses.hpp>
#include "../hooks.hpp"

namespace {

	std::span<const unsigned char> find_embedded_particle( const std::string& filename )
	{
		if ( filename.find( xs( "snow" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::snow };
		}
		if ( filename.find( xs( "rain" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::rain };
		}
		if ( filename.find( xs( "kill" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::killstars };
		}
		if ( filename.find( xs( "stars" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::stars };
		}
		if ( filename.find( xs( "tracer" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::tracer };
		}
		if ( filename.find( xs( "sparks" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::sparks };
		}
		if ( filename.find( xs( "fade" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::fade };
		}
		if ( filename.find( xs( "halo" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::halo };
		}

		return {};
	}

	/// Largest file the model VFS will hand the engine. A player model plus its meshes and textures is
	/// a few megabytes; anything past this is either not a model or is a mistake, and either way it is
	/// not worth asking the engine's allocator for.
	constexpr std::uintmax_t k_max_custom_file{ 192ull * 1024ull * 1024ull };

	/// Serve one read straight off disk into a buffer the engine allocated for it.
	///
	/// Runs on the engine's filesystem thread, so it has to be exception-free and must not touch
	/// anything the main thread is writing -- `resolve` already took the only lock involved and the
	/// absolute path it returned is a copy.
	bool serve_custom_file( std::uintptr_t a1, const std::wstring& absolute, const std::string& filename )
	{
		const std::filesystem::path path{ absolute };

		std::error_code ec{};
		const auto size = std::filesystem::file_size( path, ec );
		if ( ec || size == 0 || size > k_max_custom_file )
		{
			return false;
		}

		const auto async_filesystem = memory::read<std::uintptr_t>( a1 + 24 );
		if ( !async_filesystem )
		{
			return false;
		}

		std::ifstream stream{ path, std::ios::binary };
		if ( !stream.is_open( ) )
		{
			return false;
		}

		const auto buffer = memory::call_vfunc<std::uintptr_t>( async_filesystem, 22, static_cast< std::size_t >( size ), filename.c_str( ) );
		if ( !buffer )
		{
			return false;
		}

		// Straight into the engine's allocation -- one copy, no staging vector, because a 100 MB model
		// read on the filesystem thread is not the place to double the peak.
		stream.read( reinterpret_cast< char* >( buffer ), static_cast< std::streamsize >( size ) );

		const auto read = static_cast< std::uintmax_t >( stream.gcount( ) );
		if ( read != size )
		{
			// The engine owns the allocation either way; it frees it when the request closes. Falling
			// through to the original read is the honest answer to a short read.
			return false;
		}

		memory::write<std::uintptr_t>( a1 + 56, buffer );
		memory::write<std::uintptr_t>( a1 + 64, static_cast< std::uintptr_t >( size ) );
		memory::write<std::uintptr_t>( a1 + 72, static_cast< std::uintptr_t >( size ) );
		memory::call<void>( PATTERN (patterns::filesystem_close), a1 - 224, 0 );

		return true;
	}

	/// Same job for a file that came out of a mounted .vpk. The bytes are already in a vector by the time
	/// we get here -- `read_packed` did the seek and the sized read -- so this is only the handover.
	bool serve_custom_bytes( std::uintptr_t a1, const std::vector<std::uint8_t>& bytes, const std::string& filename )
	{
		if ( bytes.empty( ) || bytes.size( ) > k_max_custom_file )
		{
			return false;
		}

		const auto async_filesystem = memory::read<std::uintptr_t>( a1 + 24 );
		if ( !async_filesystem )
		{
			return false;
		}

		const auto buffer = memory::call_vfunc<std::uintptr_t>( async_filesystem, 22, bytes.size( ), filename.c_str( ) );
		if ( !buffer )
		{
			return false;
		}

		std::memcpy( reinterpret_cast< void* >( buffer ), bytes.data( ), bytes.size( ) );

		memory::write<std::uintptr_t>( a1 + 56, buffer );
		memory::write<std::uintptr_t>( a1 + 64, static_cast< std::uintptr_t >( bytes.size( ) ) );
		memory::write<std::uintptr_t>( a1 + 72, static_cast< std::uintptr_t >( bytes.size( ) ) );
		memory::call<void>( PATTERN (patterns::filesystem_close), a1 - 224, 0 );

		return true;
	}

	/// The whole of our side of a filesystem read, lifted out of the hook so the hook itself is nothing but
	/// a guarded call plus the fallback to the engine. True means the request is closed and answered.
	bool serve_read( std::uintptr_t a1 )
	{
		// Without the close call the engine's request never completes, so let the original read run instead.
		if ( !PATTERN (patterns::filesystem_close) )
		{
			return false;
		}

		const auto flags_len = memory::read<std::uint32_t>( a1 - 212 );
		const auto len = flags_len & 0x3fffffff;

		std::string filename;
		if ( len > 0 && len < 512 )
		{
			char buffer[ 512 ]{};

			if ( flags_len & 0x40000000 )
			{
				std::memcpy( buffer, reinterpret_cast< void* >( a1 - 208 ), std::min( len, 511u ) );
			}
			else
			{
				const auto string = memory::read<std::uintptr_t>( a1 - 208 );
				if ( string )
				{
					std::memcpy( buffer, reinterpret_cast< void* >( string ), std::min( len, 511u ) );
				}
			}

			filename = buffer;
		}

		if ( filename.find( xs( "particles/embedded/" ) ) != std::string::npos )
		{
			const auto particle = find_embedded_particle( filename );
			const auto async_filesystem = memory::read<std::uintptr_t>( a1 + 24 );

			if ( !particle.empty( ) && async_filesystem )
			{
				const auto buffer = memory::call_vfunc<std::uintptr_t>( async_filesystem, 22, particle.size( ), filename.c_str( ) );
				if ( buffer )
				{
					std::memcpy( reinterpret_cast< void* >( buffer ), particle.data( ), particle.size( ) );

					memory::write<std::uintptr_t>( a1 + 56, buffer );
					memory::write<std::uintptr_t>( a1 + 64, particle.size( ) );
					memory::write<std::uintptr_t>( a1 + 72, particle.size( ) );
					memory::call<void>( PATTERN (patterns::filesystem_close), a1 - 224, 0 );

					return true;
				}
			}
		}

		// Custom models. `has_files` is a relaxed atomic load, so a user with an empty models folder --
		// the common case -- pays that and nothing else per file the game opens.
		if ( !filename.empty( ) && features::changer::g_custom_models.has_files( ) )
		{
			const auto absolute = features::changer::g_custom_models.resolve( filename );
			if ( !absolute.empty( ) && serve_custom_file( a1, absolute, filename ) )
			{
				return true;
			}

			// Loose files first, archives second -- a file the user extracted himself is the deliberate
			// one, and this way it shadows the copy still sitting inside the .vpk next to it.
			std::vector<std::uint8_t> bytes{};
			if ( features::changer::g_custom_models.read_packed( filename, bytes ) && serve_custom_bytes( a1, bytes, filename ) )
			{
				return true;
			}
		}

		return false;
	}

} // namespace

namespace hooks {

	bool utility::initialize( )
	{
		if ( !hooking::manager::create( {
			{ &m_service_read, &service_read, xs( "service_read" ), PATTERN (patterns::service_read) },
			{ &m_log_internal, &log_internal, xs( "log_internal" ), PATTERN (patterns::log_internal) }
			} ) )
		{
			return false;
		}

		return true;
	}

	void utility::shutdown( )
	{
		m_service_read.reset( );
		m_log_internal.reset( );
	}

	std::uintptr_t __fastcall utility::service_read( std::uintptr_t a1 )
	{
		// This runs on the engine's filesystem threads, which is why a fault here was so hard to see: no
		// menu, no frame, nothing on screen, just the process going away. Everything it touches is either
		// an engine pointer read through a negative offset or a path the user chose, so it gets a boundary
		// and the engine's own read as the fallback.
		auto served = false;
		diag::exception_scope read_scope{ "filesystem: service read" };
		diag::guard( "service_read", [ & ] { served = serve_read( a1 ); } );

		if ( served )
		{
			return 0;
		}

		return m_service_read.call<std::uintptr_t>( a1 );
	}

	std::intptr_t __fastcall utility::log_internal( std::uintptr_t a1, std::uint32_t channel, std::int32_t severity, std::uintptr_t metadata, const char* message, std::intptr_t* args )
	{
		if ( settings::g_misc.disable_game_logs && !logging::console::emitting )
		{
			return 0;
		}

		return m_log_internal.call<std::intptr_t>( a1, channel, severity, metadata, message, args );
	}

} // namespace hooks
