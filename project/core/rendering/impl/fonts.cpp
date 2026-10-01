#include <pch/pch.hpp>
#include <core/resources/fonts/inter.hpp>
#include <core/resources/fonts/pixel7.hpp>
#include "../rendering.hpp"

namespace rendering {

	void fonts::initialize( )
	{
		this->load_family( this->inter_medium, std::as_bytes( std::span{ resources::fonts::inter::regular } ), { 12.0f, 15.0f, 18.0f } );
		this->load_family( this->inter_bold, std::as_bytes( std::span{ resources::fonts::inter::bold } ), { 12.0f, 15.0f, 18.0f } );
		this->load_family( this->smallest_pixel7, std::as_bytes( std::span{ resources::fonts::pixel7::smallest } ), { 9.0f, 10.5f, 14.0f } );

		// Read from the Windows fonts folder rather than embedded: every Windows install has them, and the
		// bytes stay alive for the life of the process because the rasteriser reads glyphs from them lazily.
		static std::vector<std::byte> legacy_font_data{};
		wchar_t windows_dir[ MAX_PATH ]{};
		if ( GetWindowsDirectoryW( windows_dir, MAX_PATH ) )
		{
			for ( const auto* name : { L"\\Fonts\\verdana.ttf", L"\\Fonts\\tahoma.ttf" } )
			{
				std::ifstream file( std::wstring( windows_dir ) + name, std::ios::binary );
				if ( !file )
				{
					continue;
				}

				const std::vector<char> raw( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>( ) );
				if ( raw.empty( ) )
				{
					continue;
				}

				legacy_font_data.resize( raw.size( ) );
				std::memcpy( legacy_font_data.data( ), raw.data( ), raw.size( ) );
				this->legacy_menu = xdraw::load_font( legacy_font_data, 12.0f );
				if ( this->legacy_menu )
				{
					break;
				}
			}
		}
	}

	void fonts::load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes )
	{
		for ( auto i = 0ull; i < sizes.size( ); ++i )
		{
			family.sizes[ i ] = xdraw::load_font( data, sizes[ i ] );
		}
	}

} // namespace rendering
