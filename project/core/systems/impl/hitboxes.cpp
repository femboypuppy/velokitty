#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>

#include "../systems.hpp"

namespace systems {

    namespace {

        /// Every pointer this walk follows is an engine heap object, so it sits in the low half of the
        /// address space and is 8-aligned. A model that carries no hitbox data at all -- a head, a hair
        /// module, a viewmodel's arms, anything that was never built as a player model -- leaves garbage
        /// in these slots instead of zero, and a bare non-null check follows it straight off a cliff.
        /// The value that crashed was 0xFFFFFFFF: non-null, in range, and unaligned.
        [[nodiscard]] inline bool plausible( std::uintptr_t ptr ) noexcept
        {
            return ptr >= 0x10000ull && ptr < 0x00007FFFFFFFFFFFull && ( ptr & 0x7ull ) == 0ull;
        }

        /// Guarded scalar read. The chain runs entirely on values taken out of the game's own
        /// structures, and one bad model makes any of them nonsense, so a fault has to come back as an
        /// empty hitbox set rather than as a dead client.
        template <typename T>
        [[nodiscard]] inline T guarded( std::uintptr_t address, T fallback = T{} )
        {
            if ( address < 0x10000ull || address >= 0x00007FFFFFFFFFFFull )
            {
                return fallback;
            }

            return memory::safe_read<T>( address ).value_or( fallback );
        }

        /// Same, for a slot holding a pointer: anything implausible reads back as zero so the caller's
        /// existing null checks reject it without a second branch at every hop.
        [[nodiscard]] inline std::uintptr_t guarded_ptr( std::uintptr_t address )
        {
            const auto value = guarded<std::uintptr_t>( address );
            return plausible( value ) ? value : 0ull;
        }

    } // namespace

    hitboxes::set hitboxes::query( std::uintptr_t game_scene_node )
    {
        set result{};

        if ( !plausible( game_scene_node ) )
        {
            return result;
        }

        // CSkeletonInstance moved its model handle in the current client.
        auto model_handle = guarded_ptr( game_scene_node + 0x1E0 );
        if ( !model_handle )
        {
            model_handle = guarded_ptr( game_scene_node + 0x210 );
        }

        if ( !model_handle )
        {
            return result;
        }

        const auto cmodel = guarded_ptr( model_handle );
        if ( !cmodel )
        {
            return result;
        }

        const auto render_mesh_array = guarded_ptr( cmodel + 0x78 );
        if ( !render_mesh_array )
        {
            return result;
        }

        const auto render_meshes = guarded_ptr( render_mesh_array );
        if ( !render_meshes )
        {
            return result;
        }

        std::uintptr_t hitbox_set{};

        // Hitbox sets now live in the render mesh's set container. Player
        // models use the first set; retain the old direct pointer as fallback.
        const auto set_count = guarded<int>( render_meshes + 0x174 );
        if ( set_count > 0 && set_count <= 32 )
        {
            const auto storage_flags = guarded<std::uint32_t>( render_meshes + 0x164 );
            hitbox_set = ( storage_flags & 0x7fffffff )
                ? guarded_ptr( render_meshes + 0x168 )
                : render_meshes + 0x168;
        }

        if ( !hitbox_set )
        {
            hitbox_set = guarded_ptr( render_meshes + 0x150 );
        }

        if ( !plausible( hitbox_set ) )
        {
            return result;
        }

        const auto count = guarded<int>( hitbox_set + 0x28 );
        if ( count <= 0 || count > 20 )
        {
            return result;
        }

        const auto array_ptr = guarded_ptr( hitbox_set + 0x30 );
        if ( !array_ptr )
        {
            return result;
        }

        const auto remap_count = guarded<int>( cmodel + 0x220 );
        const auto remap_table = guarded_ptr( cmodel + 0x228 );
        const auto mesh_a = guarded_ptr( cmodel + 0x240 );
        const auto mesh_b = guarded_ptr( cmodel + 0x2F0 );

        constexpr auto k_hitbox_stride{ 0x70 };

        for ( auto i = 0; i < count; ++i )
        {
            const auto base = array_ptr + static_cast< std::size_t >( i ) * k_hitbox_stride;

            auto bone = -1;

            if ( remap_table && mesh_a && mesh_b && remap_count > 0 && remap_count <= 4096 )
            {
                const auto hb_idx = guarded<std::uint16_t>( base + 0x48 );
                const auto ofs_a = guarded<std::uint16_t>( mesh_a );
                const auto ofs_b = guarded<std::uint16_t>( mesh_b );
                const auto slot = static_cast< std::size_t >( hb_idx + ofs_a + ofs_b );

                if ( slot < static_cast< std::size_t >( remap_count ) )
                {
                    bone = guarded<std::int16_t>( remap_table + 2 * slot, -1 );
                }
            }

            // Bounded here rather than at each consumer: the record bone cache is 128 entries and every
            // caller indexes it with this value. One model with a foreign skeleton used to hand them an
            // index in the thousands.
            if ( bone < 0 || bone >= 128 )
            {
                continue;
            }

            const auto radius = guarded<float>( base + 0x30, -1.0f );
            if ( !std::isfinite( radius ) || radius < 0.0f || radius > 100.0f )
            {
                continue;
            }

            const auto mins = guarded<math::vector3>( base + 0x18 );
            const auto maxs = guarded<math::vector3>( base + 0x24 );

            if ( !std::isfinite( mins.x ) || !std::isfinite( mins.y ) || !std::isfinite( mins.z ) ||
                !std::isfinite( maxs.x ) || !std::isfinite( maxs.y ) || !std::isfinite( maxs.z ) )
            {
                continue;
            }

            auto& hb = result.entries[ result.count++ ];
            hb.index = i;
            hb.bone = bone;
            hb.mins = mins;
            hb.maxs = maxs;
            hb.radius = radius;
            hb.shape_type = guarded<std::uint8_t>( base + 0x3C );
            hb.translation_only = guarded<std::uint8_t>( base + 0x3D ) != 0;
        }

        return result;
    }

	int hitboxes::hitgroup_from_hitbox( int hitbox )
	{
		switch ( hitbox )
		{
		case 0:  return 1;
		case 1:  return 8;
		case 2:  return 3;
		case 3:  return 3;
		case 4:  return 2;
		case 5:  return 2;
		case 6:  return 2;
		case 7:  return 7;
		case 8:  return 6;
		case 9:  return 7;
		case 10: return 6;
		case 11: return 7;
		case 12: return 6;
		case 13: return 5;
		case 14: return 4;
		case 15: return 5;
		case 16: return 5;
		case 17: return 4;
		case 18: return 4;
		default: return 0;
		}
	}

	const char* hitboxes::hitgroup_to_name( int hitgroup )
	{
		constexpr const char* k_names[ ]{ "body", "head", "chest", "stomach", "left arm", "right arm", "left leg", "right leg", "neck" };
		return hitgroup < 9 ? k_names[ hitgroup ] : "body";
	}

} // namespace systems
