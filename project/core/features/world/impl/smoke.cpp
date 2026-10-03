#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

#include "../world.hpp"

// was a deadend.

namespace features::world {

	void smoke::on_frame_stage_notify( )
	{
		const auto& scene = settings::g_world.m_scene;
		if ( !scene.smoke_color.value )
		{
			return;
		}

		const auto& c = scene.smoke_color_value.value;

		for ( const auto& e : systems::g_entities.get_by_type( systems::entities::type::projectile ) )
		{
			if ( !e.ptr || e.schema_hash != "C_SmokeGrenadeProjectile"_hash )
			{
				continue;
			}

			const auto field = e.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_vSmokeColor"_hash );

			// The game never says which scale this vector is in. The first smoke's own value decides it: anything
			// above 1.5 can only be 0-255, a value at or under 1 means 0-1, and all zeros keeps the 0-255 guess.
			// Logged once so the next log shows what the game actually sent.
			if ( !this->m_scale_known )
			{
				const auto original = memory::safe_read<math::vector3>( field );
				if ( !original.has_value( ) )
				{
					continue;
				}

				const auto peak = std::max( { original->x, original->y, original->z } );
				if ( peak > 0.0f )
				{
					this->m_scale = peak > 1.5f ? 255.0f : 1.0f;
				}

				this->m_scale_known = true;

				diag::writef( diag::level::info, "smoke color: game value (%.3f %.3f %.3f), writing on a 0-%.0f scale", original->x, original->y, original->z, this->m_scale );
			}

			const auto scale = this->m_scale / 255.0f;
			(void) memory::safe_write<math::vector3>( field, math::vector3{ c.r * scale, c.g * scale, c.b * scale } );
		}
	}

	void smoke::on_map( std::uintptr_t token, std::size_t size, std::uintptr_t buf_ptr )
	{
		// 2480 before the October 2026 update, 2496 after. The per-smoke arrays did not move; only the
		// trailer grew, and the cloud count always sits 16 bytes before the end.
		if ( ( size != 2480 && size != 2496 ) || !token || !buf_ptr )
		{
			return;
		}

		this->m_token = token;
		this->m_buf = buf_ptr;
		this->m_size = size;
	}

	void smoke::on_unmap( std::uintptr_t token )
	{
		const auto buf = this->m_buf;
		if ( !buf || token != this->m_token )
		{
			return;
		}

		const auto count = *reinterpret_cast< std::uint32_t* >( buf + this->m_size - 16 );

		// One dump of the first cloud's slot in every per-smoke array of this buffer, so the next log shows which
		// array the opacity and the colour really live in.
		if ( !this->m_dumped && count > 0 && count <= 16 )
		{
			this->m_dumped = true;

			std::string line{ "smoke buffer: count=" + std::to_string( count ) };
			for ( auto j = 0u; j < 9u; ++j )
			{
				const auto v = reinterpret_cast< const float* >( buf + 256u * j );
				char part[ 96 ]{};
				std::snprintf( part, sizeof( part ), " | [%u] %.3f %.3f %.3f %.3f", j * 256u, v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ] );
				line += part;
			}

			diag::writef( diag::level::info, "%s", line.c_str( ) );
		}

		const auto& scene = settings::g_world.m_scene;
		const auto opacity_scale = settings::g_misc.m_removals.smoke.value
			? 0.0f
			: std::clamp( scene.smoke_opacity.value / 100.0f, 0.0f, 1.0f );

		if ( count > 0 && count <= 16 )
		{
			for ( auto i = 0u; i < count; ++i )
			{
				const auto slot = 16 * static_cast< std::size_t >( i );

				if ( opacity_scale < 1.0f )
				{
					auto opacity = reinterpret_cast< float* >( buf + 1280 + slot );
					opacity[ 0 ] *= opacity_scale;
				}

				// The game fills this slot from the cloud's own colour every frame, so its current value tells
				// us the scale: anything above 1.5 can only be 0-255.
				if ( scene.smoke_color.value )
				{
					const auto& c = scene.smoke_color_value.value;
					auto color = reinterpret_cast< float* >( buf + 512 + slot );
					const auto scale = std::max( { color[ 0 ], color[ 1 ], color[ 2 ] } ) > 1.5f ? 1.0f : 1.0f / 255.0f;
					color[ 0 ] = c.r * scale;
					color[ 1 ] = c.g * scale;
					color[ 2 ] = c.b * scale;
				}
			}
		}

		this->m_buf = 0;
		this->m_token = 0;
	}

} // namespace features::world
