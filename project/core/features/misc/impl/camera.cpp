#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

#include "../misc.hpp"
#include <protection/game_addresses.hpp>

namespace features::misc {
	namespace {
		constexpr std::ptrdiff_t k_fov_offset{ 0x498 };
		constexpr std::ptrdiff_t k_origin_offset{ 0x4a0 };
		constexpr std::ptrdiff_t k_angles_offset{ 0x4b8 };

		// Written at the very end of the game's OverrideView (client.dll 0xB9CD73 in the Oct 2 build):
		// origin, angles, origin again, copied from the two fields above.
		constexpr std::ptrdiff_t k_origin_copy_offset{ 0x554 };
		constexpr std::ptrdiff_t k_angles_copy_offset{ 0x560 };
		constexpr std::ptrdiff_t k_origin_copy_2_offset{ 0x56c };
		constexpr std::ptrdiff_t k_aspect_ratio_offset{ 0x4d4 };
		constexpr std::ptrdiff_t k_view_flags_offset{ 0x551 };
		constexpr std::uint8_t k_explicit_aspect_ratio_flag{ 1u << 1 };

		[[nodiscard]] float scale_horizontal_fov( float fov, float aspect_ratio )
		{
			constexpr auto degrees_to_half_radians{ std::numbers::pi_v<float> / 360.0f };
			constexpr auto half_radians_to_degrees{ 360.0f / std::numbers::pi_v<float> };
			constexpr auto four_by_three_inverse{ 0.75f };

			return std::atan( std::tan( fov * degrees_to_half_radians ) * aspect_ratio * four_by_three_inverse ) * half_radians_to_degrees;
		}
	}

	void camera::on_override_view( std::uintptr_t view_setup )
	{
		const auto local = systems::g_local.get( );
		if ( local.is_alive && !systems::g_local.is_in_cinematic( ) && local.team >= 2 && local.pawn )
		{
			this->do_thirdperson( view_setup, local.pawn );
			this->do_fov_change( view_setup, local.pawn );
		}

		// Aspect conversion must run after the base FOV has been selected.
		this->do_aspect_ratio_change( view_setup );
	}

	std::uintptr_t camera::resolve_spectate_pawn( ) const
	{
		if ( !settings::g_misc.m_camera.spectate.value || systems::g_local.is_in_cinematic( ) )
		{
			return 0;
		}

		const auto controller = this->m_spec_controller.load( );
		if ( !controller || !systems::g_entities.exists( controller ) )
		{
			return 0;
		}

		const auto local = systems::g_local.get( );
		if ( controller == local.controller )
		{
			return 0;
		}

		if ( !memory::safe_read<bool>( controller + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ).value_or( false ) )
		{
			return 0;
		}

		// Looked up through the controller every frame: the pawn is a new entity after each respawn.
		const auto pawn = systems::g_entities.lookup( memory::safe_read<std::uint32_t>( controller + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) ).value_or( 0 ) );
		if ( !pawn || pawn == local.pawn )
		{
			return 0;
		}

		if ( memory::safe_read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ).value_or( 0 ) <= 0 )
		{
			return 0;
		}

		return pawn;
	}

	void camera::on_override_view_spectate( std::uintptr_t view_setup )
	{
		const auto now = std::chrono::steady_clock::now( );
		const auto dt = std::clamp( std::chrono::duration<float>( now - this->m_spec_last_frame ).count( ), 0.0f, 0.1f );
		this->m_spec_last_frame = now;

		const auto pawn = this->resolve_spectate_pawn( );
		this->m_spec_pawn.store( pawn );

		if ( !pawn )
		{
			this->m_spec_last_pawn = 0;
			return;
		}

		const auto game_scene_node = memory::safe_read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
		if ( !game_scene_node )
		{
			return;
		}

		// The scene node origin is the interpolated, rendered one, so the position is smooth on its own.
		const auto origin = memory::safe_read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto view_offset = memory::safe_read<math::vector3>( pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
		const auto eye_angles = memory::safe_read<math::vector3>( pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
		if ( !origin.has_value( ) || !view_offset.has_value( ) || !eye_angles.has_value( ) )
		{
			return;
		}

		// A crouched or mid-duck offset is fine as it is; a zero one means it has not been networked yet, and
		// standing eye height is a better guess than the floor.
		auto offset = *view_offset;
		if ( offset.z < 1.0f )
		{
			offset = { 0.0f, 0.0f, 64.0f };
		}

		auto target_angles = *eye_angles;
		target_angles.z = 0.0f;
		math::helpers::normalize_angles( target_angles );

		if ( pawn != this->m_spec_last_pawn )
		{
			this->m_spec_angles = target_angles;
		}
		else
		{
			// Eye angles arrive once per network tick and are not interpolated, so used raw the view turns in
			// 64 Hz steps. A 20 ms follow turns those steps into continuous motion for about a tick of delay.
			auto delta = target_angles - this->m_spec_angles;
			math::helpers::normalize_angles( delta );

			const auto follow = 1.0f - std::exp( -dt / 0.02f );
			this->m_spec_angles = this->m_spec_angles + delta * follow;
			math::helpers::normalize_angles( this->m_spec_angles );
		}

		this->m_spec_last_pawn = pawn;

		const auto eye = *origin + offset;
		memory::write<math::vector3>( view_setup + k_origin_offset, eye );
		memory::write<math::vector3>( view_setup + k_angles_offset, this->m_spec_angles );

		// The game's OverrideView finishes by copying origin and angles into these three, and this hook runs
		// after it -- so they still held our own eye. Near the target the two views overlap and it hardly shows;
		// far away the world and everything in it was culled against where we stand, and nothing drew.
		memory::write<math::vector3>( view_setup + k_origin_copy_offset, eye );
		memory::write<math::vector3>( view_setup + k_angles_copy_offset, this->m_spec_angles );
		memory::write<math::vector3>( view_setup + k_origin_copy_2_offset, eye );
	}

	bool camera::hides_entity( std::uintptr_t entity, std::uint32_t schema_hash ) const
	{
		const auto pawn = this->m_spec_pawn.load( );
		if ( !pawn )
		{
			return false;
		}

		if ( entity == pawn || schema_hash == "C_CS2HudModelArms"_hash || schema_hash == "C_CS2HudModelWeapon"_hash )
		{
			return true;
		}

		// Their gun, defuser and the like are parented to the pawn's scene node.
		const auto node = memory::safe_read<std::uintptr_t>( entity + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
		if ( !node )
		{
			return false;
		}

		const auto parent = memory::safe_read<std::uintptr_t>( node + SCHEMA( "CGameSceneNode", "m_pParent"_hash ) ).value_or( 0 );
		if ( !parent )
		{
			return false;
		}

		return memory::safe_read<std::uintptr_t>( parent + SCHEMA( "CGameSceneNode", "m_pOwner"_hash ) ).value_or( 0 ) == pawn;
	}

	void camera::on_render( xdraw::draw_list& draw_list ) const
	{
		if ( !settings::g_misc.m_camera.spectate.value )
		{
			return;
		}

		const auto controller = this->m_spec_controller.load( );
		if ( !controller || !systems::g_entities.exists( controller ) )
		{
			return;
		}

		const auto name_ptr = memory::safe_read<std::uintptr_t>( controller + SCHEMA( "CCSPlayerController", "m_sSanitizedPlayerName"_hash ) ).value_or( 0 );
		const auto name = name_ptr ? memory::read_string( name_ptr, 63 ) : std::string{ "player" };

		const auto label = this->m_spec_pawn.load( ) ? "spectating " + name : name + " is dead - waiting";

		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto [tw, th] = xdraw::measure_text( label );
		const auto x = std::floorf( ( static_cast< float >( screen_w ) - tw ) * 0.5f );
		const auto y = 60.0f;

		draw_list.rect_filled( x - 8.0f, y - 4.0f, tw + 16.0f, th + 8.0f, xdraw::color{ 0, 0, 0, 160 } );
		draw_list.text( x, y, label, xui::ctx( ).style.accent );
	}

	void camera::update_fov_sensitivity( std::uintptr_t player_pawn ) const
	{
		if ( !settings::g_misc.m_camera.change_fov.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( player_pawn != local.pawn )
		{
			return;
		}

		const auto& cfg = settings::g_misc.m_camera;

		// This exact read is a captured crash: ACCESS_VIOLATION reading pawn+0x1C78 on the game thread,
		// six minutes into a session. The pointer had already been freed, and `player_pawn != local.pawn`
		// cannot catch that -- when the snapshot holds the same dead pawn, both sides of the compare are
		// equally dead. So probe the object here instead of trusting either source. A pawn that no longer
		// reads has no sensitivity left to adjust, and the next snapshot will bring a live one.
		const auto is_scoped = memory::safe_read<bool>( player_pawn + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) );
		if ( !is_scoped.has_value( ) )
		{
			return;
		}

		const auto target_fov = ( *is_scoped && cfg.scoped_fov_override.value ) ? cfg.scoped_fov.value : cfg.fov.value;

		if ( *is_scoped == this->m_cached_scoped && target_fov == this->m_cached_target_fov && this->m_cached_fov_sensitivity >= 0.0f )
		{
			const auto current_adjust = memory::safe_read<float>( player_pawn + SCHEMA( "C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_hash ) );
			if ( current_adjust.has_value( ) && std::fabsf( *current_adjust - this->m_cached_fov_sensitivity ) < 0.0001f )
			{
				return;
			}
		}

		this->m_cached_scoped = *is_scoped;
		this->m_cached_target_fov = target_fov;

		const auto ratio = CONVAR ("zoom_sensitivity_ratio")->get<float>( );
		const auto desired = ratio * ( target_fov / 90.0f );

		this->m_cached_fov_sensitivity = desired;
		memory::write<float>( player_pawn + SCHEMA( "C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_hash ), desired );
	}

	void camera::do_thirdperson( std::uintptr_t view_setup, std::uintptr_t local_pawn ) const
	{
		const auto& cfg = settings::g_misc.m_camera;
		if ( !cfg.thirdperson.value )
		{
			return;
		}

		// m_pGameSceneNode was read raw and dereferenced without a null check. On the frame the local pawn
		// is torn down -- round end, respawn, disconnect -- the node pointer is either gone or zero, and
		// the origin read below lands on nothing. Probe both, and leave the camera where the engine put it
		// rather than moving it to a position computed from garbage.
		const auto game_scene_node = memory::safe_read<std::uintptr_t>( local_pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
		if ( !game_scene_node )
		{
			return;
		}

		const auto absolute_origin = memory::safe_read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto pawn_view_offset = memory::safe_read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
		if ( !absolute_origin.has_value( ) || !pawn_view_offset.has_value( ) )
		{
			return;
		}

		const auto eye_position = *absolute_origin + *pawn_view_offset;
		const auto view_angles = systems::g_input.get_view_angles( );

		math::vector3 forward{};
		{
			math::helpers::angle_vectors_left( view_angles, &forward );
		}

		auto camera_position = eye_position - forward * cfg.thirdperson_distance;
		const auto hull = math::vector3{ -cfg.thirdperson_hull_size, -cfg.thirdperson_hull_size, -cfg.thirdperson_hull_size };
		const auto result = systems::g_tracing.trace_hull( eye_position, camera_position, hull, hull, local_pawn );

		if ( result.fraction < 1.0f )
		{
			const auto world = systems::g_entities.get_by_index( 0 );
			if ( result.hit_entity == world )
			{
				camera_position = eye_position + ( camera_position - eye_position ) * result.fraction;
			}
		}

		memory::write<math::vector3>( view_setup + 0x4a0, camera_position );
	}

	void camera::do_fov_change( std::uintptr_t view_setup, std::uintptr_t local_pawn ) const
	{
		const auto& cfg = settings::g_misc.m_camera;
		if ( !cfg.change_fov.value )
		{
			return;
		}

		// Same dead-pawn exposure as update_fov_sensitivity, but here a failed read is not a reason to
		// abandon the override -- treat an unreadable pawn as not scoped and still apply the base FOV,
		// which is what the player asked for and is written to the engine's view setup, not to the pawn.
		const auto is_scoped = memory::safe_read<bool>( local_pawn + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) ).value_or( false );
		const auto target_fov = ( is_scoped && cfg.scoped_fov_override.value ) ? cfg.scoped_fov.value : cfg.fov.value;

		memory::write<float>( view_setup + k_fov_offset, target_fov );

		this->update_fov_sensitivity( local_pawn );
	}

	void camera::do_aspect_ratio_change( std::uintptr_t view_setup )
	{
		const auto& cfg = settings::g_misc.m_camera;

		if ( cfg.change_aspect_ratio.value )
		{
			const auto base_fov = memory::read<float>( view_setup + k_fov_offset );
			const auto flags = memory::read<std::uint8_t>( view_setup + k_view_flags_offset );

			// Explicit aspect bypasses the game's native 4:3-based FOV conversion.
			memory::write<float>( view_setup + k_fov_offset, scale_horizontal_fov( base_fov, cfg.aspect_ratio ) );
			memory::write<float>( view_setup + k_aspect_ratio_offset, cfg.aspect_ratio );
			memory::write<std::uint8_t>( view_setup + k_view_flags_offset, flags | k_explicit_aspect_ratio_flag );
		}
		else
		{
			const auto flags = memory::read<std::uint8_t>( view_setup + k_view_flags_offset );
			memory::write<std::uint8_t>( view_setup + k_view_flags_offset,
				flags & static_cast<std::uint8_t>( ~k_explicit_aspect_ratio_flag ) );
		}
	}

} // namespace features::misc
