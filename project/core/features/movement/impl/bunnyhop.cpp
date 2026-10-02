#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/diag.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		// The modern jump code (sv_legacy_jump 0) as of the Sep 30 2026 build:
		//  - a press within sv_jump_spam_penalty_time (one tick) of the previous press is ignored, and still
		//    counts as the previous press for the next one (0x8c3740);
		//  - a press within sv_bhop_time_window / 2 (a quarter tick) of touchdown is a bhop: the jump keeps the
		//    landing velocity, so friction costs nothing (0x8e8060);
		//  - any other press in the first tick after touchdown is dropped without a jump.
		// Touchdown is stamped by 0x8de100 as the time the falling arc reaches ground height, capped at the end of
		// the movement slice that put the player on the ground. Every subtick step ends a slice, and the game snaps
		// the player onto ground within 2 units at a slice end -- so with steps in the tick (auto strafe sends
		// sixteen) the stamp is the first slice end after the arc comes within 2 units, up to half a tick before
		// the arc itself reaches the ground. A press just after that point always has a slice end at or before
		// it and inside the window.
		constexpr auto k_jump_penalty_ticks{ 1.0 };
		constexpr auto k_press_margin_ticks{ 2.0 / 64.0 };
		constexpr auto k_snap_distance{ 2.0f };

		struct touchdown
		{
			float snap{};    // ticks until the arc is within snap distance of the ground
			float contact{}; // ticks until the arc reaches the ground
		};

		/// Ticks from the start of this command until the player's arc reaches the ground, looking up to two ticks
		/// ahead. Same arc the game solves: z0 + vz * t - gravity * t^2 / 2 = height.
		[[nodiscard]] std::optional<touchdown> predict_touchdown(
			std::uintptr_t local_pawn,
			std::uintptr_t movement_services,
			const systems::prediction::state& prestate,
			bool holding_duck )
		{
			if ( prestate.networked_velocity.z > 0.0f )
			{
				return std::nullopt;
			}

			const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
			const auto mins = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
			auto maxs = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

			auto trace_origin = prestate.networked_origin;
			if ( holding_duck && duck_amount > 0.0f )
			{
				const auto standing_height{ 72.0f };
				const auto duck_hull_diff = standing_height - maxs.z;
				trace_origin.z -= duck_hull_diff * 0.5f;
				maxs.z = standing_height;
			}

			auto trace_mask{ 0ull };
			{
				const auto pawn_ptr = memory::read<std::uintptr_t>( movement_services + 56 );
				trace_mask = memory::read<std::uintptr_t>( pawn_ptr + 0xd50 );

				if ( !pawn_ptr || ( memory::read<std::uint32_t>( pawn_ptr + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) ) & 0x10 ) )
				{
					trace_mask |= 0x20;
				}
			}

			const auto filter = systems::g_tracing.make_player_movement_filter( local_pawn, trace_mask, 11 );
			const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
			const auto sv_standable_normal = CONVAR ("sv_standable_normal")->get<float>( );
			const auto gravity_scale = memory::read<float>( local_pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );
			const auto gravity = gravity_scale * sv_gravity;

			if ( gravity <= 0.0f )
			{
				return std::nullopt;
			}

			// Two ticks along the arc, and 2 units further down: the game snaps a player within 2 units of the
			// ground onto it, and the stamp then still uses the arc's own time to reach the ground.
			const auto lookahead = cstypes::tick_interval * 2.0f;
			const auto& velocity = prestate.networked_velocity;

			const math::vector3 trace_start = trace_origin;
			math::vector3 trace_end{};

			trace_end.x = trace_origin.x + velocity.x * lookahead;
			trace_end.y = trace_origin.y + velocity.y * lookahead;
			trace_end.z = trace_origin.z + velocity.z * lookahead - 0.5f * gravity * lookahead * lookahead;
			trace_end.z -= k_snap_distance;

			const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, { mins, maxs }, filter, movement_services );
			if ( result.fraction <= 0.0f || result.fraction >= 1.0f || result.normal.z < sv_standable_normal )
			{
				return std::nullopt;
			}

			const auto ground_z = trace_start.z + ( trace_end.z - trace_start.z ) * result.fraction;

			// 0.5 * g * t^2 - vz * t - drop = 0, positive root.
			const auto ticks_to_drop = [ & ]( float drop )
			{
				drop = std::fmaxf( drop, 0.0f );
				const auto discriminant = velocity.z * velocity.z + 2.0f * gravity * drop;
				return ( velocity.z + std::sqrtf( discriminant ) ) / gravity / cstypes::tick_interval;
			};

			return touchdown{
				.snap = ticks_to_drop( trace_start.z - ground_z - k_snap_distance ),
				.contact = ticks_to_drop( trace_start.z - ground_z ),
			};
		}

		void apply_landing_jump( proto::base_usercmd_pb* base, float when )
		{
			const auto subtick_moves = base->mutable_subtick_moves( );
			const auto release_when = std::clamp( when - 1.0f / 64.0f, 1.0f / 64.0f, 63.0f / 64.0f );

			// Both steps describe a keypress and nothing else. They deliberately do not stamp analog
			// deltas: set_analog_*_delta marks the has-bits whatever value it is given, so a zero pair
			// made has_move_subticks report a wish direction this step never held -- which had the scrub
			// in create_move zero the scalar forwardmove/leftmove and input::apply skip the fallback that
			// would have carried them. A zero delta does not mean "no movement" to the server, it means
			// "unchanged since the last command", so every jump tick re-used the previous wish direction.
			if ( release_when < when )
			{
				if ( const auto jump_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					jump_up->set_button( cstypes::command_buttons::in_jump );
					jump_up->set_pressed( false );
					jump_up->set_when( release_when );
				}
			}

			if ( const auto jump_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				jump_down->set_button( cstypes::command_buttons::in_jump );
				jump_down->set_pressed( true );
				jump_down->set_when( when );
			}
		}

		[[nodiscard]] float round_to_step( float when )
		{
			return std::clamp( std::round( when * 64.0f ) / 64.0f, 1.0f / 64.0f, 63.0f / 64.0f );
		}

	} // namespace

	void bhop::reset( )
	{
		this->m_cycle = false;
		this->m_ground_tick = -1;
		this->m_pending_tick = -1;
	}

	bool bhop::press( systems::input::usercmd* cmd, int tick, float when )
	{
		// A press inside the spam penalty would be thrown away and would also restart the penalty, so it is
		// never sent; the caller tries again on a later tick.
		if ( static_cast< double >( tick ) + when - this->m_last_press <= k_jump_penalty_ticks )
		{
			return false;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return false;
		}

		apply_landing_jump( base, when );
		this->m_last_press = static_cast< double >( tick ) + when;
		return true;
	}

	void bhop::on_create_move( systems::input::usercmd* cmd )
	{
		// One call per command, so this counts ticks in the command stream the presses are timed against.
		const auto tick = ++this->m_command;

		if ( !settings::g_movement.bhop.value )
		{
			this->reset( );
			return;
		}

		g_diag.bhop_calls.fetch_add( 1, std::memory_order_relaxed );

		if (CONVAR ("sv_autobunnyhopping")->get<bool>( ) )
		{
			g_diag.bhop_autobhop_convar.fetch_add( 1, std::memory_order_relaxed );
			this->reset( );
			return;
		}

		if ( !( cmd->buttons.value & cstypes::command_buttons::in_jump ) )
		{
			g_diag.bhop_no_jump_key.fetch_add( 1, std::memory_order_relaxed );
			this->reset( );
			return;
		}

		if ( features::movement::g_jumpbug.active_this_tick( ) )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			this->reset( );
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );

		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			g_diag.bhop_on_ground.fetch_add( 1, std::memory_order_relaxed );

			// A press made on the ground is the game's own jump. Only a jump held through the air is ours.
			if ( !this->m_cycle )
			{
				return;
			}

			// Holding the key through the landing would read as a press at the start of this tick: one tick
			// or less after the press made for the landing, so the game would discard it as spam.
			cmd->buttons.value &= ~cstypes::command_buttons::in_jump;

			if ( this->m_ground_tick < 0 )
			{
				this->m_ground_tick = tick;
			}

			// The last airborne tick ended inside snap distance, so its end is the touchdown: press right after it.
			if ( this->m_pending_tick == tick )
			{
				this->m_pending_tick = -1;

				if ( this->press( cmd, tick, round_to_step( this->m_pending_when ) ) )
				{
					this->m_hop.when = round_to_step( this->m_pending_when );
					this->m_hop.logged = false;
					g_diag.bhop_scheduled.fetch_add( 1, std::memory_order_relaxed );
					return;
				}
			}

			// Still on the ground a tick after landing: the bhop press missed. Jump as soon as the game will take
			// a press -- a full tick after the last one, and clear of the tick after touchdown.
			if ( tick <= this->m_ground_tick )
			{
				return;
			}

			auto when = static_cast< float >( this->m_last_press + k_jump_penalty_ticks + k_press_margin_ticks - static_cast< double >( tick ) );
			if ( tick == this->m_ground_tick + 1 )
			{
				when = std::fmaxf( when, 0.5f );
			}

			when = std::fmaxf( when, 1.0f / 64.0f );
			if ( when > 63.0f / 64.0f )
			{
				return;
			}

			if ( this->press( cmd, tick, round_to_step( when ) ) )
			{
				this->m_hop.path = "retry";
				this->m_hop.when = round_to_step( when );
				this->m_hop.speed = prestate.networked_velocity.length_2d( );
				this->m_hop.logged = false;
				g_diag.bhop_retry.fetch_add( 1, std::memory_order_relaxed );
			}

			return;
		}

		g_diag.bhop_air_jump_held.fetch_add( 1, std::memory_order_relaxed );
		cmd->buttons.value &= ~cstypes::command_buttons::in_jump;

		this->m_cycle = true;
		this->m_ground_tick = -1;
		this->m_pending_tick = -1;

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto speed = prestate.networked_velocity.length_2d( );

		// First airborne tick after a jump: report how far from the game's own touchdown stamp the press landed,
		// and what the jump did to the speed. Inside the window the landing speed is kept.
		if ( this->m_hop.logged == false && this->m_hop.path )
		{
			const auto modern = movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_ModernJump"_hash );
			const auto press_time = memory::read<int>( modern + SCHEMA( "CCSPlayerModernJump", "m_nLastActualJumpPressTick"_hash ) )
				+ static_cast< double >( memory::read<float>( modern + SCHEMA( "CCSPlayerModernJump", "m_flLastActualJumpPressFrac"_hash ) ) );
			const auto landed_time = memory::read<int>( modern + SCHEMA( "CCSPlayerModernJump", "m_nLastLandedTick"_hash ) )
				+ static_cast< double >( memory::read<float>( modern + SCHEMA( "CCSPlayerModernJump", "m_flLastLandedFrac"_hash ) ) );

			diag::writef(
				diag::level::debug,
				"bhop hop: path=%s when=%.3f snap=%.3f contact=%.3f | game press-landed=%+.3f ticks (bhop if within +-0.25) | speed %.1f -> %.1f",
				this->m_hop.path, this->m_hop.when, this->m_hop.snap, this->m_hop.contact,
				press_time - landed_time, this->m_hop.speed, speed );

			this->m_hop.logged = true;
		}

		const auto holding_duck = ( cmd->buttons.value & cstypes::command_buttons::in_duck ) != 0;
		const auto touchdown = predict_touchdown( local.pawn, movement_services, prestate, holding_duck );
		if ( !touchdown || touchdown->snap >= 1.0f )
		{
			g_diag.bhop_no_landing.fetch_add( 1, std::memory_order_relaxed );
			return;
		}

		this->m_hop = { .path = "air", .snap = touchdown->snap, .contact = touchdown->contact, .speed = speed };

		// Just past the point the arc comes within snap distance, so a slice end sits between it and the press.
		const auto when = std::fmaxf( std::ceil( ( touchdown->snap + 1.0f / 128.0f ) * 64.0f ) / 64.0f, 1.0f / 64.0f );
		if ( when > 63.0f / 64.0f )
		{
			// The tick ends first, and its end is where the player is put on the ground. Press right after it.
			this->m_hop.path = "next-tick";
			this->m_pending_tick = tick + 1;
			this->m_pending_when = 1.0f / 64.0f;
			return;
		}

		if ( this->press( cmd, tick, when ) )
		{
			this->m_hop.when = when;
			this->m_hop.logged = false;
			g_diag.bhop_scheduled.fetch_add( 1, std::memory_order_relaxed );
		}
	}

} // namespace features::movement
