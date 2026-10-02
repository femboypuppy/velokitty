#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/threadpool/threadpool.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
namespace features::combat {

	void rage::on_create_move( systems::input::usercmd* cmd )
	{
		struct report_on_exit
		{
			rage* self;
			~report_on_exit( ) { self->report_decision_diag( ); }
		} const report{ this };

		auto& ctx = g_shared.ctx( );
		const auto local = systems::g_local.get( );
		this->update_penetration_crosshair( local );

		// Reset before the context guard, not after. These are read by other features
		// (autostop brakes on m_should_stop / has_target), so leaving them set when we bail
		// on an invalid context kept the brake engaged through a holster or a death.
		this->m_should_stop = false;
		this->m_firing_this_tick = false;
		this->m_target = {};

		if ( !ctx.valid )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		// Age the resolver's pending shots and rotate hypotheses on confirmed misses. Runs once
		// per command, before any scanning reads hypothesis(). Cheap no-op when nothing is pending.
		g_shared.res( ).on_tick( );

		if ( !settings::g_combat.m_duckpeek.enabled.value )
		{
			this->m_release_duck_for_shot = false;
			this->m_duckpeek_reduck = false;
		}

		if ( this->m_zeus_fired )
		{
			this->m_zeus_fired = false;

			if ( settings::g_combat.m_zeusbot.drop_after && !systems::g_local.is_in_deathmatch( ) )
			{
				memory::call<void>(PATTERN (patterns::engine_client_cmd), addresses::globals::source2engine_to_client, 0, "drop", 0x7ffef001, std::numeric_limits<double>::quiet_NaN( ), 0ull );
			}

			return;
		}

		const auto is_knife = ctx.weapon_type == cstypes::weapon_type::knife;
		const auto is_taser = ctx.weapon_type == cstypes::weapon_type::taser;

		if ( !is_knife && !is_taser && ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg ) )
		{
			return;
		}

		auto aim_ctx = this->build_context( cmd, local );

		if ( is_knife )
		{
			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				return;
			}

			this->run_knife( cmd, aim_ctx, local );
		}
		else if ( is_taser )
		{
			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				return;
			}

			this->run_taser( cmd, aim_ctx, local );
		}
		else if ( ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver )
		{
			this->auto_revolver( cmd, aim_ctx, local );
		}
		else
		{
			this->m_revolver_cock_ticks = 0;

			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				++this->m_diag.cant_shoot;
				return;
			}

			this->run_gun( cmd, aim_ctx, local );
		}
	}

	void rage::on_render( xdraw::draw_list& draw_list )
	{
		this->draw_penetration_crosshair( draw_list );

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( !config.debug_multipoints.value )
		{
			return;
		}

		std::lock_guard lock( m_debug_mtx );

		for ( const auto& pt : m_debug_points )
		{
			const auto screen = systems::g_view.project( pt.position );
			if ( !systems::g_view.projection_valid( screen ) )
			{
				continue;
			}

			xdraw::color col{};
			switch ( pt.hitbox_index )
			{
			case 0:
				col = { 255, 80,  80  }; break; // head — red
			case 2: case 3:
				col = { 220, 220, 60  }; break; // stomach — yellow
			case 4: case 5: case 6:
				col = { 255, 160, 60  }; break; // chest — orange
			case 7: case 8: case 9: case 10: case 11: case 12:
				col = { 80,  160, 255 }; break; // legs — blue
			case 13: case 14: case 15: case 16: case 17: case 18:
				col = { 180, 80,  255 }; break; // arms — purple
			default:
				col = { 200, 200, 200 }; break;
			}

			const auto alpha  = pt.is_center ? std::uint8_t{ 255 } : std::uint8_t{ 160 };
			const auto radius = pt.is_center ? 3.5f : 2.0f;

			draw_list.circle_filled( screen.x, screen.y, radius, col.alpha( alpha ) );
		}
	}

	rage::aim_context rage::build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const
	{
		auto& ctx = g_shared.ctx( );
		const auto& prestate = systems::g_prediction.pre( );

		aim_context out{};
		out.velocity = prestate.velocity;
		out.spread = g_shared.get_spread( );
		out.predicted_inaccuracy = g_shared.get_inaccuracy( true );

		systems::g_prediction.simulate( cmd, local, [ & ]
			{
				g_shared.sh( ).snapshot( local.pawn, ctx.weapon_services );

				out.velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
				out.spread = g_shared.get_spread( );
				out.predicted_inaccuracy = g_shared.get_inaccuracy( true );
			} );

		ctx.spread = out.spread;
		ctx.inaccuracy = out.predicted_inaccuracy;
		ctx.inaccuracy_velocity_z = out.velocity.z;

		out.view_angles = systems::g_input.get_view_angles( );
		out.on_ground = ( prestate.flags & cstypes::entity_flags::on_ground ) != 0;
		out.is_scoped = ctx.is_scoped;
		out.weapon_max_speed = ctx.weapon_max_speed;
		out.accurate_threshold = ctx.weapon_max_speed * 0.34f;

		return out;
	}

	bool rage::can_subtick_shot( const systems::input::usercmd* cmd ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		if ( !config.subtick_shot.value )
		{
			return false;
		}

		const auto base_cmd = cmd->csgo_user_cmd.base( );
		if ( !base_cmd )
		{
			return false;
		}

		const auto client_tick = base_cmd->client_tick( );
		const auto next_primary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );

		// m_nNextPrimaryAttackTick is the game's own authority on when this gun may fire again.
		// The extra tick_base >= last_shoot_tick + 2 floor that used to sit here was our own
		// invention, stricter than the engine, and it swallowed the pulse on the first tick the
		// weapon was actually ready.
		return client_tick >= next_primary && !memory::read<bool>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) ) && memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) ) > 0;
	}

	bool rage::emit_subtick_shot( systems::input::usercmd* cmd )
	{
		const auto base_cmd = cmd->csgo_user_cmd.mutable_base( );
		if ( !base_cmd )
		{
			return false;
		}

		const auto subtick_moves = base_cmd->mutable_subtick_moves( );
		if ( !subtick_moves )
		{
			return false;
		}

		const auto old_size = subtick_moves->m_current_size;
		const auto press = systems::g_input.acquire_subtick_step( subtick_moves );
		const auto release = systems::g_input.acquire_subtick_step( subtick_moves );

		if ( !press || !release )
		{
			// Do not leave a partial press in the command if allocation of
			// the matching release failed.
			subtick_moves->m_current_size = old_size;
			return false;
		}

		press->set_button( cstypes::command_buttons::in_attack );
		press->set_pressed( true );
		press->set_when( 0.0f );

		release->set_button( cstypes::command_buttons::in_attack );
		release->set_pressed( false );
		// Half way through the tick window. The old value -- the last float below
		// 1.0f -- put the release on the boundary, where it is at the mercy of how
		// the server rounds the fraction back into a sub-tick offset.
		release->set_when( 0.5f );

		// The subtick release is the final state. Do not let the
		// base command turn this pulse into a held attack.
		//
		// value_changed is deliberately untouched: the press and release steps above
		// already carry the edges, and repeating them in the legacy edge bitfield
		// reports the same shot twice.
		cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
		cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
		// The shot's input history entry is the one fire_gun fills with the aim angles, the no-spread roll and
		// the stamped player/render ticks. -1 here told the server the attack started in none of them: it either
		// dropped the shot or fired it from the base command angles, which carry no no-spread roll -- the
		// "doesn't shoot" and "shoots into the air" the feature was known for.
		const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		cmd->csgo_user_cmd.set_attack1_start_history_index( history_index >= 0 ? history_index : -1 );

		return true;
	}

	std::optional<rage::stop_prediction> rage::predict_stop( const aim_context& ctx, const math::vector3& current_eye, const systems::local::snapshot& local ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto& prestate = systems::g_prediction.pre( );
		const auto speed = prestate.networked_velocity.length_2d( );
		const auto will_stop = ctx.on_ground && ( speed > ctx.accurate_threshold || ( ctx.is_scoped && speed > 1.0f ) );

		if ( !will_stop )
		{
			return std::nullopt;
		}

		auto sim_vel = prestate.networked_velocity;
		sim_vel.z = 0.0f;

		const auto sv_friction = CONVAR("sv_friction")->get<float>( );
		const auto sv_stopspeed = CONVAR("sv_stopspeed")->get<float>( );
		const auto sv_accelerate = CONVAR("sv_accelerate")->get<float>( );
		const auto surface_friction = prestate.surface_friction;

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		const auto max_move_speed = movement_services ? memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) ) : 250.0f;

		for ( auto i = 0; i < 15; ++i )
		{
			const auto sim_speed = sim_vel.length_2d( );
			if ( sim_speed < 1.0f )
			{
				break;
			}

			const auto control = std::fmaxf( sim_speed, sv_stopspeed );
			const auto drop = sv_friction * surface_friction * control * cstypes::tick_interval;
			auto new_speed = std::fmaxf( sim_speed - drop, 0.0f );
			auto accel = sv_accelerate;

			if ( shared_ctx.is_scoped )
			{
				const auto weapon_ratio = std::fminf( 1.0f, shared_ctx.weapon_max_speed / 250.0f );
				const auto scoped_max = std::fmaxf( 250.0f, max_move_speed ) * weapon_ratio * 0.52f;

				if ( new_speed > scoped_max - 5.0f )
				{
					const auto t = 1.0f - std::fmaxf( 0.0f, new_speed - ( scoped_max - 5.0f ) ) / std::fmaxf( 0.01f, 5.0f );
					accel *= std::clamp( t, 0.0f, 1.0f );
				}
			}

			const auto accel_speed = std::fminf( accel * shared_ctx.weapon_max_speed * surface_friction * cstypes::tick_interval, new_speed );
			new_speed = std::fmaxf( new_speed - accel_speed, 0.0f );

			if ( new_speed > 0.0f )
			{
				sim_vel *= ( new_speed / sim_speed );
			}
			else
			{
				sim_vel = {};
				break;
			}
		}

		const auto avg_vel = ( prestate.networked_velocity + sim_vel ) * 0.5f;
		const auto stop_ticks = g_shared.calculate_stop_ticks( prestate.networked_velocity, shared_ctx.weapon_max_speed, local.pawn );
		const auto stop_time = static_cast< float >( stop_ticks ) * cstypes::tick_interval;

		return stop_prediction
		{
			.eye =
			{
				current_eye.x + avg_vel.x * stop_time,
				current_eye.y + avg_vel.y * stop_time,
				current_eye.z
			},
			.inaccuracy = g_shared.get_inaccuracy_at_velocity( local.pawn, sim_vel )
		};
	}

	std::vector<rage::candidate> rage::gather_candidates( const systems::local::snapshot& local, float max_distance_sq ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );

		std::vector<candidate> out;
		out.reserve( players.size( ) );

		const_cast<rage*>( this )->m_extrapolated_records.clear( );
		const_cast<rage*>( this )->m_extrapolated_records.reserve( players.size( ) );

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

			if ( memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
			{
				continue;
			}

			auto records = g_shared.lc( ).get_valid_records( pawn );

			this->m_scan_diag.candidates.fetch_add( 1, std::memory_order_relaxed );

			if ( records.empty( ) )
			{
				this->m_scan_diag.extrapolated.fetch_add( 1, std::memory_order_relaxed );
				auto extrap = g_shared.lc( ).extrapolate( pawn );
				if ( !extrap.has_value( ) )
				{
					continue;
				}

				const_cast<rage*>( this )->m_extrapolated_records.push_back( std::move( *extrap ) );
				records.push_back( &const_cast<rage*>( this )->m_extrapolated_records.back( ) );
			}

			if ( max_distance_sq > 0.0f )
			{
				const auto& origin = systems::g_prediction.pre( ).origin;
				const auto delta_front = records.front( )->origin - origin;
				auto closest_sq = delta_front.x * delta_front.x + delta_front.y * delta_front.y + delta_front.z * delta_front.z;

				if ( records.size( ) > 1 )
				{
					const auto delta_back = records.back( )->origin - origin;
					const auto back_sq = delta_back.x * delta_back.x + delta_back.y * delta_back.y + delta_back.z * delta_back.z;
					closest_sq = std::min( closest_sq, back_sq );
				}

				if ( closest_sq > max_distance_sq )
				{
					continue;
				}
			}

			candidate c{};
			c.pawn = pawn;
			c.health = health;
			c.armor = memory::read<int>( pawn + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) );

			// How many records to actually scan for this candidate. get_group falls back to the
			// rifle group for an unknown weapon type, so this is always a valid read; clamp keeps
			// it inside the index buffer even if a config on disk carries a larger value.
			const auto& backtrack_config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
			const auto max_scan = std::clamp( backtrack_config.backtrack_records.value, 1, k_max_scan_records );

			const auto pick_record_indices = [ &records, max_scan ]( std::array<int, k_max_scan_records>& out_indices ) -> int
				{
					const auto count = static_cast< int >( records.size( ) );
					if ( count == 0 )
					{
						return 0;
					}

					auto picked{ 0 };
					const auto add_index = [ & ]( int idx )
						{
							if ( picked >= max_scan )
							{
								return;
							}

							for ( auto i = 0; i < picked; ++i )
							{
								if ( out_indices[ i ] == idx )
								{
									return;
								}
							}

							out_indices[ picked++ ] = idx;
						};

					// Fewer records than we're allowed to scan: take them all, newest-first.
					if ( count <= max_scan )
					{
						for ( auto i = 0; i < count; ++i )
						{
							add_index( i );
						}

						return picked;
					}

					// Spread the picks evenly over [0, count-1] so the endpoints (newest + oldest)
					// are always in and the middle samples land at an even stride. round() on the
					// interpolation keeps the samples from clustering at the low end; add_index
					// dedupes if two rounded slots collide.
					for ( auto i = 0; i < max_scan; ++i )
					{
						const auto t = static_cast< float >( i ) / static_cast< float >( max_scan - 1 );
						const auto idx = static_cast< int >( std::lround( t * static_cast< float >( count - 1 ) ) );
						add_index( idx );
					}

					return picked;
				};

			std::array<int, k_max_scan_records> record_indices{};
			const auto picked_count = pick_record_indices( record_indices );

			for ( auto i = 0; i < picked_count; ++i )
			{
				c.records[ i ] = records[ static_cast< std::size_t >( record_indices[ i ] ) ];
			}

			c.record_count = picked_count;

			if ( shared_ctx.weapon_type >= cstypes::weapon_type::pistol && shared_ctx.weapon_type <= cstypes::weapon_type::lmg )
			{
				const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
				c.min_damage = this->get_min_damage( config, health, config.min_damage_override.value );
			}

			out.push_back( c );
		}

		return out;
	}

	void rage::run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire )
	{
		if ( !settings::g_combat.m_ragebot.enabled )
		{
			return;
		}

		auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );

		// Asks only whether the shot *could* go out as a subtick pulse. Nothing is written to the
		// command here -- fire_gun emits the pulse itself, once it is past every one of its own
		// early returns. The no-target paths below used to call this with want_shot = false, which
		// is now simply nothing to do.
		const auto subtick_shot_possible = [ & ]
		{
			if ( config.subtick_shot.value && shared_ctx.item_def_idx != cstypes::item_definition_index::weapon_r8_revolver )
			{
				return this->can_subtick_shot( cmd );
			}

			return false;
		};

		auto candidates = this->gather_candidates( local );
		++this->m_diag.frames;

		{
			std::lock_guard lock( m_debug_mtx );
			m_debug_points.clear( );
		}

		if ( candidates.empty( ) )
		{
			++this->m_diag.no_candidates;
			this->m_first_target_tick = -1;
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		if ( eye_candidates.count == 0 )
		{
			eye_candidates.entries[ 0 ].position = g_shared.get_shoot_position( );
			eye_candidates.entries[ 0 ].is_uninterpolated = true;
			eye_candidates.count = 1;
		}

		const auto scan_from_eye_candidates = [ & ]( const math::vector3& eye_offset, float inaccuracy )
		{
			std::vector<scan_hit> hits_out;

			for ( auto i = 0; i < eye_candidates.count; ++i )
			{
				const auto eye = eye_candidates.entries[ i ].position + eye_offset;
				auto hits = this->scan_players( eye, inaccuracy, ctx, candidates, local );
				auto found_direct{ false };

				for ( auto& hit : hits )
				{
					auto source_eye = eye_candidates.entries[ i ];
					source_eye.position = eye;
					hit.source_eye = source_eye;
					found_direct = found_direct || !hit.penetrated;
					hits_out.push_back( std::move( hit ) );
				}

				if ( found_direct )
				{
					break;
				}
			}

			return hits_out;
		};

		if ( config.no_spread.value )
		{
			// shared_ctx.inaccuracy already holds the inaccuracy predicted for the tick this shot goes out on
			// (build_context stored it after simulating the command). The spread solve has to use that value:
			// re-reading the weapon here gave the state from before this tick's movement, which is a different
			// number on every jump and landing tick, and a solve built on the wrong inaccuracy hands the
			// server a completely different spread offset.
			auto all_hits = scan_from_eye_candidates( {}, shared_ctx.inaccuracy );

			if ( all_hits.empty( ) )
			{
				++this->m_diag.no_hit;
				return;
			}

			const auto best = this->select_best( ctx, all_hits, shared_ctx.inaccuracy );
			this->m_target = best;

			if ( !best.valid )
			{
				++this->m_diag.no_hit;
				return;
			}

			++this->m_diag.targets;

			// no_spread does not care about accuracy, but autostop is gated on this flag and
			// the player still expects the brake to engage on a target. Without it this path
			// silently disabled autostop for anyone running no_spread.
			this->m_should_stop = this->should_stop_movement( ctx );

			if ( !allow_fire )
			{
				return;
			}

			const auto subtick_attack = subtick_shot_possible( );
			this->fire_gun( cmd, best, false, best.hit.source_eye.position, local, subtick_attack );

			if ( this->m_firing_this_tick )
			{
				++this->m_diag.fired;
			}

			return;
		}

		const auto primary_eye = eye_candidates.entries[ 0 ].position;
		const auto& prestate = systems::g_prediction.pre( );

		// Current-shot selection is always based on current engine shoot-history.
		auto current_hits = scan_from_eye_candidates( {}, ctx.predicted_inaccuracy );
		const auto best = this->select_best( ctx, current_hits, ctx.predicted_inaccuracy );
		this->m_target = best;

		const auto needed_hc = config.hitchance_override.value ? static_cast< float >( config.hitchance_override_value ) / 100.0f : static_cast< float >( config.hitchance ) / 100.0f;
		const auto duckpeek_active = settings::g_combat.m_duckpeek.enabled.value && ctx.on_ground;
		const auto is_ducked = ( prestate.flags & cstypes::entity_flags::ducking ) != 0;

		const auto standing_inaccuracy = duckpeek_active ? this->get_standing_inaccuracy( local, ctx ) : ctx.predicted_inaccuracy;
		const auto standing_hc = best.valid
			? ( duckpeek_active ? this->evaluate_hitchance( best.hit, ctx, standing_inaccuracy ) : best.hitchance )
			: 0.0f;

		// Jump scout: in the air the shot waits for the top of the jump, where the scout is most accurate, while
		// auto stop takes the horizontal speed out. With no spread there is nothing to wait for.
		const auto apex_hold = g_misc.jumpscout( ).active_this_tick( ) && !g_misc.jumpscout( ).apex_ready( );
		const auto accurate = best.valid && standing_hc >= needed_hc && !apex_hold;

		// force-shot used to also demand is_max_accuracy, which only reports true once the
		// weapon's inaccuracy has decayed all the way to its floor. Mid-spray, in the air, or
		// on the first tick of a peek that never happens, so holding the bind did nothing in
		// exactly the moments you press it. The bind now bypasses the configured hitchance and
		// answers to a low floor instead: early shots like neverlose, without degenerating
		// into a blind trigger that fires at wall corners.
		// A fraction, not a percentage: calculate_hitchance returns hits / samples.
		constexpr auto k_force_min_hitchance{ 0.15f };
		const auto force_bind = ctx.on_ground ? config.force_shot.value : config.force_shot_air.value;
		const auto force = best.valid && force_bind && standing_hc >= k_force_min_hitchance;
		const auto shot_viable = accurate || force;

		if ( best.valid )
		{
			++this->m_diag.targets;
			this->m_diag.hc_sum += standing_hc;

			if ( this->m_first_target_tick < 0 )
			{
				this->m_first_target_tick = g_shared.ctx( ).current_tick;
			}

			if ( !shot_viable )
			{
				++this->m_diag.held_hitchance;
				this->m_diag.hc_short_sum += needed_hc - standing_hc;
			}
		}
		else
		{
			++this->m_diag.no_hit;
			this->m_first_target_tick = -1;
		}

		// Autostop planning is independent from firing. Ground movement can use a
		// predicted stopped eye; airborne stopping keeps the current target context.
		if ( !shot_viable && this->should_stop_movement( ctx ) )
		{
			const auto stop = this->predict_stop( ctx, primary_eye, local );
			if ( stop )
			{
				const auto future_offset = stop->eye - primary_eye;
				auto planned_hits = scan_from_eye_candidates( future_offset, stop->inaccuracy );
				const auto planned = this->select_best( ctx, planned_hits, stop->inaccuracy );
				this->m_should_stop = planned.valid;
			}
			else
			{
				this->m_should_stop = best.valid;
			}
		}

		if ( this->m_should_stop )
		{
			++this->m_diag.stop_frames;
		}

		if ( !best.valid )
		{
			return;
		}

		if ( duckpeek_active && allow_fire )
		{
			if ( shot_viable )
			{
				this->m_release_duck_for_shot = true;
			}
			else if ( !this->m_duckpeek_reduck )
			{
				this->m_release_duck_for_shot = false;
			}
		}

		auto ready_to_fire = shot_viable;
		if ( duckpeek_active )
		{
			if ( is_ducked )
			{
				ready_to_fire = false;
			}
			else
			{
				ready_to_fire = ready_to_fire && this->m_release_duck_for_shot;
			}
		}

		if ( shot_viable && !ready_to_fire )
		{
			++this->m_diag.held_duck;
		}

		if ( ready_to_fire && allow_fire )
		{
			const auto subtick_attack = subtick_shot_possible( );
			this->fire_gun( cmd, best, !accurate && force, best.hit.source_eye.position, local, subtick_attack );

			if ( this->m_firing_this_tick )
			{
				const auto waited = std::max( 0, g_shared.ctx( ).current_tick - this->m_first_target_tick );
				++this->m_diag.fired;
				++this->m_diag.latency_n;
				this->m_diag.latency_sum += waited;
				this->m_diag.latency_max = std::max( this->m_diag.latency_max, waited );
				this->m_first_target_tick = -1;
			}

			if ( duckpeek_active )
			{
				this->m_duckpeek_reduck = true;
				this->m_release_duck_for_shot = false;
			}
		}
	}

	void rage::run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_zeusbot.enabled )
		{
			return;
		}

		auto candidates = this->gather_candidates( local );
		if ( candidates.empty( ) )
		{
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		if ( eye_candidates.count == 0 )
		{
			eye_candidates.entries[ 0 ].position = g_shared.get_shoot_position( );
			eye_candidates.entries[ 0 ].is_uninterpolated = true;
			eye_candidates.count = 1;
		}

		std::vector<scan_hit> all_hits;

		for ( auto i = 0; i < eye_candidates.count; ++i )
		{
			auto hits = this->scan_taser( eye_candidates.entries[ i ].position, ctx, candidates, local );

			for ( auto& h : hits )
			{
				h.source_eye = eye_candidates.entries[ i ];
				all_hits.push_back( std::move( h ) );
			}
		}

		if ( all_hits.empty( ) )
		{
			return;
		}

		target best{};

		for ( const auto& h : all_hits )
		{
			if ( !best.valid || h.score > best.score )
			{
				best.hit = h;
				best.hitchance = 1.0f;
				best.score = h.score;
				best.valid = true;
			}
		}

		this->m_target = best;

		if ( best.valid )
		{
			this->m_should_stop = this->should_stop_movement( ctx );
			this->m_zeus_fired = true;
			this->fire_melee( cmd, best, local );
		}
	}

	void rage::run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_knifebot.enabled )
		{
			return;
		}

		const auto info = this->get_knife_info( local );
		if ( !info.can_slash && !info.can_stab )
		{
			return;
		}

		constexpr auto max_knife_dist_sq = 150.0f * 150.0f;
		auto candidates = this->gather_candidates( local, max_knife_dist_sq );
		if ( candidates.empty( ) )
		{
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		if ( eye_candidates.count == 0 )
		{
			eye_candidates.entries[ 0 ].position = g_shared.get_shoot_position( );
			eye_candidates.entries[ 0 ].is_uninterpolated = true;
			eye_candidates.count = 1;
		}

		std::vector<scan_hit> all_hits;

		for ( auto i = 0; i < eye_candidates.count; ++i )
		{
			auto hits = this->scan_knife( eye_candidates.entries[ i ].position, ctx, info, candidates, local );

			for ( auto& h : hits )
			{
				h.source_eye = eye_candidates.entries[ i ];
				all_hits.push_back( std::move( h ) );
			}
		}

		if ( all_hits.empty( ) )
		{
			return;
		}

		target best{};
		target best_backstab{};

		for ( const auto& h : all_hits )
		{
			auto& dest = h.is_backstab ? best_backstab : best;

			if ( !dest.valid || h.score > dest.score )
			{
				dest.hit = h;
				dest.hitchance = 1.0f;
				dest.score = h.score;
				dest.valid = true;
			}
		}

		auto& chosen = best_backstab.valid ? best_backstab : best;
		this->m_target = chosen;

		if ( !chosen.valid )
		{
			return;
		}

		this->m_should_stop = this->should_stop_movement( ctx );
		this->m_knife_attack = static_cast< std::uint8_t >( chosen.hit.attack_type );
		this->fire_melee( cmd, chosen, local );
	}

	void rage::auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_ragebot.enabled )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		if ( !g_shared.can_shoot( cmd, local.controller ) )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		if ( !settings::g_combat.m_autos.revolver.value )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		constexpr auto cock_ticks{ 13 };
		if ( this->m_revolver_cock_ticks >= cock_ticks )
		{
			// End the held cycle. Target selection adds attack back on this
			// command only when the revolver should actually fire.
			cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
			cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			this->m_revolver_cock_ticks = 0;

			this->run_gun( cmd, ctx, local );
			return;
		}

		// Keep target and hitchance planning active throughout the cock cycle.
		// Autostop consumes this command's decision on the following command.
		this->run_gun( cmd, ctx, local, false );

		cmd->buttons.value |= cstypes::command_buttons::in_attack;
		cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
		cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;

		const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		if ( history_index >= 0 )
		{
			cmd->csgo_user_cmd.set_attack1_start_history_index( history_index );
		}

		++this->m_revolver_cock_ticks;
	}

	std::vector<rage::scan_hit> rage::scan_players( const math::vector3& eye, float inaccuracy, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		std::vector<std::vector<scan_hit>> per_candidate( candidates.size( ) );

		const auto& resolve_cfg = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto resolver_on = resolve_cfg.resolver.value;
		const auto desync_range = static_cast< float >( resolve_cfg.desync_range.value );

		threadpool::parallel_for( 0, static_cast< int >( candidates.size( ) ), [ & ]( int begin, int end )
			{
				for ( auto ci = begin; ci < end; ++ci )
				{
					auto& cand = candidates[ ci ];
					auto& candidate_hits = per_candidate[ ci ];
					candidate_hits.reserve( 24 );

					// Resolver hypothesis for this pawn (0 = no correction). Read once per candidate;
					// the whole pawn shares one hypothesis, so every backtrack record is yawed by
					// the same delta. Reserve the pose store up front -- record_count pushes into a
					// reserved vector keep every stored pointer stable across the scan.
					const auto hypo = resolver_on ? g_shared.res( ).hypothesis( cand.pawn ) : 0;
					const auto yaw_delta = shared::resolver::yaw_delta( hypo, desync_range );
					if ( yaw_delta != 0.0f )
					{
						cand.resolver_poses.reserve( static_cast< std::size_t >( cand.record_count ) );
					}

					for ( auto ri = 0; ri < cand.record_count; ++ri )
					{
						if ( !cand.records[ ri ] || !cand.records[ ri ]->valid )
						{
							continue;
						}

						// Every record spread across the backtrack window is scanned; backtrack_records
						// (per weapon) sets how many gather_candidates handed us. select_best then
						// decides which record and point is actually best, so a 12-damage graze on a
						// foot cannot pre-empt a 90-damage wallbang on an older pose.
						auto* record = cand.records[ ri ];

						// When the pawn is on a non-zero hypothesis, scan a yawed copy of the pose
						// instead. tick/simulation_time/origin are copied through unchanged, so
						// backtrack timing stays correct while the bones (and therefore the aim point
						// and hitchance) move to where we guess the real hitbox is.
						if ( yaw_delta != 0.0f )
						{
							cand.resolver_poses.push_back( shared::resolver::rotate_pose( *record, yaw_delta ) );
							record = &cand.resolver_poses.back( );
						}

						this->m_scan_diag.records.fetch_add( 1, std::memory_order_relaxed );
						auto hits = this->scan_player( eye, inaccuracy, ctx, cand, record, local );

						for ( auto& h : hits )
						{
							candidate_hits.push_back( std::move( h ) );
						}
					}
				}
			}, 1 );

		std::vector<scan_hit> flat;
		auto total_hits{ std::size_t{} };
		for ( const auto& hits : per_candidate )
		{
			total_hits += hits.size( );
		}
		flat.reserve( total_hits );

		for ( auto& v : per_candidate )
		{
			for ( auto& h : v )
			{
				flat.push_back( std::move( h ) );
			}
		}

		return flat;
	}

	std::vector<rage::scan_hit> rage::scan_player( const math::vector3& eye, float inaccuracy, const aim_context& ctx, candidate& cand, shared::lagcomp::record* record, const systems::local::snapshot& local ) const
	{
		// idk how this happens
		if (!cand.pawn || cand.record_count <= 0 || cand.health <= 0)
			return {};

		const auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );

		const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
		const auto skeleton = g_shared.lc( ).get_skeleton( *record );
		const auto pen_ctx = g_shared.pen( ).prepare_target( cand.pawn, record );

		// Safe-point evaluation is opt-in: it costs two extra penetration traces per surviving
		// point, so only build the extreme poses when a setting actually consumes is_safe.
		const auto need_safety = config.prefer_safe_point.value || config.force_safe_point.value || config.baim_lethal.value;
		const auto desync_range = static_cast< float >( config.desync_range.value );

		shared::lagcomp::record pose_plus{};
		shared::lagcomp::record pose_minus{};
		shared::penetration::run_context pen_ctx_plus{};
		shared::penetration::run_context pen_ctx_minus{};
		if ( need_safety )
		{
			// The pose swung to both desync extremes about the record we're scanning. A point
			// that still deals min_damage after the hitboxes rotate ±desync_range lands no
			// matter which way the enemy's real desync is turned -- that is the safe guarantee.
			pose_plus = shared::resolver::rotate_pose( *record, desync_range );
			pose_minus = shared::resolver::rotate_pose( *record, -desync_range );
			pen_ctx_plus = g_shared.pen( ).prepare_target( cand.pawn, &pose_plus );
			pen_ctx_minus = g_shared.pen( ).prepare_target( cand.pawn, &pose_minus );
		}

		const auto force_body = config.body_aim.value;

		std::array<int, 19> scan_order{};
		auto scan_count{ 0 };

		if ( !force_body && config.hitboxes.values[ 0 ] )
		{
			scan_order[ scan_count++ ] = 0;
		}

		if ( config.hitboxes.values[ 1 ] )
		{
			scan_order[ scan_count++ ] = 4;
			scan_order[ scan_count++ ] = 5;
			scan_order[ scan_count++ ] = 6;
		}

		if ( config.hitboxes.values[ 2 ] )
		{
			scan_order[ scan_count++ ] = 3;
			scan_order[ scan_count++ ] = 2;
		}

		if ( config.hitboxes.values[ 3 ] )
		{
			for ( auto idx : { 13, 14, 15, 16, 17, 18 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		if ( config.hitboxes.values[ 4 ] )
		{
			for ( auto idx : { 7, 8, 9, 10 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		if ( config.hitboxes.values[ 5 ] )
		{
			for ( auto idx : { 11, 12 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		// Old configs can deserialize with every hitbox disabled. Keep the
		// ragebot operational with the core head and torso hitboxes.
		if ( scan_count == 0 )
		{
			if ( !force_body )
			{
				scan_order[ scan_count++ ] = 0;
			}

			for ( auto idx : { 4, 5, 6, 3, 2 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		struct trace_point
		{
			math::vector3 position;
			int hitbox_index;
			int bone_index;
			systems::hitboxes::entry hitbox;
			bool is_center;
		};

		std::vector<trace_point> points;
		points.reserve( static_cast< std::size_t >( scan_count ) * 12 );

		for ( auto idx = 0; idx < scan_count; ++idx )
		{
			const auto hitbox_index = scan_order[ idx ];
			const systems::hitboxes::entry* hb{ nullptr };

			for ( const auto& entry : hitbox_set )
			{
				if ( entry.index == hitbox_index )
				{
					hb = &entry;
					break;
				}
			}

			if ( !hb || hb->bone < 0 || hb->bone >= 28 )
			{
				continue;
			}

			const auto& bone = skeleton[ hb->bone ];
			if ( bone.position.length_sqr( ) < 1.0f )
			{
				continue;
			}

			const auto hitbox_center = ( hb->mins + hb->maxs ) * 0.5f;
			const auto center = bone.rotation.rotate_vector( hitbox_center ) + bone.position;

			trace_point cp{};
			cp.position = center;
			cp.hitbox_index = hitbox_index;
			cp.bone_index = hb->bone;
			cp.hitbox = *hb;
			cp.is_center = true;
			points.push_back( cp );

			if ( config.debug_multipoints.value )
			{
				std::lock_guard lock( m_debug_mtx );
				m_debug_points.push_back( { center, hitbox_index, true } );
			}

			if ( config.pointscale > 0.0f )
			{
				const auto mps = this->generate_multipoints( *hb, center, bone.rotation, config.pointscale, eye, inaccuracy );

				for ( const auto& mp : mps )
				{
					const auto duplicate = std::any_of( points.begin( ), points.end( ), [ & ]( const trace_point& point )
						{
							return point.hitbox_index == hitbox_index && ( point.position - mp ).length_sqr( ) < 0.01f;
						} );
					if ( duplicate )
					{
						continue;
					}

					trace_point tp{};
					tp.position = mp;
					tp.hitbox_index = hitbox_index;
					tp.bone_index = hb->bone;
					tp.hitbox = *hb;
					tp.is_center = false;
					points.push_back( tp );

					if ( config.debug_multipoints.value )
					{
						std::lock_guard lock( m_debug_mtx );
						m_debug_points.push_back( { mp, hitbox_index, false } );
					}
				}
			}
		}

		if ( points.empty( ) )
		{
			return {};
		}

		std::vector<scan_hit> results;
		results.reserve( static_cast< std::size_t >( scan_count ) * 2 );
		std::array<bool, 19> center_sufficient{};

		for ( const auto& tp : points )
		{
			if ( !tp.is_center && tp.hitbox_index >= 0 && tp.hitbox_index < static_cast< int >( center_sufficient.size( ) ) && center_sufficient[ tp.hitbox_index ] )
			{
				continue;
			}

			const auto aim = math::helpers::calculate_angle( eye, tp.position );
			const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

			this->m_scan_diag.points.fetch_add( 1, std::memory_order_relaxed );

			if ( fov > config.max_fov )
			{
				this->m_scan_diag.fov.fetch_add( 1, std::memory_order_relaxed );
				continue;
			}

			shared::penetration::result pen{};
			// tp.hitbox_index is the point we aimed at. run( ) only falls back to it when the
			// engine confirmed damage on this exact pawn and its own straight-ray bone pass
			// came up empty -- the signature of a bullet deflected by the wall it went through.
			if ( !g_shared.pen( ).run( eye, tp.position, pen_ctx, local.pawn, local.team, pen, tp.hitbox_index ) )
			{
				this->m_scan_diag.no_damage.fetch_add( 1, std::memory_order_relaxed );
				continue;
			}

			{
				const auto seen = static_cast< int >( pen.damage );
				auto best = this->m_scan_diag.best_damage.load( std::memory_order_relaxed );
				while ( seen > best && !this->m_scan_diag.best_damage.compare_exchange_weak( best, seen, std::memory_order_relaxed ) )
				{
				}
			}

			if ( pen.damage < cand.min_damage )
			{
				this->m_scan_diag.below_min.fetch_add( 1, std::memory_order_relaxed );
				continue;
			}

			if ( !tp.is_center && tp.hitbox_index == 0 )
			{
				if ( pen.hitgroup != systems::g_hitboxes.hitgroup_from_hitbox( tp.hitbox_index ) )
				{
					this->m_scan_diag.head_group.fetch_add( 1, std::memory_order_relaxed );
					continue;
				}
			}

			this->m_scan_diag.accepted.fetch_add( 1, std::memory_order_relaxed );

			if ( tp.is_center && tp.hitbox_index >= 0 && tp.hitbox_index < static_cast< int >( center_sufficient.size( ) ) )
			{
				center_sufficient[ tp.hitbox_index ] = !pen.penetrated || pen.damage >= static_cast< float >( cand.health );
			}

			scan_hit h{};
			h.position = tp.position;
			h.aim_angle = aim;
			h.damage = pen.damage;
			h.fov = fov;
			h.hitbox_index = tp.hitbox_index;
			h.hitgroup = pen.hitgroup;
			h.bone_index = tp.bone_index;
			h.hitbox = tp.hitbox;
			h.is_center = tp.is_center;
			h.penetrated = pen.penetrated;
			h.pawn = cand.pawn;
			h.health = cand.health;
			h.record = record;

			if ( need_safety )
			{
				// Same aim point, but validated against the two extreme poses. Both must still
				// meet min_damage for the point to count as safe.
				shared::penetration::result pen_p{};
				shared::penetration::result pen_m{};
				const auto safe_plus = g_shared.pen( ).run( eye, tp.position, pen_ctx_plus, local.pawn, local.team, pen_p, tp.hitbox_index ) && pen_p.damage >= cand.min_damage;
				const auto safe_minus = g_shared.pen( ).run( eye, tp.position, pen_ctx_minus, local.pawn, local.team, pen_m, tp.hitbox_index ) && pen_m.damage >= cand.min_damage;
				h.is_safe = safe_plus && safe_minus;
			}

			results.push_back( h );
		}

		return results;
	}

	rage::target rage::select_best( const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy ) const
	{
		auto hitgroup_priority = [ ]( int hitbox_index ) -> int
			{
				if ( hitbox_index == 0 ) { return 4; }
				if ( hitbox_index >= 1 && hitbox_index <= 6 ) { return 3; }
				if ( hitbox_index >= 13 && hitbox_index <= 18 ) { return 2; }
				if ( hitbox_index >= 7 && hitbox_index <= 12 ) { return 1; }
				return 0;
			};

		struct record_group
		{
			shared::lagcomp::record* record;
			std::vector<int> hit_indices;
		};

		std::vector<record_group> groups;
		groups.reserve( 16 );

		for ( auto i = 0; i < static_cast< int >( hits.size( ) ); ++i )
		{
			auto rec = hits[ i ].record;
			auto found{ false };

			for ( auto& g : groups )
			{
				if ( g.record == rec )
				{
					g.hit_indices.push_back( i );
					found = true;
					break;
				}
			}

			if ( !found )
			{
				record_group g{};
				g.record = rec;
				g.hit_indices.reserve( 16 );
				g.hit_indices.push_back( i );
				groups.push_back( std::move( g ) );
			}
		}

		constexpr auto top_k_per_record{ 8 };

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto prefer_safe = config.prefer_safe_point.value || config.force_safe_point.value;

		auto cheap_score = [ & ]( const scan_hit& h ) -> float
			{
				const auto lethal_bonus = h.damage >= static_cast< float >( h.health ) ? 100000.0f : 0.0f;
				// 500, not 5000. At 5000 a direct hit outranked 250 points of damage, so the
				// cheap prefilter threw away every wallbang before the real scorer ever saw it.
				// Visibility is still worth something -- it just no longer beats the damage term.
				const auto direct_bonus = h.penetrated ? 0.0f : 500.0f;
				const auto center_bonus = h.is_center ? 2000.0f : 0.0f;
				// Keep safe points alive through the top-k trim when the user is asking for them,
				// or force_safe_point could prune every survivor and leave nothing to shoot.
				const auto safe_bonus = ( prefer_safe && h.is_safe ) ? 50000.0f : 0.0f;

				return lethal_bonus + safe_bonus + direct_bonus + center_bonus + h.damage * 20.0f +
					static_cast< float >( hitgroup_priority( h.hitbox_index ) ) * 10.0f - h.fov;
			};


		for ( auto& group : groups )
		{
			if ( static_cast< int >( group.hit_indices.size( ) ) <= top_k_per_record )
			{
				continue;
			}

			std::partial_sort
			(
				group.hit_indices.begin( ),
				group.hit_indices.begin( ) + top_k_per_record,
				group.hit_indices.end( ),
				[ & ]( int a, int b ) { return cheap_score( hits[ a ] ) > cheap_score( hits[ b ] ); }
			);

			group.hit_indices.resize( top_k_per_record );
		}

		struct evaluated_hit
		{
			int hit_index;
			float hitchance;
			float score;
		};

		std::vector<evaluated_hit> evaluated;
		evaluated.reserve( hits.size( ) );

		const auto needed_hc = config.hitchance_override.value
			? static_cast< float >( config.hitchance_override_value ) / 100.0f
			: static_cast< float >( config.hitchance ) / 100.0f;

		for ( auto& group : groups )
		{
			if ( !group.record || !group.record->valid )
			{
				continue;
			}

			for ( const auto idx : group.hit_indices )
			{
				const auto& h = hits[ idx ];

				if ( !h.record || !h.record->valid )
				{
					continue;
				}

				if ( h.bone_index < 0 || h.bone_index >= 28 )
				{
					continue;
				}

				// force_safe_point takes only guaranteed points -- if none survive, we hold fire
				// rather than take a gamble the user asked us not to take.
				if ( config.force_safe_point.value && !h.is_safe )
				{
					continue;
				}

				const auto& bone = group.record->bones[ h.bone_index ];
				const auto hc = config.no_spread.value
					? 1.0f
					: g_shared.calculate_hitchance( h.source_eye.position, h.aim_angle, h.hitbox, bone, eval_inaccuracy, aim_ctx.spread );
				const auto hp = static_cast< float >( h.health );
				const auto can_kill = h.damage >= hp;
				const auto passes_hitchance = config.no_spread.value || hc >= needed_hc;
				auto score = passes_hitchance ? 1000000.0f : 0.0f;

				if ( can_kill )
				{
					score += 100000.0f + hc * 10000.0f;
				}
				else
				{
					score += h.damage * hc * 100.0f + h.damage * 5.0f;
				}

				// 25, not 250. The damage term above contributes h.damage * hc * 100, so a
				// 250-point flat bonus was worth ~25 damage at full hitchance and reliably
				// picked a weak visible hit over a strong penetrated one.
				score += h.penetrated ? 0.0f : 25.0f;
				score += h.is_center ? 50.0f : 0.0f;
				score += static_cast< float >( hitgroup_priority( h.hitbox_index ) ) * 2.0f;
				score -= h.fov * 0.1f;

				// Shoot the enemy where they are now, not where they were. A backtrack record is a guess about the
				// server's lag compensation and it shows as a shot at an empty spot; it is worth taking only when the
				// newer records cannot deliver. 5000 per tick of age outweighs every damage, hitbox and centre term
				// above, and stays under the 100000 kill bonus -- so an older record still wins when it is the only
				// kill on offer, and when it is the only thing that can be hit at all.
				const auto record_age = std::max( 0, g_shared.ctx( ).current_tick - h.record->tick );
				score -= static_cast< float >( record_age ) * 5000.0f;

				if ( config.prefer_safe_point.value && h.is_safe )
				{
					// Above the can_kill bonus (100000) on purpose: a guaranteed safe hit is worth
					// more than an unsafe point that only kills if the enemy's desync cooperates.
					score += 200000.0f;
				}

				if ( config.baim_lethal.value && h.hitbox_index == 0 && !h.is_safe )
				{
					// Head isn't guaranteed against the desync. Sink it under the body points --
					// they all cleared min_damage to reach the scorer -- so a reliable body shot wins.
					score -= 150000.0f;
				}

				evaluated.push_back( evaluated_hit{ idx, hc, score } );
			}
		}

		target best{};

		for ( const auto& e : evaluated )
		{
			if ( e.hit_index < 0 || e.hit_index >= static_cast< int >( hits.size( ) ) )
			{
				continue;
			}

			const auto& h = hits[ e.hit_index ];

			if ( !h.record || !h.record->valid )
			{
				continue;
			}

			auto is_better = !best.valid || e.score > best.score;
			if ( best.valid && std::fabsf( e.score - best.score ) < 0.01f )
			{
				if ( h.record->tick != best.hit.record->tick )
				{
					is_better = h.record->tick > best.hit.record->tick;
				}
				else if ( h.is_center != best.hit.is_center )
				{
					is_better = h.is_center;
				}
				else
				{
					is_better = h.fov < best.hit.fov;
				}
			}

			if ( is_better )
			{
				best.hit = h;
				best.hitchance = e.hitchance;
				best.score = e.score;
				best.valid = true;
			}
		}

		return best;
	}

	float rage::evaluate_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy ) const
	{
		if ( !hit.record || !hit.record->valid || hit.bone_index < 0 || hit.bone_index >= 28 )
		{
			return 0.0f;
		}

		return g_shared.calculate_hitchance( hit.source_eye.position, hit.aim_angle, hit.hitbox, hit.record->bones[ hit.bone_index ], inaccuracy, ctx.spread );
	}

	void rage::report_decision_diag( )
	{
		const auto now = GetTickCount64( );
		if ( now - this->m_diag.last_report < 2500ull )
		{
			return;
		}

		this->m_diag.last_report = now;

		const auto& d = this->m_diag;
		if ( d.frames == 0 && d.cant_shoot == 0 )
		{
			return;
		}

		diag::writef(
			diag::level::debug,
			"rage: frames=%d cant_shoot=%d no_candidates=%d no_hit=%d targets=%d fired=%d | held: hitchance=%d (avg short by %.0f%%) duckpeek=%d | autostop_frames=%d | avg hc on target=%.0f%% | ticks target->shot: avg=%.1f max=%d n=%d | scan: candidates=%d extrapolated=%d records=%d points=%d fov=%d no_damage=%d below_min=%d head_group=%d ok=%d best_dmg=%d",
			d.frames, d.cant_shoot, d.no_candidates, d.no_hit, d.targets, d.fired,
			d.held_hitchance, d.held_hitchance ? d.hc_short_sum / static_cast< float >( d.held_hitchance ) * 100.0f : 0.0f,
			d.held_duck, d.stop_frames,
			d.targets ? d.hc_sum / static_cast< float >( d.targets ) * 100.0f : 0.0f,
			d.latency_n ? static_cast< float >( d.latency_sum ) / static_cast< float >( d.latency_n ) : 0.0f,
			d.latency_max, d.latency_n,
			this->m_scan_diag.candidates.exchange( 0 ), this->m_scan_diag.extrapolated.exchange( 0 ), this->m_scan_diag.records.exchange( 0 ),
			this->m_scan_diag.points.exchange( 0 ), this->m_scan_diag.fov.exchange( 0 ), this->m_scan_diag.no_damage.exchange( 0 ),
			this->m_scan_diag.below_min.exchange( 0 ), this->m_scan_diag.head_group.exchange( 0 ), this->m_scan_diag.accepted.exchange( 0 ),
			this->m_scan_diag.best_damage.exchange( 0 ) );

		this->m_diag = decision_diag{};
		this->m_diag.last_report = now;
	}

	float rage::get_standing_inaccuracy( const systems::local::snapshot& local, const aim_context& ctx ) const
	{
		const auto& prestate = systems::g_prediction.pre( );
		auto velocity = prestate.networked_velocity;
		velocity.z = 0.0f;

		const auto speed = velocity.length_2d( );
		if ( speed > ctx.accurate_threshold )
		{
			return g_shared.get_inaccuracy_at_velocity( local.pawn, velocity );
		}

		const auto& shared_ctx = g_shared.ctx( );
		if ( !shared_ctx.weapon_vdata )
		{
			return ctx.predicted_inaccuracy;
		}

		const auto inaccuracy_stand = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyStand"_hash ) );
		return std::max( inaccuracy_stand, g_shared.get_inaccuracy_at_velocity( local.pawn, velocity ) );
	}

	std::vector<rage::scan_hit> rage::scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		std::vector<scan_hit> results;

		for ( auto& cand : candidates )
		{
			for ( auto ri = 0; ri < cand.record_count; ++ri )
			{
				auto* record = cand.records[ ri ];
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !game_scene_node )
				{
					continue;
				}

				const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
				if ( hitbox_set.count <= 0 )
				{
					continue;
				}

				record->apply( );
				const auto skeleton = g_shared.lc( ).get_skeleton( *record );

				for ( auto i = 0; i < hitbox_set.count; ++i )
				{
					const auto& hb = hitbox_set.entries[ i ];

					if ( hb.bone < 0 || hb.bone >= static_cast< int >( skeleton.size( ) ) )
					{
						continue;
					}

					const auto& bone = skeleton[ hb.bone ];
					if ( bone.position.length_sqr( ) < 1.0f )
					{
						continue;
					}

					const auto center = bone.rotation.rotate_vector( ( hb.mins + hb.maxs ) * 0.5f ) + bone.position;
					const auto aim = math::helpers::calculate_angle( eye, center );
					const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

					if ( fov > settings::g_combat.m_zeusbot.max_fov )
					{
						continue;
					}

					math::vector3 forward{};
					math::helpers::angle_vectors_left( aim, &forward );

					const auto trace = this->trace_taser_hit( eye, forward, shared_ctx.range * 0.85f, cand.pawn, local.pawn );
					if ( trace.hit_entity != cand.pawn )
					{
						continue;
					}

					const auto dist = ( center - eye ).length( );
					const auto range_fraction = dist / shared_ctx.range;

					scan_hit h{};
					h.position = center;
					h.aim_angle = aim;
					h.damage = 500.0f;
					h.score = ( 10000.0f - dist ) * ( range_fraction > 0.92f ? 0.8f : 1.0f );
					h.fov = fov;
					h.hitbox_index = hb.index;
					h.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hb.index );
					h.bone_index = hb.bone;
					h.hitbox = hb;
					h.is_center = true;
					h.pawn = cand.pawn;
					h.health = cand.health;
					h.record = record;

					results.push_back( h );
				}

				record->restore( );
			}
		}

		return results;
	}

	rage::knife_info rage::get_knife_info( const systems::local::snapshot& local ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto next_primary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		const auto next_secondary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick"_hash ) );
		const auto last_shot_time = memory::read<float>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_fLastShotTime"_hash ) );
		const auto cur_time = static_cast< float >( tick_base ) * cstypes::tick_interval;

		return knife_info
		{
			.can_slash = tick_base >= next_primary,
			.can_stab = tick_base >= next_secondary,
			.charged = ( cur_time - last_shot_time ) > 0.4f,
			.armor_ratio = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flArmorRatio"_hash ) )
		};
	}

	std::vector<rage::scan_hit> rage::scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		constexpr auto stab_range{ 50.0f };
		constexpr auto slash_range{ 66.0f };

		std::vector<scan_hit> results;

		for ( auto& cand : candidates )
		{
			const auto eye_angles = memory::read<math::vector3>( cand.pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
			const auto hp = static_cast< float >( cand.health );

			const auto frontal_slash_dmg = this->get_knife_damage( info.charged ? 40.0f : 25.0f, cand.armor, info.armor_ratio );
			const auto frontal_stab_dmg = this->get_knife_damage( 65.0f, cand.armor, info.armor_ratio );
			const auto frontal_can_kill = ( info.can_slash && frontal_slash_dmg >= hp ) || ( info.can_stab && frontal_stab_dmg >= hp );

			for ( auto ri = 0; ri < cand.record_count; ++ri )
			{
				auto* record = cand.records[ ri ];
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !game_scene_node )
				{
					continue;
				}

				const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
				if ( hitbox_set.count <= 0 )
				{
					continue;
				}

				record->apply( );
				const auto skeleton = g_shared.lc( ).get_skeleton( *record );

				auto backstab{ false };
				{
					const auto delta = record->origin - systems::g_prediction.pre( ).origin;
					const auto dist_2d = std::sqrtf( delta.x * delta.x + delta.y * delta.y );

					if ( dist_2d > 0.001f )
					{
						const auto dir_x = delta.x / dist_2d;
						const auto dir_y = delta.y / dist_2d;

						math::vector3 body_forward{};
						math::helpers::angle_vectors_left( record->rotation, &body_forward );

						math::vector3 eye_forward{};
						math::helpers::angle_vectors_left( eye_angles, &eye_forward );

						backstab = ( dir_x * body_forward.x + dir_y * body_forward.y ) > 0.475f ||
							( dir_x * eye_forward.x + dir_y * eye_forward.y ) > 0.475f;
					}
				}

				const auto wait_for_backstab = backstab && !frontal_can_kill;

				for ( auto i = 0; i < hitbox_set.count; ++i )
				{
					const auto& hb = hitbox_set.entries[ i ];

					if ( hb.bone < 0 || hb.bone >= static_cast< int >( skeleton.size( ) ) )
					{
						continue;
					}

					const auto& bone = skeleton[ hb.bone ];
					if ( bone.position.length_sqr( ) < 1.0f )
					{
						continue;
					}

					const auto center = bone.rotation.rotate_vector( ( hb.mins + hb.maxs ) * 0.5f ) + bone.position;
					const auto dist = ( center - eye ).length( );
					const auto max_reach = info.can_slash ? slash_range : stab_range;

					if ( dist > max_reach )
					{
						continue;
					}

					const auto aim = math::helpers::calculate_angle( eye, center );
					const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

					if ( fov > settings::g_combat.m_knifebot.max_fov )
					{
						continue;
					}

					math::vector3 forward{};
					math::helpers::angle_vectors_left( aim, &forward );

					for ( const auto try_stab : { true, false } )
					{
						if ( try_stab && !info.can_stab )
						{
							continue;
						}

						if ( !try_stab && !info.can_slash )
						{
							continue;
						}

						const auto reach = try_stab ? stab_range : slash_range;
						if ( dist > reach )
						{
							continue;
						}

						const auto raw_dmg = try_stab ? ( backstab ? 180.0f : 65.0f ) : ( backstab ? 90.0f : ( info.charged ? 40.0f : 25.0f ) );
						const auto damage = this->get_knife_damage( raw_dmg, cand.armor, info.armor_ratio );
						const auto can_kill = damage >= hp;

						if ( wait_for_backstab && !can_kill )
						{
							continue;
						}

						const auto trace = this->trace_knife_hit( eye, forward, reach, cand.pawn, local.pawn );
						if ( trace.hit_entity != cand.pawn )
						{
							continue;
						}

						const auto reach_margin = 1.0f - ( dist / reach );

						scan_hit h{};
						h.position = center;
						h.aim_angle = aim;
						h.damage = damage;
						h.score = can_kill ? ( 10000.0f + damage * reach_margin ) : ( damage * 100.0f * reach_margin );
						h.fov = fov;
						h.hitbox_index = hb.index;
						h.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hb.index );
						h.bone_index = hb.bone;
						h.hitbox = hb;
						h.is_center = true;
						h.is_backstab = backstab;
						h.attack_type = try_stab ? 1 : 0;
						h.pawn = cand.pawn;
						h.health = cand.health;
						h.record = record;

						results.push_back( h );
						break;
					}
				}

				record->restore( );
			}
		}

		return results;
	}

	void rage::fire_gun( systems::input::usercmd* cmd, const target& tgt, bool was_forced, const math::vector3& shoot_eye, const systems::local::snapshot& local, bool want_subtick )
	{
		if ( !tgt.hit.record || !tgt.hit.record->valid )
		{
			return;
		}

		this->m_firing_this_tick = true;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );

		// The time the shot is stamped with. It is written into every input history entry below, and it is the
		// time the fire code evaluates the spread seed and the aim punch at.
		auto stamp_tick = tick_base;
		auto stamp_frac{ 0.0f };

		if ( !tgt.hit.source_eye.is_uninterpolated )
		{
			auto tick_add = [ ]( int t, float f, int dt, float df )
				{
					f += df;
					auto carry = static_cast< int >( std::floor( f ) );
					f -= static_cast< float >( carry );
					return std::pair{ t + dt + carry, f };
				};

			std::tie( stamp_tick, stamp_frac ) = tick_add( tgt.hit.source_eye.player_tick, tgt.hit.source_eye.player_frac, tgt.hit.source_eye.lerp_ticks_int, tgt.hit.source_eye.lerp_ticks_frac );
		}

		// The punch the fire code adds for this shot, evaluated at the shot's own time the way the fire code does
		// it (0x80FD8F). The render-time punch the camera uses drifts from it while the recoil settles, and with no
		// spread on that drift is enough to move the angles into another seed cell -- follow-up shots fired while
		// the punch was still moving went wide.
		const auto render_punch = g_shared.get_aim_punch( local.pawn );
		auto aim_punch = g_shared.get_aim_punch_at( local.pawn, stamp_tick, stamp_frac );

		// The two are the same recoil a fraction of a tick apart. If they disagree by degrees the shot-time call
		// is not doing what it did when this was written (a game update moved it), and the render-time value is
		// the safer of the two.
		const auto punch_sane = std::isfinite( aim_punch.x ) && std::isfinite( aim_punch.y ) && std::isfinite( aim_punch.z ) &&
			std::fabsf( aim_punch.x - render_punch.x ) < 3.0f && std::fabsf( aim_punch.y - render_punch.y ) < 3.0f;
		if ( !punch_sane )
		{
			diag::writef( diag::level::warning, "shot-time punch (%.3f %.3f %.3f) disagrees with render punch (%.3f %.3f); using render punch", aim_punch.x, aim_punch.y, aim_punch.z, render_punch.x, render_punch.y );
			aim_punch = render_punch;
		}

		// Where the bullet is meant to go. With no spread the angles sent differ from it on purpose, so this is
		// what the impact logger has to measure a miss against.
		const auto intended_angle = config.no_spread.value ? math::helpers::calculate_angle( shoot_eye, tgt.hit.position ) : tgt.hit.aim_angle;
		auto aim_angle = intended_angle;

		shared::spread_solution solution{};
		if ( config.no_spread.value )
		{
			solution = g_shared.find_spread_correction( intended_angle, stamp_tick, aim_punch );
			if ( !solution.valid )
			{
				diag::writef( diag::level::info, "no spread: no solution after %d cells (inaccuracy %.4f spread %.4f)", solution.evaluated, shared_ctx.inaccuracy, shared_ctx.spread );
				this->m_firing_this_tick = false;
				return;
			}

			aim_angle = solution.shot_angles;
		}

		// Point of no return. Every early return above leaves the command untouched, which is the
		// whole point of the split: the pulse used to be appended by run_gun before this function
		// ran, so a bail-out here shipped an attack with no aim behind it and the bullet left along
		// whatever yaw the command was carrying -- the anti-aim fake, if one was on.
		const auto subtick_attack = want_subtick && this->emit_subtick_shot( cmd );

		g_shared.last_shoot_tick( ) = tick_base;

		// Tell the resolver a shot is committed under this pawn's current hypothesis. on_tick
		// watches the victim's health from here: no drop within the grace window rotates the
		// hypothesis. hypothesis() is unchanged since the scan (on_tick only runs at the top of
		// create_move), so this is the same side the aim point was built from.
		g_shared.res( ).on_fired( tgt.hit.pawn, g_shared.res( ).hypothesis( tgt.hit.pawn ), tgt.hit.health );

		if ( settings::g_misc.m_impacts.console_log.value )
		{
			const auto hitgroup_name = systems::g_hitboxes.hitgroup_to_name( tgt.hit.hitgroup );
			const auto bt_delta = g_shared.ctx( ).current_tick - tgt.hit.record->tick;
			logging::console::print(
				xs( "[rage] shot target hp {} for {:.0f} in {} (hc {:.0f}%, bt {}t{})" ),
				tgt.hit.health,
				tgt.hit.damage,
				hitgroup_name,
				tgt.hitchance * 100.0f,
				bt_delta,
				was_forced ? xs( ", forced" ) : ""
			);
		}

		features::misc::g_impacts.on_boom( tgt.hit.pawn, tgt.hit.hitgroup, tgt.hit.damage, tgt.hitchance, shared_ctx.inaccuracy, shared_ctx.spread, intended_angle, shoot_eye, tgt.hit.record->tick, g_shared.lc( ).get_skeleton( *tgt.hit.record ), was_forced );
		features::esp::player::g_chams.os ().push (tgt.hit.pawn);
		const auto record_time = cstypes::tick_fraction::from_value( tgt.hit.record->simulation_time / cstypes::tick_interval );
		const auto history_size = cmd->csgo_user_cmd.input_history_size( );
		for ( auto i = 0; i < history_size; ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( !entry )
			{
				continue;
			}

			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( aim_angle.x - aim_punch.x );
				angles->set_y( aim_angle.y - aim_punch.y );

				// The fire code adds the whole punch vector, roll included, before it builds the bullet frame.
				if ( config.no_spread.value )
				{
					angles->set_z( aim_angle.z - aim_punch.z );
				}
			}

			entry->set_render_tick_count( record_time.tick + 1 );
			entry->set_render_tick_fraction( 0.0f );

			if ( !tgt.hit.source_eye.is_uninterpolated )
			{
				entry->set_player_tick_count( stamp_tick );
				entry->set_player_tick_fraction( stamp_frac );
			}

			if ( entry->has_sv_interp0( ) )
			{
				const auto interp = entry->mutable_sv_interp0( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_sv_interp1( ) )
			{
				const auto interp = entry->mutable_sv_interp1( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_cl_interp( ) )
			{
				const auto interp = entry->mutable_cl_interp( );
				interp->set_frac( 0.0f );
			}
		}

		if ( !subtick_attack )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;

			if ( history_size > 0 )
			{
				cmd->csgo_user_cmd.set_attack1_start_history_index( history_size - 1 );
			}
		}

		math::vector3 forward{};
		{
			if ( const auto angles = base->viewangles( ) )
			{
				math::helpers::angle_vectors_left( { angles->x( ), angles->y( ), angles->z( ) }, &forward );
			}
		}

		const auto punched_aim = math::vector3{ aim_angle.x - aim_punch.x, aim_angle.y - aim_punch.y, 0.0f };
		const auto facing_away = forward.dot( ( tgt.hit.record->origin - systems::g_prediction.pre( ).networked_origin ).normalized( ) ) < 0.707107f;

		auto command_aim = punched_aim;
		if ( !subtick_attack && facing_away && settings::g_combat.m_antiaim.hide_shots.value )
		{
			command_aim.x = 179.9f;
			command_aim.y = std::remainderf( punched_aim.y + 180.0f, 360.0f );
		}

		if ( const auto angles = base->mutable_viewangles( ) )
		{
			angles->set_x( command_aim.x );
			angles->set_y( command_aim.y );
		}

		if ( !config.silent.value )
		{
			systems::g_input.set_view_angles( punched_aim );
		}

		// Re-trace the exact point that was fired at, with contact logging on, so the log carries the surfaces
		// behind this shot's predicted damage.
		if ( tgt.hit.penetrated )
		{
			const auto dump_ctx = g_shared.pen( ).prepare_target( tgt.hit.pawn, tgt.hit.record );
			shared::penetration::result dump{};
			( void )g_shared.pen( ).run( shoot_eye, tgt.hit.position, dump_ctx, local.pawn, local.team, dump, tgt.hit.hitbox_index, true );
		}

		// Log-file detail for this shot, paired with the "shot detail" line the impact code writes on a miss.
		// Everything that turns a scan result into the bytes the server reads is here: the aim angle, the
		// punch taken off it, the angle the command actually carries, and the ticks stamped onto the input.
		diag::writef(
			diag::level::info,
			"fire detail: no_spread=%d silent=%d subtick=%d forced=%d | eye=(%.1f %.1f %.1f) uninterp=%d player_tick=%d frac=%.3f lerp=%d+%.3f | aim=(%.2f %.2f %.2f) punch=(%.3f %.3f %.3f) render_punch=(%.3f %.3f) cmd=(%.2f %.2f) | tick_base=%d current=%d record_age=%d hp=%d | solver cells=%d margin=%.3f | pen=%d hitbox=%d dmg=%.0f | inacc=%.5f at vz=%.1f",
			config.no_spread.value ? 1 : 0, config.silent.value ? 1 : 0, subtick_attack ? 1 : 0, was_forced ? 1 : 0,
			shoot_eye.x, shoot_eye.y, shoot_eye.z,
			tgt.hit.source_eye.is_uninterpolated ? 1 : 0, tgt.hit.source_eye.player_tick, tgt.hit.source_eye.player_frac, tgt.hit.source_eye.lerp_ticks_int, tgt.hit.source_eye.lerp_ticks_frac,
			aim_angle.x, aim_angle.y, aim_angle.z, aim_punch.x, aim_punch.y, aim_punch.z, render_punch.x, render_punch.y, command_aim.x, command_aim.y,
			tick_base, shared_ctx.current_tick, shared_ctx.current_tick - tgt.hit.record->tick, tgt.hit.health,
			solution.evaluated, solution.margin,
			tgt.hit.penetrated ? 1 : 0, tgt.hit.hitbox_index, tgt.hit.damage,
			shared_ctx.inaccuracy, shared_ctx.inaccuracy_velocity_z );
	}

	void rage::fire_melee( systems::input::usercmd* cmd, const target& tgt, const systems::local::snapshot& local )
	{
		if ( !tgt.hit.record || !tgt.hit.record->valid )
		{
			return;
		}

		this->m_firing_this_tick = true;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );

		g_shared.last_shoot_tick( ) = tick_base;

		const auto record_time = cstypes::tick_fraction::from_value( tgt.hit.record->simulation_time / cstypes::tick_interval );
		const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		const auto entry = history_index >= 0 ? cmd->csgo_user_cmd.mutable_input_history( history_index ) : nullptr;

		if ( entry )
		{
			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( tgt.hit.aim_angle.x );
				angles->set_y( tgt.hit.aim_angle.y );
			}

			entry->set_render_tick_count( record_time.tick + 1 );
			entry->set_render_tick_fraction( 0.0f );

			if ( !tgt.hit.source_eye.is_uninterpolated )
			{
				auto tick_add = [ ]( int t, float f, int dt, float df )
					{
						f += df;
						auto carry = static_cast< int >( std::floor( f ) );
						f -= static_cast< float >( carry );
						return std::pair{ t + dt + carry, f };
					};

				const auto [stamp_tick, stamp_frac] = tick_add( tgt.hit.source_eye.player_tick, tgt.hit.source_eye.player_frac, tgt.hit.source_eye.lerp_ticks_int, tgt.hit.source_eye.lerp_ticks_frac );

				entry->set_player_tick_count( stamp_tick );
				entry->set_player_tick_fraction( stamp_frac );
			}

			if ( entry->has_sv_interp0( ) )
			{
				const auto interp = entry->mutable_sv_interp0( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_sv_interp1( ) )
			{
				const auto interp = entry->mutable_sv_interp1( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_cl_interp( ) )
			{
				const auto interp = entry->mutable_cl_interp( );
				interp->set_frac( 0.0f );
			}

		}

		const auto is_secondary = tgt.hit.attack_type == 1;
		const auto attack_button = is_secondary
			? cstypes::command_buttons::in_second_attack
			: cstypes::command_buttons::in_attack;

		cmd->buttons.value |= attack_button;
		cmd->buttons.value_changed |= attack_button;
		cmd->buttons.value_scroll |= attack_button;

		if ( history_index >= 0 )
		{
			if ( is_secondary )
			{
				cmd->csgo_user_cmd.set_attack2_start_history_index( history_index );
			}
			else
			{
				cmd->csgo_user_cmd.set_attack1_start_history_index( history_index );
			}
		}

		if ( const auto angles = base->mutable_viewangles( ) )
		{
			angles->set_x( tgt.hit.aim_angle.x );
			angles->set_y( tgt.hit.aim_angle.y );
		}
	}

	std::vector<math::vector3> rage::generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& bone_rot, float pointscale, const math::vector3& shoot_pos, float inaccuracy ) const
	{
		std::vector<math::vector3> out;

		auto scale = std::clamp( pointscale / 100.0f, 0.0f, 1.0f );
		if ( scale <= 0.01f )
		{
			return out;
		}

		const auto hb_mid   = ( hitbox.mins + hitbox.maxs ) * 0.5f;
		const auto capsule_a = center + bone_rot.rotate_vector( hitbox.mins - hb_mid );
		const auto capsule_b = center + bone_rot.rotate_vector( hitbox.maxs - hb_mid );

		// Keep points inside the part of the hitbox reachable by the full spread cone.
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( config.dynamic_pointscale.value && hitbox.radius > 0.001f )
		{
			const auto cone = std::max( inaccuracy + g_shared.ctx( ).spread, 0.0f );
			const auto cone_radius = std::tanf( cone ) * ( center - shoot_pos ).length( );
			const auto automatic_scale = std::clamp( 0.9f - cone_radius / hitbox.radius, 0.0f, 1.0f );
			scale = std::min( scale, automatic_scale );

			if ( scale <= 0.01f )
			{
				return out;
			}
		}

		// Build a view-relative frame so points rotate correctly above and below us.
		const auto shoot_dir = ( center - shoot_pos ).normalized( );
		const auto ang       = math::helpers::vector_to_angle( shoot_dir );

		math::vector3 left{}, up{};
		math::helpers::angle_vectors_left( ang, nullptr, &left, &up );

		// angle_vectors_left returns left, so negate it for right.
		const auto right = math::vector3{ -left.x, -left.y, -left.z };

		// Trace from outside through the center to find the real capsule surface.
		// This is accurate around rounded end caps, where radius offsets are not.
		const auto surface_point = [ & ]( const math::vector3& direction ) -> math::vector3
		{
			const auto dir = direction.normalized( );

			if ( hitbox.radius > 0.001f )
			{
				const auto reach = ( capsule_b - capsule_a ).length( ) + hitbox.radius * 2.0f + 1.0f;
				const auto origin = center + dir * reach;
				const auto delta = dir * ( reach * -2.0f );
				auto fraction{ 1.0f };

				if ( g_shared.ray_vs_capsule( origin, delta, capsule_a, capsule_b, hitbox.radius, fraction ) )
				{
					return origin + delta * fraction;
				}
			}
			else
			{
				// Intersect box hitboxes in bone space using their directional support.
				auto inverse = bone_rot;
				inverse.x = -inverse.x;
				inverse.y = -inverse.y;
				inverse.z = -inverse.z;
				const auto local_dir = inverse.rotate_vector( dir );
				const auto extents = ( hitbox.maxs - hitbox.mins ) * 0.5f;
				auto distance = 8192.0f;

				if ( std::fabs( local_dir.x ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.x / local_dir.x ) );
				if ( std::fabs( local_dir.y ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.y / local_dir.y ) );
				if ( std::fabs( local_dir.z ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.z / local_dir.z ) );

				if ( distance < 8192.0f )
				{
					return center + dir * distance;
				}
			}

			return center;
		};

		const auto scaled_surface = [ & ]( const math::vector3& direction )
		{
			const auto surface = surface_point( direction );
			return center + ( surface - center ) * scale;
		};

		switch ( hitbox.index )
		{
		case 0: // head
		{
			out.reserve( 4 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			out.push_back( scaled_surface( up ) );
			out.push_back( scaled_surface( -up ) );
			break;
		}

		case 2: case 3: // stomach / pelvis
		{
			out.reserve( 2 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			break;
		}

		case 4: case 5: case 6: // chest
		{
			out.reserve( 3 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			if ( hitbox.index == 6 )
			{
				out.push_back( scaled_surface( up ) );
			}
			break;
		}

		case 7: case 8: case 9: case 10: case 11: case 12: // legs / feet
		{
			out.reserve( 2 );
			out.push_back( capsule_a );
			out.push_back( capsule_b );
			break;
		}

		case 13: case 14: case 15: case 16: case 17: case 18: // arms
		{
			out.reserve( 1 );
			out.push_back( capsule_b );
			break;
		}

		default:
		{
			out.reserve( 2 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			break;
		}
		}

		return out;
	}

	bool rage::should_stop_movement( const aim_context& ctx ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto& prestate = systems::g_prediction.pre( );
		const auto velocity = prestate.networked_velocity;

		if ( shared_ctx.weapon_type == cstypes::weapon_type::sniper && !ctx.is_scoped )
		{
			return false;
		}

		if ( ctx.on_ground )
		{
			const auto speed_2d = velocity.length_2d( );
			if ( speed_2d <= 0.1f )
			{
				return false;
			}

			const auto inaccuracy_move = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyMove"_hash ) );
			const auto inaccuracy_stand = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyStand"_hash ) );

			return speed_2d * inaccuracy_move > inaccuracy_stand;
		}

		// Stopping in the air is jump scout's job, and only the scout is accurate enough up there for it to pay.
		if ( !g_misc.jumpscout( ).active_this_tick( ) )
		{
			return false;
		}

		if ( velocity.z > 140.0f )
		{
			return false;
		}

		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_friction = CONVAR ("sv_friction")->get<float>( );
		const auto sv_stopspeed = CONVAR ("sv_stopspeed")->get<float>( );

		const auto inac_jump_initial = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpInitial"_hash ) );
		const auto inac_jump_apex = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpApex"_hash ) );
		const auto shootable_threshold = inac_jump_apex + 0.001f;
		const auto early_threshold = inac_jump_initial * 0.55f + inac_jump_apex * 0.45f;
		const auto air_inaccuracy = g_shared.get_air_inaccuracy( velocity.z, inac_jump_initial, inac_jump_apex );

		if ( air_inaccuracy <= shootable_threshold || air_inaccuracy <= early_threshold )
		{
			return true;
		}

		auto sim_vz = velocity.z;
		auto ticks_to_shootable{ 0 };

		for ( auto i = 1; i <= 32; ++i )
		{
			sim_vz -= sv_gravity * cstypes::tick_interval;

			if ( g_shared.get_air_inaccuracy( sim_vz, inac_jump_initial, inac_jump_apex ) <= shootable_threshold )
			{
				ticks_to_shootable = i;
				break;
			}
		}

		if ( ticks_to_shootable == 0 )
		{
			return false;
		}

		const auto speed_2d = velocity.length_2d( );
		const auto max_speed = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) );
		const auto accurate_threshold = max_speed * 0.34f;

		if ( speed_2d <= accurate_threshold )
		{
			return true;
		}

		auto sim_speed = speed_2d;
		auto ticks_to_stop{ 32 };

		for ( auto i = 1; i <= 32; ++i )
		{
			const auto drop = std::fmaxf( sim_speed, sv_stopspeed ) * sv_friction * cstypes::tick_interval;
			sim_speed -= drop;

			if ( sim_speed <= accurate_threshold )
			{
				ticks_to_stop = i;
				break;
			}
		}

		return ticks_to_shootable <= ticks_to_stop + 2;
	}

	float rage::get_min_damage( const settings::combat::ragebot::weapon_group& config, int target_health, bool override_active ) const
	{
		if ( override_active )
		{
			return static_cast< float >( config.min_damage_override_value );
		}

		const auto base = static_cast< float >( config.min_damage );

		const auto hp = static_cast< float >( target_health );
		if ( hp < base )
		{
			return hp + 1.0f;
		}

		return base;
	}

	float rage::get_knife_damage( float raw, int armor, float armor_ratio ) const
	{
		if ( armor <= 0 )
		{
			return raw;
		}

		const auto ratio = armor_ratio * 0.5f;
		auto damage_to_health = raw * ratio;
		const auto damage_to_armor = ( raw - damage_to_health ) * 0.5f;

		if ( damage_to_armor > static_cast< float >( armor ) )
		{
			damage_to_health = raw - static_cast< float >( armor ) * 2.0f;
		}

		return std::max( 0.0f, std::floorf( damage_to_health ) );
	}

	systems::tracing::result rage::trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * range;
		const int filter_extras[ ]{ 0, 15 };

		for ( const auto extra : filter_extras )
		{
			const auto filter = extra == 0 ? systems::g_tracing.make_filter( local_pawn, 0x001c1003, 4 ) : systems::g_tracing.make_filter( local_pawn, 0x001c1003, 4, 15 );
			auto result = systems::g_tracing.trace( origin, end, filter );

			if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
			{
				return result;
			}

			for ( auto radius = 2.0f; radius <= 4.0f; radius += 2.0f )
			{
				const auto sweep_end = end - forward * radius;
				result = systems::g_tracing.trace_sphere( origin, sweep_end, radius, filter );

				if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
				{
					return result;
				}
			}
		}

		systems::tracing::result miss{};
		miss.fraction = 1.0f;
		miss.hit_entity = 0;
		return miss;
	}

	systems::tracing::result rage::trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * reach;
		const auto knife_filter = systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4 );
		auto result = systems::g_tracing.trace( origin, end, knife_filter );

		if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
		{
			return result;
		}

		const auto weapon_filter = systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4, 15 );
		result = systems::g_tracing.trace( origin, end, weapon_filter );

		if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
		{
			return result;
		}

		for ( auto radius = 14.0f; radius > 0.0f; radius -= 3.0f )
		{
			const auto sweep_end = end - forward * radius;
			result = systems::g_tracing.trace_sphere( origin, sweep_end, radius, weapon_filter );

			if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
			{
				return result;
			}
		}

		result.fraction = 1.0f;
		result.hit_entity = 0;
		return result;
	}

	void rage::update_penetration_crosshair( const systems::local::snapshot& local )
	{
		const auto& cfg = settings::g_combat.m_penetration_crosshair;
		const auto& ctx = g_shared.ctx( );

		if ( !cfg.enabled.value || !ctx.valid || !local.is_alive || local.team < 2
			|| !local.pawn || !ctx.weapon
			|| ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			this->m_penetration_crosshair_state.store( penetration_crosshair_state::unavailable, std::memory_order_relaxed );
			return;
		}

		// get_shoot_position() can be zero before prediction has populated the
		// weapon-services shoot history. The crosshair needs the current eye now.
		const auto eye_pos = g_shared.get_eye_position( local.pawn );
		auto view_angles = systems::g_input.get_view_angles( );
		const auto aim_punch = g_shared.get_aim_punch( local.pawn );
		view_angles.x += aim_punch.x;
		view_angles.y += aim_punch.y;

		math::vector3 forward{};
		math::helpers::angle_vectors_left( view_angles, &forward );

		auto pen_damage{ 0.0f };
		const auto can_pen = g_shared.pen( ).can( eye_pos, forward, pen_damage, local );
		this->m_penetration_crosshair_state.store(
			can_pen ? penetration_crosshair_state::penetrable : penetration_crosshair_state::blocked,
			std::memory_order_relaxed );
	}

	void rage::draw_penetration_crosshair( xdraw::draw_list& draw_list ) const
	{
		const auto& cfg = settings::g_combat.m_penetration_crosshair;
		if ( !cfg.enabled.value )
		{
			return;
		}

		const auto state = this->m_penetration_crosshair_state.load( std::memory_order_relaxed );
		const auto local = systems::g_local.get( );
		if ( state == penetration_crosshair_state::unavailable || !local.is_alive || systems::g_local.is_in_cinematic( ) )
		{
			return;
		}

		const auto can_pen = state == penetration_crosshair_state::penetrable;

		const auto& fill = can_pen ? cfg.can_penetrate_fill : cfg.blocked_fill;
		const auto& outline = can_pen ? cfg.can_penetrate_outline : cfg.blocked_outline;
		const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
		const auto cx = std::floorf( static_cast< float >( screen_w ) * 0.5f );
		const auto cy = std::floorf( static_cast< float >( screen_h ) * 0.5f );
		constexpr auto half_size{ 3.0f };
		constexpr auto outline_size{ 1.0f };

		if ( cfg.glow )
		{
			auto& glow = xdraw::get_glow( );
			const auto glow_a = static_cast< std::uint8_t >( static_cast< float >( outline.value.a ) * cfg.glow_strength );
			const auto glow_col = xdraw::color{ outline.value.r, outline.value.g, outline.value.b, glow_a };

			glow.rect_filled( cx - half_size - outline_size, cy - half_size - outline_size,
				( half_size + outline_size ) * 2.0f, ( half_size + outline_size ) * 2.0f, glow_col );
		}

		draw_list.rect_filled( cx - half_size - outline_size, cy - half_size - outline_size,
			( half_size + outline_size ) * 2.0f, ( half_size + outline_size ) * 2.0f, outline );
		draw_list.rect_filled( cx - half_size, cy - half_size, half_size * 2.0f, half_size * 2.0f, fill );
	}

} // namespace features::combat
