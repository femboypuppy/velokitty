#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
namespace features::combat {

	namespace {

		/// Bring an analog movement pair inside the +-1 the protobuf fields hold without turning it.
		///
		/// Rotating a diagonal overflows: F=1, L=1 at a 22.5 degree offset comes out 1.307 / 0.541, and
		/// clamping the two components independently keeps the 0.541 while cutting the 1.307 down to
		/// 1.0 -- a different direction, about 5.5 degrees off the one the player asked for, and the
		/// error grows with the offset. Dividing both by the larger magnitude keeps the direction exact
		/// and only shortens the vector, which costs nothing: the server normalises the wish direction
		/// and scales it by the weapon's max speed regardless of how long the pair was.
		[[nodiscard]] math::vector2 scale_into_range( float forward, float left )
		{
			const auto overflow = std::fmaxf( std::fabsf( forward ), std::fabsf( left ) );
			if ( overflow <= 1.0f )
			{
				return math::vector2{ forward, left };
			}

			return math::vector2{ forward / overflow, left / overflow };
		}

		/// The command tick, read the same way `get_mode_offset` reads it. Used to tell "the first
		/// CreateMove of this tick" from "the fourth", which the correction has to know: it is a
		/// per-frame callback applying a per-tick rotation.
		[[nodiscard]] int current_command_tick( )
		{
			const auto global_vars = memory::read<std::uintptr_t>( addresses::globals::global_vars );
			return global_vars ? memory::read<int>( global_vars + 0x44 ) : 0;
		}

	} // namespace

	void misc::antiaim::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_antiaim_active = false;

		// The movement basis is captured here, at the top of the first feature the pipeline runs, and it
		// is captured on every tick rather than only the ticks anti-aim wins. Anti-aim is not the only
		// thing that rewrites viewangles -- the ragebot's hide-shot and the projectile trajectory both
		// do -- and anti-aim itself stands down on a dozen conditions below: holding E, a grenade
		// mid-throw, a ladder, freezetime, or simply being switched off. Arming the correction only on
		// its own successful ticks left every one of those shipping the player's wish direction measured
		// against a yaw they were not looking at, which is the "sometimes it won't let me go where I'm
		// pointing" of it. When nothing moves the yaw the delta comes out zero and the correction costs
		// a subtraction and a compare.
		//
		// The camera comes first and the command's own viewangles are only the fallback. They agree on
		// the first CreateMove of a tick, but CreateMove is a per-frame callback: on the second and
		// later calls for the same command the protobuf can already be carrying the fake yaw this
		// function wrote a frame ago, and taking the basis from there would read the spin as the
		// player's own aim -- delta zero, no correction, and the doubly-rotated pair left in place.
		// csgo_input's angles cannot be contaminated that way; nothing in the cheat writes them.
		// cheat.cpp already calls get_view_angles( ) unconditionally on the line above this one
		// (airstrafe::store_angles), so it costs nothing new and is no less safe.
		const auto basis_base = cmd->csgo_user_cmd.mutable_base( );
		const auto basis_angles = basis_base ? basis_base->viewangles( ) : nullptr;
		const auto camera_yaw = systems::g_input.get_view_angles( ).y;

		this->m_basis_yaw = std::isfinite( camera_yaw ) ? camera_yaw
			: ( basis_angles ? basis_angles->y( ) : this->m_basis_yaw );
		this->m_should_correct = true;

		if ( !settings::g_combat.m_antiaim.enabled.value )
		{
			return;
		}

		if ( systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			return;
		}

		if ( settings::g_combat.m_antiaim.manual_left.value && settings::g_combat.m_antiaim.manual_right.value )
		{
			settings::g_combat.m_antiaim.manual_right.value = false;
			settings::g_combat.m_antiaim.manual_right.bind.active = false;
		}

		if ( settings::g_combat.m_antiaim.manual_left.value )
		{
			this->m_yaw_side = -1;
		}
		else if ( settings::g_combat.m_antiaim.manual_right.value )
		{
			this->m_yaw_side = 1;
		}
		else
		{
			this->m_yaw_side = 0;
		}

		const auto local = systems::g_local.get( );
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto view_angles = systems::g_input.get_view_angles( );
		const auto& ctx = g_shared.ctx( );

		if ( cmd->buttons.value & cstypes::command_buttons::in_use )
		{
			return;
		}

		if ( ctx.weapon_type == cstypes::weapon_type::grenade )
		{
			if ( memory::read<float>( ctx.weapon + SCHEMA( "C_BaseCSGrenade", "m_fThrowTime"_hash ) ) > 0.0f )  // bail regardless of pin state
			{
				return;
			}
		}

		const auto move_type = memory::read<int>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		this->m_old_angles = view_angles;
		this->m_antiaim_active = true;

		this->m_modified_angles = this->m_old_angles;
		this->m_modified_angles.x = this->get_pitch( this->m_old_angles.x );
		this->m_modified_angles.y = this->get_yaw( this->m_old_angles, local );

		math::helpers::normalize_angles( this->m_modified_angles );

		base->mutable_viewangles( )->set_x( this->m_modified_angles.x );
		base->mutable_viewangles( )->set_y( this->m_modified_angles.y );
		base->mutable_viewangles( )->set_z( this->m_modified_angles.z );
	}

	void misc::antiaim::on_render( xdraw::draw_list& draw_list ) const
	{
		// Drawn before the indicator's guards and independently of them: the whole point of the readout
		// is to be visible on the ticks the correction did nothing, which are the ticks the indicator is
		// hidden on. Everything here is a snapshot taken in CreateMove -- no game memory is touched.
		if ( settings::g_combat.m_antiaim.movement_debug.value )
		{
			const auto state_name = [ this ]( ) -> const char*
				{
					switch ( this->m_dbg_state )
					{
					case correction_state::disarmed:      return "disarmed (no create_move)";
					case correction_state::strafer_owned: return "test_strafer owns the tick";
					case correction_state::no_base:       return "no usercmd base";
					case correction_state::no_angles:     return "no viewangles";
					case correction_state::no_delta:      return "delta below threshold";
					case correction_state::rotated:       return "rotated";
					case correction_state::replayed:      return "rotated (replay guarded)";
					default:                              return "idle";
					}
				}( );

			char lines[ 8 ][ 96 ]{};

			std::snprintf( lines[ 0 ], sizeof( lines[ 0 ] ), "state      %s", state_name );
			std::snprintf( lines[ 1 ], sizeof( lines[ 1 ] ), "calls/tick %d", this->m_dbg_calls );
			std::snprintf( lines[ 2 ], sizeof( lines[ 2 ] ), "camera yaw %.2f", this->m_dbg_camera_yaw );
			std::snprintf( lines[ 3 ], sizeof( lines[ 3 ] ), "sent yaw   %.2f", this->m_dbg_sent_yaw );
			std::snprintf( lines[ 4 ], sizeof( lines[ 4 ] ), "delta      %.2f", this->m_dbg_delta );
			std::snprintf( lines[ 5 ], sizeof( lines[ 5 ] ), "move in    %.2f / %.2f", this->m_dbg_in.x, this->m_dbg_in.y );
			std::snprintf( lines[ 6 ], sizeof( lines[ 6 ] ), "move out   %.2f / %.2f", this->m_dbg_out.x, this->m_dbg_out.y );
			std::snprintf( lines[ 7 ], sizeof( lines[ 7 ] ), "subticks   %d", this->m_dbg_steps );

			constexpr auto pad{ 8.0f };
			constexpr auto panel_x{ 16.0f };
			constexpr auto panel_y{ 320.0f };

			const auto [ title_w, title_h ] = xdraw::measure_text( "movement debug" );

			auto widest = title_w;
			auto line_h = title_h;

			for ( const auto& line : lines )
			{
				const auto [ w, h ] = xdraw::measure_text( line );
				widest = std::fmaxf( widest, w );
				line_h = std::fmaxf( line_h, h );
			}

			const auto rows = static_cast< float >( std::size( lines ) + 1 );

			draw_list.rect_filled( panel_x, panel_y, widest + pad * 2.0f, rows * line_h + pad * 2.0f, xdraw::color{ 12, 12, 14, 200 }, 6.0f );

			auto cursor_y = panel_y + pad;
			draw_list.text( panel_x + pad, cursor_y, "movement debug", settings::g_combat.m_antiaim.direction_indicator_color.value );
			cursor_y += line_h;

			for ( const auto& line : lines )
			{
				draw_list.text( panel_x + pad, cursor_y, line, xdraw::color{ 221, 229, 255, 235 } );
				cursor_y += line_h;
			}
		}

		if ( !settings::g_combat.m_antiaim.enabled.value || !settings::g_combat.m_antiaim.direction_indicator.value )
		{
			return;
		}

		if ( !this->m_antiaim_active )
		{
			return;
		}

		if ( !systems::g_frame_data.valid( ) )
		{
			return;
		}

		const auto origin = systems::g_frame_data.origin( );
		const auto aa_yaw_rad = this->m_indicator_yaw * ( std::numbers::pi_v<float> / 180.0f );

		constexpr auto radius{ 28.0f };
		constexpr auto feet_offset{ -2.0f };
		constexpr auto arc_sweep_deg{ 60.0f };
		constexpr auto arc_segments{ 48 };
		constexpr auto max_thickness{ 3.0f };

		const auto& cfg = settings::g_combat.m_antiaim;
		const auto& color = cfg.direction_indicator_color;
		const auto base = math::vector3{ origin.x, origin.y, origin.z + feet_offset };

		const auto half_sweep = ( arc_sweep_deg * 0.5f ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto start_angle = aa_yaw_rad - half_sweep;
		const auto end_angle = aa_yaw_rad + half_sweep;
		const auto angle_step = ( end_angle - start_angle ) / static_cast< float >( arc_segments );

		std::vector<math::vector2> pts;
		pts.reserve( arc_segments + 1 );

		for ( auto i = 0; i <= arc_segments; ++i )
		{
			const auto angle = start_angle + angle_step * static_cast< float >( i );

			const auto world_pt = math::vector3
			{
				base.x + std::cosf( angle ) * radius,
				base.y + std::sinf( angle ) * radius,
				base.z
			};

			const auto sp = systems::g_view.project( world_pt );

			if ( !systems::g_view.projection_valid( sp ) )
			{
				return;
			}

			pts.push_back( { sp.x, sp.y } );
		}

		if ( pts.size( ) < 2 )
		{
			return;
		}

		const auto total = static_cast< float >( pts.size( ) - 1 );

		const auto fade_at = [ ]( std::size_t idx, float total ) -> float
			{
				const auto frac = static_cast< float >( idx ) / total;
				const auto edge = 1.0f - std::fabsf( frac - 0.5f ) * 2.0f;
				return edge * edge * edge * ( edge * ( edge * 6.0f - 15.0f ) + 10.0f );
			};

		if ( cfg.direction_indicator_glow )
		{
			auto& glow = xdraw::get_glow( );

			for ( auto i = 0ull; i + 1 < pts.size( ); ++i )
			{
				const auto f0 = fade_at( i, total );
				const auto f1 = fade_at( i + 1, total );

				const auto ga0 = static_cast< std::uint8_t >( static_cast< float >( color.value.a ) * cfg.direction_indicator_glow_strength * std::fmaxf( f0, 0.05f ) );
				const auto ga1 = static_cast< std::uint8_t >( static_cast< float >( color.value.a ) * cfg.direction_indicator_glow_strength * std::fmaxf( f1, 0.05f ) );

				const auto thickness = ( max_thickness + 2.0f ) * ( ( f0 + f1 ) * 0.5f * 0.85f + 0.15f );

				const float seg[ ]{ pts[ i ].x, pts[ i ].y, pts[ i + 1 ].x, pts[ i + 1 ].y };
				const xdraw::color cols[ ]{ { color.value.r, color.value.g, color.value.b, ga0 }, { color.value.r, color.value.g, color.value.b, ga1 } };

				glow.polyline_gradient( seg, cols, false, thickness );
			}
		}

		for ( auto i = 0ull; i + 1 < pts.size( ); ++i )
		{
			const auto f0 = fade_at( i, total );
			const auto f1 = fade_at( i + 1, total );

			const auto a0 = static_cast< std::uint8_t >( color.value.a * std::fmaxf( f0, 0.05f ) );
			const auto a1 = static_cast< std::uint8_t >( color.value.a * std::fmaxf( f1, 0.05f ) );

			const auto thickness = max_thickness * ( ( f0 + f1 ) * 0.5f * 0.85f + 0.15f );

			const float seg[ ]{ pts[ i ].x, pts[ i ].y, pts[ i + 1 ].x, pts[ i + 1 ].y };
			const xdraw::color cols[ ]{ { color.value.r, color.value.g, color.value.b, a0 }, { color.value.r, color.value.g, color.value.b, a1 } };

			draw_list.polyline_gradient( seg, cols, false, thickness );
		}
	}

	float misc::antiaim::get_pitch( float view_pitch )
	{
		switch ( settings::g_combat.m_antiaim.pitch )
		{
		case settings::combat::antiaim::pitch_mode::down:
			return 89.0f;
		case settings::combat::antiaim::pitch_mode::up:
			return -89.0f;
		default:
			return view_pitch;
		}
	}

	float misc::antiaim::get_yaw( const math::vector3& view_angles, const systems::local::snapshot& local )
	{
		auto base_yaw_offset{ 180.0f };

		const auto view_yaw = view_angles.y;
		auto base_yaw = view_yaw - base_yaw_offset;

		const auto local_game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto local_origin = memory::read<math::vector3>( local_game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );
		const auto eye_pos = local_origin + memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );

		if ( settings::g_combat.m_antiaim.avoid_backstab.value )
		{
			constexpr auto backstab_range_sq = 350.0f * 350.0f;
			auto knife_dist = std::numeric_limits<float>::max( );
			auto knife_yaw{ 0.0f };
			auto knife_found{ false };

			for ( const auto& p : players )
			{
				if ( !p.ptr || p.ptr == local.controller )
				{
					continue;
				}

				if ( !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
				{
					continue;
				}

				const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
				const auto pawn = systems::g_entities.lookup( pawn_handle );

				if ( !pawn || pawn == local.pawn )
				{
					continue;
				}

				const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
				if ( !local.is_this_other_team( team ) )
				{
					continue;
				}

				const auto health = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
				if ( health <= 0 )
				{
					continue;
				}

				const auto enemy_game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !enemy_game_scene_node )
				{
					continue;
				}

				const auto enemy_origin = memory::read<math::vector3>( enemy_game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
				const auto dx = enemy_origin.x - local_origin.x;
				const auto dy = enemy_origin.y - local_origin.y;
				const auto dist_sq = dx * dx + dy * dy;

				if ( dist_sq > backstab_range_sq )
				{
					continue;
				}

				const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
				if ( !weapon_services )
				{
					continue;
				}

				const auto weapon_handle = memory::read<std::uint32_t>( weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hActiveWeapon"_hash ) );
				if ( !weapon_handle )
				{
					continue;
				}

				const auto weapon = systems::g_entities.lookup( weapon_handle );
				if ( !weapon )
				{
					continue;
				}

				const auto weapon_vdata = memory::read<std::uintptr_t>( weapon + SCHEMA( "C_BaseEntity", "m_nSubclassID"_hash ) + 0x8 );
				if ( !weapon_vdata )
				{
					continue;
				}

				const auto weapon_type = memory::read<std::uint32_t>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_WeaponType"_hash ) );
				if ( weapon_type != cstypes::weapon_type::knife )
				{
					continue;
				}

				if ( dist_sq < knife_dist )
				{
					knife_dist = dist_sq;
					knife_yaw = std::atan2f( dy, dx ) * ( 180.0f / std::numbers::pi_v<float> );
					knife_found = true;
				}
			}

			if ( knife_found )
			{
				this->m_indicator_yaw = knife_yaw;
				return knife_yaw;
			}
		}

		const auto pick_target_yaw = [ & ]( ) -> std::optional<float>
			{
				auto best_yaw = base_yaw;
				auto best_threat_score = std::numeric_limits<float>::max( );

				for ( const auto& p : players )
				{
					if ( !p.ptr || p.ptr == local.controller )
					{
						continue;
					}

					if ( !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
					{
						continue;
					}

					const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
					const auto pawn = systems::g_entities.lookup( pawn_handle );

					if ( !pawn || pawn == local.pawn )
					{
						continue;
					}

					const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
					if ( !local.is_this_other_team( team ) )
					{
						continue;
					}

					if ( memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 )
					{
						continue;
					}

					if ( memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
					{
						continue;
					}

					const auto enemy_game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
					if ( !enemy_game_scene_node )
					{
						continue;
					}

					const auto enemy_origin = memory::read<math::vector3>( enemy_game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
					const auto enemy_eye_pos = enemy_origin + memory::read<math::vector3>( pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
					const auto angle_to_enemy = math::helpers::calculate_angle( eye_pos, enemy_eye_pos );
					const auto fov = math::helpers::angle_distance( view_angles, angle_to_enemy );
					const auto distance = eye_pos.distance( enemy_eye_pos );

					// Match Requiem's threat order: crosshair first, then proximity, whether
					// the enemy is looking at us, and finally whether they are visible.
					auto threat_score = fov * 4.0f + distance * 0.01f;

					math::vector3 enemy_forward{};
					const auto enemy_eye_angles = memory::read<math::vector3>( pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
					math::helpers::angle_vectors_left( enemy_eye_angles, &enemy_forward );
					const auto direction_to_us = ( eye_pos - enemy_eye_pos ).normalized( );
					threat_score -= std::clamp( enemy_forward.dot( direction_to_us ), -1.0f, 1.0f ) * 25.0f;

					if ( systems::g_tracing.is_visible( eye_pos, enemy_eye_pos, pawn, local.pawn ) )
					{
						threat_score -= 15.0f;
					}

					if ( threat_score < best_threat_score )
					{
						best_threat_score = threat_score;
						best_yaw = angle_to_enemy.y - base_yaw_offset;
					}
				}

				return best_threat_score < std::numeric_limits<float>::max( ) ? std::optional<float>{ best_yaw } : std::nullopt;
			};

		if ( const auto target_yaw = pick_target_yaw( ) )
		{
			base_yaw = *target_yaw;
		}

		// The selected yaw mode rides on top of the backwards/threat-facing base, so jitter and
		// spin still track the enemy the base picked instead of ignoring it.
		base_yaw += this->get_mode_offset( );

		auto yaw = base_yaw;
		if ( this->m_yaw_side == -1 )
		{
			yaw -= 90.0f;
		}
		else if ( this->m_yaw_side == 1 )
		{
			yaw += 90.0f;
		}

		if (settings::g_combat.m_antiaim.auto_yaw_adjust.value)
			yaw += 33.0f; // thx saphy

		// The indicator used to be computed before the +33 compensation, so the on-screen arc
		// sat a permanent 33 degrees off the yaw actually being sent. Derive it from the final
		// value instead — one source of truth, and the new modes come along for free.
		this->m_indicator_yaw = yaw;

		return yaw;
	}

	float misc::antiaim::movement_basis_yaw( const proto::base_usercmd_pb* base ) const
	{
		// m_basis_yaw is refreshed at the top of every CreateMove from the command's own untouched
		// viewangles, so it is the right answer whether anti-aim is spinning, standing down, or off --
		// and it stays right when the ragebot or the trajectory code rewrites the yaw later in the
		// pipeline. Reading base->viewangles( ) here instead, as this used to when anti-aim was
		// inactive, meant picking up whichever fake yaw had been written by the time the caller ran.
		( void )base;
		return this->m_basis_yaw;
	}

	void misc::antiaim::rotate_subtick_moves( proto::base_usercmd_pb* base, const math::vector2& start, float sin_delta, float cos_delta ) const
	{
		// The engine treats each step as an increment on a running total, so the deltas cannot be
		// rotated where they sit: R(a - b) is not (R(a) - b), and the total the server starts from
		// belongs to last tick's basis. Walk the chain instead, rebuilding the absolute value each
		// step described, rotating that, and re-differencing against the rotated running total.
		auto raw = start;
		auto rotated = start;

		for ( auto i = 0; i < base->subtick_moves_size( ); ++i )
		{
			const auto step = base->mutable_subtick_moves( i );
			if ( !step )
			{
				continue;
			}

			const auto has_forward = step->m_has_bits.test( 0x8 );
			const auto has_left = step->m_has_bits.test( 0x10 );

			if ( !has_forward && !has_left )
			{
				continue;
			}

			// A step may carry only one of the pair; the other component holds its previous value.
			if ( has_forward )
			{
				raw.x += step->analog_forward_delta( );
			}

			if ( has_left )
			{
				raw.y += step->analog_left_delta( );
			}

			// Same uniform scaling as the scalar pair: clamping the two components apart from each other
			// would turn the step's direction instead of merely shortening it.
			const auto target = scale_into_range( raw.x * cos_delta + raw.y * sin_delta, -raw.x * sin_delta + raw.y * cos_delta );
			const auto target_x = target.x;
			const auto target_y = target.y;

			// Both components are written even when only one had a delta: the rotation mixes them,
			// so a change in forward alone still moves the left component of the world direction.
			step->set_analog_forward_delta( target_x - rotated.x );
			step->set_analog_left_delta( target_y - rotated.y );

			rotated.x = target_x;
			rotated.y = target_y;
		}
	}

	void misc::antiaim::apply_movement_correction( systems::input::usercmd* cmd )
	{
		// Every path out of here leaves the readout current, including the ones that do nothing --
		// "why did it not rotate" is the question the on-screen numbers exist to answer.
		const auto tick = current_command_tick( );
		this->m_dbg_calls = ( tick == this->m_corrected_tick ) ? this->m_dbg_calls + 1 : 1;
		this->m_dbg_camera_yaw = this->m_basis_yaw;
		this->m_dbg_state = correction_state::rotated;
		this->m_dbg_steps = 0;

		if ( !this->m_should_correct )
		{
			this->m_dbg_state = correction_state::disarmed;
			return;
		}

		this->m_should_correct = false;

		// test_strafer steers by yaw, not by the analog pair, and it already accounts for the fake yaw
		// itself: it seeds its accumulator from base->viewangles and emits deltas that walk the server's
		// view angle onto the strafe direction it wants. forwardmove/leftmove have to stay as the player
		// pressed them for that to land. Rotating them here on top rotates the wish direction a second
		// time, so the strafe came out off by the whole anti-aim offset -- and since airstrafe stands
		// down whenever the strafer handled the tick, that was every airborne tick with it enabled.
		// It has no analog subtick steps of its own either, so there is nothing left to rotate.
		if ( features::movement::g_test_strafer.handled_this_tick( ) )
		{
			this->m_dbg_state = correction_state::strafer_owned;
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			this->m_dbg_state = correction_state::no_base;
			return;
		}

		// Read the yaw the command is actually shipping rather than m_modified_angles: rage
		// hide-shots and projectile trajectory both overwrite viewangles after anti-aim runs.
		const auto sent_angles = base->viewangles( );
		if ( !sent_angles )
		{
			this->m_dbg_state = correction_state::no_angles;
			return;
		}

		// Undo our own last rotation before measuring anything, if this is a repeat call for the tick
		// we already rotated and the pair still holds the bits we left there. The game rewrites
		// forwardmove/leftmove from fresh input on most repeat calls, in which case the compare fails
		// and there is nothing to undo; when it does not, this is the difference between rotating by
		// delta and rotating by delta once per frame.
		auto forward_move = base->forwardmove( );
		auto side_move = base->leftmove( );
		auto replayed = false;

		if ( tick == this->m_corrected_tick && forward_move == this->m_corrected_out.x && side_move == this->m_corrected_out.y )
		{
			forward_move = this->m_corrected_in.x;
			side_move = this->m_corrected_in.y;
			replayed = true;
		}

		this->m_dbg_sent_yaw = sent_angles->y( );
		this->m_dbg_in = math::vector2{ forward_move, side_move };
		this->m_dbg_out = this->m_dbg_in;

		auto delta = sent_angles->y( ) - this->m_basis_yaw;
		math::helpers::normalize_angle( delta );
		this->m_dbg_delta = delta;

		// Nothing moved the yaw this tick, which is the common case now that the correction is armed on
		// every tick rather than only anti-aim's own. A zero rotation would still walk the subtick chain
		// and re-difference every step against a float-rounded copy of itself, so stop here instead.
		if ( std::fabsf( delta ) < 0.01f )
		{
			// A replay still has to be written back: the pair currently in the protobuf is last frame's
			// rotated output, and this frame's delta is zero, so leaving it would ship a rotation that
			// no longer corresponds to any offset.
			if ( replayed )
			{
				base->set_forwardmove( forward_move );
				base->set_leftmove( side_move );
			}

			this->m_corrected_tick = -1;
			this->m_dbg_state = correction_state::no_delta;
			return;
		}

		const auto delta_rad = delta * ( std::numbers::pi_v<float> / 180.0f );
		const auto sin_delta = std::sinf( delta_rad );
		const auto cos_delta = std::cosf( delta_rad );

		// The server builds the wish direction as
		//     forward( yaw ) * forwardmove + left( yaw ) * leftmove
		// with left = ( -sin yaw, cos yaw ) — the basis edgestop reconstructs at edgestop.cpp:124
		// and autostop projects onto at misc.cpp:1031. Holding that world direction fixed while
		// the sent yaw moves by delta means expressing it in the rotated frame, which is R(-delta):
		//     forward' =  forward * cos + side * sin
		//     side'    = -forward * sin + side * cos
		const auto corrected_forward = forward_move * cos_delta + side_move * sin_delta;
		const auto corrected_side = -forward_move * sin_delta + side_move * cos_delta;

		const auto scaled = scale_into_range( corrected_forward, corrected_side );

		base->set_forwardmove( scaled.x );
		base->set_leftmove( scaled.y );

		this->m_corrected_tick = tick;
		this->m_corrected_in = math::vector2{ forward_move, side_move };
		this->m_corrected_out = math::vector2{ base->forwardmove( ), base->leftmove( ) };

		this->m_dbg_out = this->m_corrected_out;
		this->m_dbg_state = replayed ? correction_state::replayed : correction_state::rotated;

		// Airstrafe and autostop build their wish direction against movement_basis_yaw(), so their
		// subtick steps are in camera space too and need the same rotation. Skipping them (as this
		// function used to) left every airborne tick uncorrected, which is exactly when bhopping and
		// strafing happen. test_strafer is not in that list -- it returned above.
		//
		// The steps need no replay guard of their own: input::desubtick clears the whole chain at the
		// top of every CreateMove, so whatever is here was rebuilt by the features this frame.
		const auto& impulses = systems::g_prediction.pre( ).last_movement_impulses;
		this->m_dbg_steps = base->subtick_moves_size( );
		this->rotate_subtick_moves( base, math::vector2{ impulses.x, impulses.y }, sin_delta, cos_delta );

		// Deliberately no button rewriting. The server derives movement from the analog fields,
		// and downstream features (airstrafe, test_strafer) read cmd->buttons for the player's
		// real intent — the old code inverted the sign convention there and steered them wrong.
	}


	float misc::antiaim::get_mode_offset( )
	{
		const auto& cfg = settings::g_combat.m_antiaim;

		const auto global_vars = memory::read<std::uintptr_t>( addresses::globals::global_vars );
		if ( !global_vars )
		{
			return 0.0f;
		}

		const auto curtime = memory::read<float>( global_vars + 0x30 );
		const auto tick = memory::read<int>( global_vars + 0x44 );

		// CreateMove can run twice for the same tick. Animating on every call makes the spin
		// rate depend on framerate and lets jitter flip back to the side it started on.
		const auto advance = tick != this->m_last_yaw_tick;
		this->m_last_yaw_tick = tick;

		switch ( cfg.yaw.value )
		{
		case settings::combat::antiaim::yaw_mode::jitter:
		{
			// Step index off wall-clock rather than a tick counter, so the flip rate is the
			// same whether the server runs 64 or 128 tick.
			const auto speed = std::fmaxf( cfg.jitter_speed.value, 0.0f );
			const auto step = static_cast< int >( curtime * speed );

			if ( advance && step != this->m_jitter_step )
			{
				this->m_jitter_step = step;
				this->m_jitter_side = !this->m_jitter_side;
			}

			const auto half = cfg.jitter_range.value * 0.5f;
			return this->m_jitter_side ? half : -half;
		}

		case settings::combat::antiaim::yaw_mode::spin:
		{
			if ( advance )
			{
				this->m_spin_yaw += cfg.spin_speed.value * cstypes::tick_interval;
				math::helpers::normalize_angle( this->m_spin_yaw );
			}

			return this->m_spin_yaw;
		}

		case settings::combat::antiaim::yaw_mode::random:
		{
			const auto speed = std::fmaxf( cfg.random_speed.value, 0.0f );
			const auto step = static_cast< int >( curtime * speed );

			if ( advance && step != this->m_random_step )
			{
				this->m_random_step = step;

				// Seeded once and kept: a fresh engine per reroll seeded from the clock would
				// correlate consecutive picks whenever two rerolls land in the same millisecond.
				static std::mt19937 engine{ std::random_device{}( ) };
				std::uniform_real_distribution<float> dist{ -0.5f, 0.5f };
				this->m_random_yaw = dist( engine ) * cfg.random_range.value;
			}

			return this->m_random_yaw;
		}

		default:
			return cfg.yaw_offset.value;
		}
	}

	namespace {

		math::vector3 quickpeek_ground_snap( std::uintptr_t skip_pawn, const math::vector3& feet_pos )
		{
			const auto start = math::vector3{ feet_pos.x, feet_pos.y, feet_pos.z + 64.0f };
			const auto end = math::vector3{ feet_pos.x, feet_pos.y, feet_pos.z - 8192.0f };
			const auto tr = systems::g_tracing.trace( start, end, skip_pawn );

			if ( tr.fraction <= 0.0f || tr.fraction >= 0.997f )
			{
				return feet_pos;
			}

			auto out = tr.position;
			out.z += 1.0f;
			return out;
		}

	} // namespace

	void misc::duckpeek::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_fake_stand_active = false;

		if ( !cmd || !settings::g_combat.m_duckpeek.enabled.value )
		{
			this->m_was_active = false;
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn || systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			this->m_was_active = false;
			return;
		}

		this->m_was_active = true;

		if ( g_rage.should_release_duck_for_shot( ) )
		{
			cmd->buttons.value &= ~cstypes::command_buttons::in_duck;
			this->m_fake_stand_active = true;
			return;
		}

		cmd->buttons.value |= cstypes::command_buttons::in_duck;

		if ( g_rage.duckpeek_wants_reduck( ) )
		{
			g_rage.clear_duckpeek_reduck( );
		}
	}

	void misc::duckpeek::on_override_view( std::uintptr_t view_setup )
	{
		(void)view_setup;

		if ( !settings::g_combat.m_duckpeek.enabled.value )
		{
			this->m_was_active = false;
			this->m_fake_stand_active = false;
		}
	}

	void misc::quickpeek::on_create_move( systems::input::usercmd* cmd )
	{
		if ( !settings::g_combat.m_quickpeek.enabled.value )
		{
			this->reset( );
			return;
		}

		const auto local = systems::g_local.get( );
		const auto& ctx = g_shared.ctx( );

		if ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			this->reset( );
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		constexpr auto movement_cancel_mask = static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );
		const auto curr_movement_bits = cmd->buttons.value & movement_cancel_mask;

		const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );

		if ( this->m_saved_origin.length_sqr( ) < 0.001f )
		{
			this->m_saved_origin = quickpeek_ground_snap( local.pawn, origin );
			this->m_should_retrack = false;
			this->m_fired = false;
			this->m_active = true;
			this->create_particle( );
			this->m_prev_movement_bits = curr_movement_bits;
			return;
		}

		this->update_particle( );

		const auto distance = ( origin - this->m_saved_origin ).length_2d( );

		if ( this->m_should_retrack && ( curr_movement_bits & ~this->m_prev_movement_bits ) != 0 )
		{
			this->m_should_retrack = false;
		}

		if ( this->m_should_retrack && ( systems::g_prediction.pre( ).flags & cstypes::entity_flags::on_ground ) )
		{
			const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
			const auto speed = velocity.length_2d( );

			if ( distance < 5.0f && speed < 15.0f )
			{
				this->m_should_retrack = false;
				this->m_fired = false;
			}
			else if ( distance < speed * 0.1f && speed > 15.0f )
			{
				const auto vel_angle = math::helpers::vector_to_angle( velocity * -1.0f );
				const auto yaw_diff = math::helpers::deg_to_rad( features::combat::g_misc.antiaim( ).movement_basis_yaw( base ) - vel_angle.y );

				base->set_forwardmove( std::cosf( yaw_diff ) );
				base->set_leftmove( -std::sinf( yaw_diff ) );

				auto buttons = cmd->buttons.value;
				buttons &= ~static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );

				if ( base->forwardmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_forward;
				}
				else if ( base->forwardmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_back;
				}

				if ( base->leftmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveleft;
				}
				else if ( base->leftmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveright;
				}

				cmd->buttons.value = buttons;
			}
			else
			{
				const auto diff = this->m_saved_origin - origin;
				const auto angle_to_pos = math::helpers::vector_to_angle( diff );

				// Camera yaw for the same reason autostop uses it: the walk-back vector is expressed in
				// the basis the player is looking along and rotated into the sent basis afterwards.
				const auto yaw_diff = math::helpers::deg_to_rad( features::combat::g_misc.antiaim( ).movement_basis_yaw( base ) - angle_to_pos.y );

				base->set_forwardmove( std::cosf( yaw_diff ) );
				base->set_leftmove( -std::sinf( yaw_diff ) );

				auto buttons = cmd->buttons.value;
				buttons &= ~static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );

				if ( base->forwardmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_forward;
				}
				else if ( base->forwardmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_back;
				}

				if ( base->leftmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveleft;
				}
				else if ( base->leftmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveright;
				}

				cmd->buttons.value = buttons;
			}
		}

		if ( ( cmd->buttons.value & cstypes::command_buttons::in_attack ) && !g_rage.is_cocking_revolver( ) )
		{
			this->m_should_retrack = true;
			this->m_fired = true;
		}

		this->m_prev_movement_bits = curr_movement_bits;
	}

	void misc::quickpeek::reset_if_needed( )
	{
		if ( !this->m_active )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn )
		{
			this->reset( );
			return;
		}

		if ( !settings::g_combat.m_quickpeek.enabled.value )
		{
			this->reset( );
		}
	}

	void misc::quickpeek::create_particle( )
	{
		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( !particle_manager )
		{
			return;
		}

		constexpr auto particle_path{ "particles/embedded/halo.vpcf" };

		if ( !this->m_particle_loaded )
		{
			struct buffer_string
			{
				std::uint32_t m_unknown1{};
				std::uint32_t m_unknown2{ 0xc00000c8 };

				union
				{
					std::uintptr_t m_str_ptr;
					std::uint8_t data[ 0xc8 ];
				};

				std::uintptr_t m_unknown3{ 0 };
				std::uintptr_t m_unknown4{ 0 };
			} buffer;

			memory::call<void>(PATTERN (patterns::init_particle_path_buffer), &buffer, particle_path );
			buffer.m_unknown4 = 'fcpv';
			memory::call<void>(PATTERN (patterns::resource_system_precache), addresses::globals::resource_system, &buffer, "" );

			this->m_particle_loaded = true;
		}

		auto effect_index{ invalid_effect_index };
		memory::call<int*>(PATTERN (patterns::particle_create_effect), particle_manager, &effect_index, particle_path, 8, 0ll, 0ll, 0ll, 0 );

		this->m_particle_effect = effect_index;

		if ( effect_index == invalid_effect_index )
		{
			return;
		}

		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, effect_index, 0, &this->m_saved_origin, 0 );
	}

	void misc::quickpeek::update_particle( )
	{
		if ( this->m_particle_effect == invalid_effect_index )
		{
			return;
		}

		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( !particle_manager )
		{
			return;
		}

		const auto& cfg = settings::g_combat.m_quickpeek;
		const auto& col = this->m_should_retrack ? cfg.retrack_color : cfg.color;
		const auto color = math::vector3{ static_cast< float >( col.value.r ), static_cast< float >( col.value.g ), static_cast< float >( col.value.b ) };

		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, this->m_particle_effect, 1, &color, 0 );
		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, this->m_particle_effect, 0, &this->m_saved_origin, 0 );
	}

	void misc::quickpeek::release_particle( )
	{
		if ( this->m_particle_effect == invalid_effect_index )
		{
			return;
		}

		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( particle_manager )
		{
			memory::call<void>(PATTERN (patterns::particle_destroy_effect), particle_manager, this->m_particle_effect, true, true );
		}

		this->m_particle_effect = invalid_effect_index;
	}

	void misc::quickpeek::reset( )
	{
		this->release_particle( );
		this->m_saved_origin = {};
		this->m_should_retrack = false;
		this->m_fired = false;
		this->m_active = false;
		this->m_prev_movement_bits = 0;
	}

	bool misc::autostop::wants_rage_stop( ) const
	{
		return features::combat::g_rage.should_stop( );
	}

	bool misc::autostop::wants_manual_stop( ) const
	{
		if ( !settings::g_combat.m_autostop.enabled.value )
		{
			return false;
		}

		const auto& ctx = g_shared.ctx( );
		if ( !ctx.valid )
		{
			return false;
		}

		// Knives and grenades have no accuracy penalty to counter-strafe away.
		if ( ctx.weapon_type == cstypes::weapon_type::knife || ctx.weapon_type == cstypes::weapon_type::grenade )
		{
			return false;
		}

		// Only brake when the ragebot actually resolved someone. Holding the key used to stop
		// you for any live weapon, which glued you to the floor alone on a wall.
		//
		// This reads one tick behind: autostop runs before rage in the create_move pipeline
		// (see hooks/impl/cheat.cpp), so has_target( ) reports the previous command. That is
		// deliberate — moving autostop after rage would also move it after slowwalk, edgebug
		// and bhop and let it stomp them. 15.6 ms of latency on engaging the brake is not
		// perceptible; the release is immediate because rage clears its target before the
		// context guard.
		return features::combat::g_rage.has_target( );
	}

	void misc::autostop::on_create_move( systems::input::usercmd* cmd )
	{
		const auto rage_stop = this->wants_rage_stop( );
		const auto manual_stop = this->wants_manual_stop( );

		if ( !rage_stop && !manual_stop )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		const auto& ctx = g_shared.ctx( );

		if ( !( prestate.flags & cstypes::entity_flags::on_ground ) )
		{
			return;
		}

		// A jump scout owns the command while airborne and on the landing tick; stopping
		// here would cancel the hop before it leaves the ground.
		if ( features::combat::g_misc.jumpscout( ).active_this_tick( ) )
		{
			return;
		}

		if ( ctx.weapon_max_speed <= 0.0f )
		{
			return;
		}

		auto velocity = prestate.networked_velocity;
		auto speed = velocity.length_2d( );

		if ( speed <= 1.0f )
		{
			return;
		}

		const auto sv_friction = CONVAR ("sv_friction")->get<float>( );
		const auto sv_stopspeed = CONVAR ("sv_stopspeed")->get<float>( );
		const auto surface_friction = prestate.surface_friction;

		const auto control = std::fmaxf( speed, sv_stopspeed );
		const auto drop = control * sv_friction * surface_friction * cstypes::tick_interval;
		const auto post_friction = std::fmaxf( speed - drop, 0.0f );

		if ( post_friction > 0.0f )
		{
			velocity *= ( post_friction / speed );
			speed = post_friction;
		}
		else
		{
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			return;
		}

		if ( speed < 2.0f )
		{
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			return;
		}

		auto accel = CONVAR ("sv_accelerate")->get<float>( );
		const auto accel_base = this->get_effective_accel_base( local.pawn, movement_services, prestate.flags, ctx.weapon_max_speed );

		if ( ctx.is_scoped )
		{
			const auto weapon_ratio = std::fminf( 1.0f, ctx.weapon_max_speed / 250.0f );
			const auto v20 = std::fmaxf( 250.0f, memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) ) ) * weapon_ratio;
			const auto scoped_max = v20 * 0.52f;

			if ( speed > scoped_max - 5.0f )
			{
				const auto t = 1.0f - std::fmaxf( 0.0f, speed - ( scoped_max - 5.0f ) ) / std::fmaxf( 0.01f, 5.0f );
				accel *= std::clamp( t, 0.0f, 1.0f );
			}
		}

		const auto wish_x = -velocity.x / speed;
		const auto wish_y = -velocity.y / speed;
		const auto accel_speed = std::fminf( accel * accel_base * surface_friction * cstypes::tick_interval, speed );

		velocity.x += wish_x * accel_speed;
		velocity.y += wish_y * accel_speed;

		// Scaling the analog input with the remaining speed brakes gently near the end; holding
		// full deflection instead trades a little overshoot for a faster stop.
		const auto move_magnitude = settings::g_combat.m_autostop.aggressive.value
			? 1.0f
			: std::clamp( speed / ctx.weapon_max_speed, 0.0f, 1.0f );
		// Camera yaw, not the yaw on the wire. The brake vector is built here and rotated into the sent
		// basis later by apply_movement_correction, so building it against a fake yaw rotated it twice
		// and pointed the brake off true by the whole anti-aim offset -- accelerating sideways out of
		// the stop rather than into it.
		const auto yaw_rad = features::combat::g_misc.antiaim( ).movement_basis_yaw( base ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto sy = std::sinf( yaw_rad );
		const auto cy = std::cosf( yaw_rad );

		const auto forward_move = std::clamp( ( wish_x * cy + wish_y * sy ) * move_magnitude, -1.0f, 1.0f );
		const auto left_move = std::clamp( ( wish_x * sy - wish_y * cy ) * -move_magnitude, -1.0f, 1.0f );

		base->set_forwardmove( forward_move );
		base->set_leftmove( left_move );

		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( subtick_moves )
		{
			const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
			if ( step )
			{
				step->set_button( 0 );
				step->set_pressed( false );
				step->set_when( 0.0f );
				step->set_analog_forward_delta( forward_move - prestate.last_movement_impulses.x );
				step->set_analog_left_delta( left_move - prestate.last_movement_impulses.y );
			}
		}

		if ( forward_move > 0.0f )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_forward;
		}
		else if ( forward_move < 0.0f )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_back;
		}

		// Negative leftmove is A/left — see test_strafer::movement_from_buttons, which is the
		// convention airstrafe's yaw offsets are built against.
		if ( left_move < 0.0f )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_moveleft;
		}
		else if ( left_move > 0.0f )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_moveright;
		}
	}

	float misc::autostop::get_effective_accel_base( std::uintptr_t local_pawn, std::uintptr_t movement_services, std::uint32_t flags, float max_weapon_speed ) const
	{
		const auto max_speed_base = memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );
		const auto is_ducked = ( flags & 4 ) != 0;
		const auto ducking_state = memory::read<bool>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_bDucking"_hash ) );
		const auto is_scoped = g_shared.ctx( ).is_scoped;
		const auto is_ducking = is_ducked || ducking_state;
		const auto v19 = std::fmaxf( 250.0f, max_speed_base );

		auto friction_scale{ 1.0f };

		if (CONVAR ("sv_accelerate_use_weapon_speed")->get<bool>( ) )
		{
			const auto weapon_ratio = std::fminf( 1.0f, max_weapon_speed / 250.0f );

			if ( !is_ducking && !is_scoped )
			{
				friction_scale = weapon_ratio;
			}
		}

		if ( is_ducking )
		{
			friction_scale = std::fminf( 0.34f, friction_scale );
		}

		auto accel_base = v19 * friction_scale;

		if ( is_scoped && !is_ducking )
		{
			accel_base *= 0.52f;
		}

		return accel_base;
	}

	bool misc::jumpscout::has_ssg_08( ) const
	{
		return g_shared.ctx( ).item_def_idx == cstypes::item_definition_index::weapon_ssg_08;
	}

	bool misc::jumpscout::should_jump( const math::vector3& velocity, bool on_ground, float jump_initial, float jump_apex, float min_air_inaccuracy, float threshold_multiplier ) const
	{
		if ( !on_ground )
		{
			return false;
		}

		// Jumping while already braking wastes the hop — the scout needs the airborne
		// window to spend the jump penalty decaying toward apex.
		if ( velocity.z > 0.0f )
		{
			return false;
		}

		// A hop is only worth it if the apex is actually accurate enough to shoot from.
		const auto apex_inaccuracy = min_air_inaccuracy + jump_apex;
		const auto initial_inaccuracy = min_air_inaccuracy + jump_initial;

		if ( initial_inaccuracy <= 0.0f )
		{
			return false;
		}

		return apex_inaccuracy <= initial_inaccuracy * threshold_multiplier;
	}

	bool misc::jumpscout::ready_to_fire( systems::input::usercmd* cmd, std::uintptr_t local_controller, float jump_initial, float jump_apex ) const
	{
		if ( !g_shared.can_shoot( cmd, local_controller ) )
		{
			return false;
		}

		if ( !g_shared.ctx( ).is_scoped )
		{
			return false;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return false;
		}

		// The SSG reaches its airborne accuracy floor near the apex, where vertical speed
		// crosses zero. Fire on the tick the interpolated penalty lands at that floor.
		const auto air_inaccuracy = g_shared.get_air_inaccuracy( prestate.networked_velocity.z, jump_initial, jump_apex );

		return air_inaccuracy <= jump_apex + 0.001f;
	}

	void misc::jumpscout::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;

		auto& cfg = settings::g_combat.m_jumpscout;
		if ( !cfg.enabled.value )
		{
			return;
		}

		const auto& ctx = g_shared.ctx( );
		if ( !ctx.valid || !this->has_ssg_08( ) )
		{
			this->m_hop_pending = false;
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn || !local.controller )
		{
			this->m_hop_pending = false;
			return;
		}

		if ( systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			this->m_hop_pending = false;
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			this->m_hop_pending = false;
			return;
		}

		const auto attacking = ( cmd->buttons.value & cstypes::command_buttons::in_attack ) != 0;
		const auto wants_shot = attacking || this->m_hop_pending;

		if ( cfg.mode.value == settings::combat::jumpscout::scout_mode::on_attack && !wants_shot )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto jump_initial = memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpInitial"_hash ) );
		const auto jump_apex = memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpApex"_hash ) );
		const auto accuracy_penalty = memory::read<float>( ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_fAccuracyPenalty"_hash ) );

		const auto& prestate = systems::g_prediction.pre( );
		const auto on_ground = ( prestate.flags & cstypes::entity_flags::on_ground ) != 0;
		const auto threshold = std::clamp( cfg.threshold.value, 0.05f, 1.0f );

		if ( this->should_jump( prestate.networked_velocity, on_ground, jump_initial, jump_apex, accuracy_penalty, threshold ) )
		{
			// Bunnyhop owns the airborne jump timing; here we only need the initial hop.
			cmd->buttons.value |= cstypes::command_buttons::in_jump;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_jump;

			// Suppress the shot on the launch tick — firing on the ground defeats the hop.
			cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;

			this->m_hop_pending = true;
			this->m_active_this_tick = true;
			return;
		}

		if ( on_ground )
		{
			this->m_hop_pending = false;
			return;
		}

		this->m_active_this_tick = true;

		if ( !this->ready_to_fire( cmd, local.controller, jump_initial, jump_apex ) )
		{
			// Hold the shot until the apex window; releasing keeps the weapon primed.
			cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
			return;
		}

		if ( cfg.mode.value == settings::combat::jumpscout::scout_mode::always || attacking || this->m_hop_pending )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;
		}

		this->m_hop_pending = false;
	}

} // namespace features::combat
