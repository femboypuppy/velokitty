#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/diag.hpp>

#include "../systems.hpp"

namespace systems {

	std::uint32_t schemas::lookup( const char* class_name, std::uint32_t field_hash, const char* field_source )
	{
		// A miss is a zero, and a zero is a valid offset -- the caller then reads the object's first eight bytes
		// as if they were the field. It used to be silent, which is how a field renamed by a game update turned
		// into a feature that quietly did nothing. Each call site caches its result, so this logs once per site.
		//
		// The field is named from the call site's source text: after the October 9 update the log said only
		// "hash=0xE9EFCFCF", and turning that back into m_angShootAngleHistory took a script.
		const auto miss = [ & ]( const char* reason )
		{
			diag::writef( diag::level::warning, "schema miss: %s::%s hash=0x%08X (%s)",
				class_name, field_source ? field_source : "?", field_hash, reason );
			return std::uint32_t{ 0 };
		};

		const auto type_scope = memory::call_vfunc<std::uintptr_t>( addresses::globals::schema_system, 13, xs( "client.dll" ), nullptr );
		if ( !type_scope )
		{
			return miss( "no type scope" );
		}

		auto class_info{ 0ull };
		memory::call_vfunc<void>( type_scope, 2, &class_info, class_name );

		if ( !class_info )
		{
			return miss( "class not found" );
		}

		const auto fields_ptr = memory::read<std::uintptr_t>( class_info + 0x30 );
		const auto field_count = memory::read<std::uint16_t>( class_info + 0x24 );

		if ( !fields_ptr || !field_count )
		{
			return miss( "no fields" );
		}

		for ( std::uint16_t i = 0; i < field_count; ++i )
		{
			const auto field_addr = fields_ptr + ( static_cast< std::size_t >( i ) * 0x20 );
			const auto name_ptr = memory::read<std::uintptr_t>( field_addr );

			if ( !name_ptr )
			{
				continue;
			}

			if ( fnv1a::runtime_hash( reinterpret_cast< const char* >( name_ptr ) ) == field_hash )
			{
				return memory::read<std::uint32_t>( field_addr + 0x10 );
			}
		}

		return miss( "field not found" );
	}
} // namespace systems