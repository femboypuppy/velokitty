#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/random/random.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	namespace {

		// True when the sight line from->to is buried in an active smoke cloud. The aim scan only
		// traces solid geometry, so without this a target standing in smoke reads as plainly
		// visible. We approximate each deployed smoke as a sphere: the projectile rests on the
		// floor and the cloud billows up and out from there, so the test sphere is lifted toward
		// the cloud centre and given the full ~144u fill radius. Closest-point-on-segment vs that
		// sphere is enough -- smoke is a soft volume, not something that needs a precise hull.
		[[nodiscard]] bool line_crosses_smoke( const math::vector3& from, const math::vector3& to )
		{
			constexpr auto k_smoke_radius = 144.0f;
			constexpr auto k_smoke_rise = 60.0f;

			for ( const auto& e : systems::g_entities.get_by_type( systems::entities::type::projectile ) )
			{
				if ( !e.ptr || e.schema_hash != "C_SmokeGrenadeProjectile"_hash )
				{
					continue;
				}

				const auto effect_begin = memory::read<int>( e.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin"_hash ) );
				const auto did_effect = memory::read<bool>( e.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect"_hash ) );
				if ( effect_begin <= 0 || !did_effect )
				{
					continue;
				}

				const auto scene = memory::read<std::uintptr_t>( e.ptr + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !scene )
				{
					continue;
				}

				auto center = memory::read<math::vector3>( scene + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
				center.z += k_smoke_rise;

				const auto seg = to - from;
				const auto len_sqr = seg.length_sqr( );
				auto t = 0.0f;
				if ( len_sqr > 0.0001f )
				{
					t = std::clamp( ( center - from ).dot( seg ) / len_sqr, 0.0f, 1.0f );
				}

				const auto closest = from + seg * t;
				if ( closest.distance_sqr( center ) <= k_smoke_radius * k_smoke_radius )
				{
					return true;
				}
			}

			return false;
		}

	} // namespace

	void legit::on_create_move( systems::input::usercmd* cmd )
	{
		if ( !settings::g_combat.m_legitbot.enabled.value )
		{
			return;
		}

		auto& ctx = g_shared.ctx( );
		if ( !ctx.valid )
		{
			return;
		}

		if ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			return;
		}

		const auto& config = settings::g_combat.m_legitbot.get_group( ctx.weapon_type );
		const auto view_angles = systems::g_input.get_view_angles( );
		const auto local = systems::g_local.get( );
		const auto aim_punch = g_shared.get_aim_punch( local.pawn );

		this->m_cached_view_angles = view_angles;
		this->m_cached_aim_punch = aim_punch;

		if ( config.standalone_rcs.value )
		{
			this->update_standalone_rcs( view_angles, aim_punch, config.standalone_rcs_strength.value, config.standalone_rcs_min.value, config.standalone_rcs_max.value, !config.aimbot.value, local );
		}

		this->m_target = {};

		if ( !g_shared.can_shoot( cmd, local.controller, false ) )
		{
			return;
		}

		auto shoot_position = g_shared.get_interpolated_shoot_position( local.pawn, false );
		ctx.spread = g_shared.get_spread( );
		ctx.inaccuracy = g_shared.get_inaccuracy( true );

		systems::g_prediction.simulate( cmd, local, [ & ]
			{
				shoot_position = g_shared.get_interpolated_shoot_position( local.pawn, false );

				ctx.spread = g_shared.get_spread( );
				ctx.inaccuracy = g_shared.get_inaccuracy( true );
			} );

		auto detection_angles = view_angles;
		if ( config.rcs.value && aim_punch.length_sqr( ) > 0.0001f )
		{
			detection_angles.x += aim_punch.x;
			detection_angles.y += aim_punch.y;
			math::helpers::normalize_angles( detection_angles );
		}

		if ( config.aimbot.value )
		{
			this->m_target = this->find_target( shoot_position, detection_angles, config, local );

			if ( this->m_target.has_target( ) )
			{
				this->m_engage_seen_time = ctx.current_time;

				// One pass, on the winner only, at a quarter of the default sample count -- this is a
				// percentage gate, not a trace, and the scan above would otherwise run it per hitbox
				// per backtrack record. Max accuracy short-circuits it the way the triggerbot does.
				const auto min_hitchance = static_cast< float >( config.aim_hitchance.value ) / 100.0f;
				const auto gate_active = min_hitchance > 0.0f && !g_shared.is_max_accuracy( ctx.inaccuracy );

				this->m_target.hitchance = gate_active
					? this->evaluate_hitchance( this->m_target, shoot_position, 64 )
					: 1.0f;

				// A failed gate skips the aim for this tick and leaves the engagement standing. Mid-spray
				// the hitchance collapses and recovers several times a second; re-rolling the reaction
				// delay on each dip would restart the wind-up and produce exactly the stutter this
				// section is meant to remove.
				if ( this->m_target.hitchance >= min_hitchance )
				{
					this->apply_aimbot( cmd, this->m_target, view_angles, aim_punch, config, local );
				}
			}
			else
			{
				// A target that blinks out for a tick -- a corner, a lag-comp gap, a smoke -- must not
				// re-roll the engagement, or the aim restarts its wind-up every time the enemy clips an
				// edge. A real disengage lasts longer than this.
				constexpr auto k_lost_grace{ 0.25f };

				if ( this->m_engage_pawn && ctx.current_time - this->m_engage_seen_time > k_lost_grace )
				{
					this->reset_engagement( );
				}
			}
		}
		else
		{
			// Key released. Pressing again is a new engagement and gets a new reaction time.
			this->reset_engagement( );
		}

		if ( !g_shared.can_shoot( cmd, local.controller ) )
		{
			return;
		}

		if ( config.triggerbot.value )
		{
			this->apply_triggerbot( cmd, shoot_position, view_angles, aim_punch, config, local );
		}
	}

	void legit::on_render( xdraw::draw_list& draw_list )
	{
		if ( !settings::g_combat.m_legitbot.enabled.value )
		{
			return;
		}

		const auto& ctx = g_shared.ctx( );
		if ( !ctx.valid )
		{
			return;
		}

		if ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			return;
		}

		const auto& config = settings::g_combat.m_legitbot.get_group( ctx.weapon_type );

		if ( config.visualize_fov.value )
		{
			this->draw_fov( draw_list, this->m_cached_view_angles, this->m_cached_aim_punch, config.fov.value, config.fov_color, config.rcs.value );
		}
	}

	void legit::invalidate_if_needed( )
	{
		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn || !local.controller )
		{
			this->m_trigger_release_time = 0.0f;
			this->m_trigger_pending_pawn = 0;
			this->m_trigger_delay_start = 0.0f;
		}
	}

	legit::target_result legit::find_target( const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const
	{
		target_result best{};

		for ( const auto& p : systems::g_entities.get_by_type( systems::entities::type::player ) )
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

			const auto team = memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) )
			{
				continue;
			}

			const auto health = memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			if ( health <= 0 )
			{
				continue;
			}

			if ( memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
			{
				continue;
			}

			const auto records = gather_records( pawn, 1 + std::clamp( config.backtrack_ticks.value, 0, 2 ) );
			if ( !records[ 0 ] )
			{
				continue;
			}

			// One hitbox query per player, reused across records: query walks the model's hitbox set,
			// and that set is a property of the model, not of any one snapshot of it.
			const auto game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );

			scan_point point{};
			shared::lagcomp::record* chosen{ nullptr };

			for ( const auto record : records )
			{
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto candidate = this->scan_player( pawn, record, hitbox_set, shoot_position, view_angles, config, local );
				if ( !candidate.valid )
				{
					continue;
				}

				if ( !chosen || better_point( candidate, point ) )
				{
					point = candidate;
					chosen = record;
				}
			}

			if ( !chosen )
			{
				continue;
			}

			const auto aim = math::helpers::calculate_angle( shoot_position, point.position );
			const auto fov = math::helpers::angle_distance( view_angles, aim );

			const auto can_kill = point.damage >= static_cast< float >( health );
			const auto best_can_kill = best.has_target( ) && best.best_point.damage >= static_cast< float >( best.health );

			auto score{ 1000.0f };

			if ( can_kill && !best_can_kill )
			{
				score += 10000.0f + point.damage;
			}
			else if ( can_kill == best_can_kill )
			{
				// Crosshair distance dominates, damage only breaks ties. The weights used to be the other
				// way round, which meant a body shot on someone across the map outscored a head shot on
				// the player already under the crosshair -- a ragebot's priority, not a legit aim's.
				score += ( 180.0f - fov ) * 100.0f + point.damage;
			}
			else
			{
				continue;
			}

			if ( !best.has_target( ) || score > best.score )
			{
				best.pawn = pawn;
				best.aim_angle = aim;
				best.hitchance = 1.0f;
				best.score = score;
				best.fov = fov;
				best.health = health;
				best.record = chosen;
				best.best_point = point;
			}
		}

		return best;
	}

	legit::scan_point legit::scan_player( std::uintptr_t pawn, shared::lagcomp::record* record, const systems::hitboxes::set& hitboxes, const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const
	{
		struct hitbox_entry
		{
			std::size_t cfg_index;
			std::uint32_t bone_id;
			int hitgroup;
		};

		constexpr std::array<hitbox_entry, 7> hitbox_map
		{ {
			{ 0, cstypes::bone_ids::head,             1 },
			{ 1, cstypes::bone_ids::spine_3,          2 },
			{ 2, cstypes::bone_ids::spine_2,          3 },
			{ 3, cstypes::bone_ids::left_shoulder,    4 },
			{ 3, cstypes::bone_ids::right_shoulder,   5 },
			{ 4, cstypes::bone_ids::left_knee,        6 },
			{ 4, cstypes::bone_ids::right_knee,       7 },
		} };

		const auto pen_ctx = g_shared.pen( ).prepare_target( pawn, record );
		const auto skeleton = g_shared.lc( ).get_skeleton( *record );

		scan_point best{};

		for ( const auto& [cfg_idx, bone_id, hitgroup] : hitbox_map )
		{
			if ( !config.hitboxes.values[ cfg_idx ] )
			{
				continue;
			}

			const auto bone_index = static_cast< std::size_t >( bone_id );
			if ( bone_index >= 27 )
			{
				continue;
			}

			const auto& bone = skeleton[ bone_index ];
			if ( bone.position.length_sqr( ) < 1.0f )
			{
				continue;
			}

			const systems::hitboxes::entry* hb{ nullptr };
			for ( const auto& entry : hitboxes )
			{
				if ( entry.bone == static_cast< int >( bone_id ) )
				{
					hb = &entry;
					break;
				}
			}

			// The bone origin is not the middle of the hitbox. The head bone sits high in the skull, so
			// aiming at it puts the shot above the head and every correction the smoothing makes is
			// chasing the wrong destination. This is the same capsule-centre expression the triggerbot
			// further down this file has always used.
			const auto aim_point = hb
				? bone.rotation.rotate_vector( ( hb->mins + hb->maxs ) * 0.5f ) + bone.position
				: bone.position;

			const auto aim = math::helpers::calculate_angle( shoot_position, aim_point );
			const auto fov = math::helpers::angle_distance( view_angles, aim );

			if ( fov > config.fov.value )
			{
				continue;
			}

			// A real player is blind through smoke; unless the user opts into aiming through it,
			// a cloud on the sight line disqualifies the point just like a wall would.
			if ( !config.aim_through_smoke.value && line_crosses_smoke( shoot_position, aim_point ) )
			{
				continue;
			}

			shared::penetration::result pen{};
			if ( !g_shared.pen( ).run( shoot_position, aim_point, pen_ctx, local.pawn, local.team, pen ) )
			{
				continue;
			}

			const auto visible = !pen.penetrated;
			if ( !visible && !config.autowall.value )
			{
				continue;
			}

			if ( !visible && pen.damage < static_cast< float >( config.min_damage.value ) )
			{
				continue;
			}

			scan_point candidate{};
			candidate.position = aim_point;
			candidate.damage = pen.damage;
			candidate.fov = fov;
			candidate.hitgroup = pen.hitgroup;
			candidate.cfg_index = cfg_idx;
			candidate.bone_index = static_cast< int >( bone_index );
			candidate.hitbox = hb ? *hb : systems::hitboxes::entry{};
			candidate.visible = visible;
			candidate.valid = true;

			if ( !best.valid || better_point( candidate, best ) )
			{
				best = candidate;
			}
		}

		return best;
	}

	bool legit::better_point( const scan_point& a, const scan_point& b )
	{
		// Visibility first, because a legit aim must never prefer a two-damage wallbang on a head over
		// a clean shot at a chest -- min_damage now defaults to 1, so occluded points are cheap to
		// qualify. Then the head outright: on a USP a penetrating chest hit ties a head hit on damage,
		// and the old damage-first ordering handed it the chest, which is the "it doesn't track the
		// head" complaint in one line. Damage and crosshair distance only break ties.
		const auto rank_a = ( a.visible ? 2 : 0 ) + ( a.hitgroup == 1 ? 1 : 0 );
		const auto rank_b = ( b.visible ? 2 : 0 ) + ( b.hitgroup == 1 ? 1 : 0 );

		if ( rank_a != rank_b )
		{
			return rank_a > rank_b;
		}

		if ( a.damage != b.damage )
		{
			return a.damage > b.damage;
		}

		return a.fov < b.fov;
	}

	std::array<shared::lagcomp::record*, 3> legit::gather_records( std::uintptr_t pawn, int max_records )
	{
		std::array<shared::lagcomp::record*, 3> out{ nullptr, nullptr, nullptr };

		const auto count = std::clamp( max_records, 1, 3 );

		const auto records = g_shared.lc( ).get_valid_records( pawn );
		if ( records.empty( ) || !records.front( ) || !records.front( )->valid )
		{
			return out;
		}

		out[ 0 ] = records.front( );

		if ( count < 2 )
		{
			return out;
		}

		if ( records.size( ) > 2 )
		{
			const auto mid = records.size( ) / 2;

			if ( records[ mid ] && records[ mid ]->valid )
			{
				out[ 1 ] = records[ mid ];
			}

			if ( count > 2 && records.back( ) && records.back( )->valid )
			{
				out[ 2 ] = records.back( );
			}
		}
		else if ( records.size( ) > 1 )
		{
			if ( records.back( ) && records.back( )->valid )
			{
				out[ 1 ] = records.back( );
			}
		}

		return out;
	}

	float legit::evaluate_hitchance( const target_result& tgt, const math::vector3& shoot_position, int samples ) const
	{
		// No hitbox means the point came off a raw bone origin with no capsule to test against. Fail
		// open: a gate that cannot measure must not be the thing that stops the aim.
		if ( !tgt.record || tgt.best_point.hitbox.index < 0 || tgt.best_point.bone_index < 0 || tgt.best_point.bone_index >= 27 )
		{
			return 1.0f;
		}

		const auto& ctx = g_shared.ctx( );
		const auto skeleton = g_shared.lc( ).get_skeleton( *tgt.record );

		return g_shared.calculate_hitchance( shoot_position, tgt.aim_angle, tgt.best_point.hitbox, skeleton[ tgt.best_point.bone_index ], ctx.inaccuracy, ctx.spread, samples );
	}

	bool legit::update_engagement( std::uintptr_t pawn, const settings::combat::legitbot::weapon_group& config )
	{
		const auto now = g_shared.ctx( ).current_time;

		if ( this->m_engage_pawn != pawn )
		{
			this->m_engage_pawn = pawn;
			this->m_engage_ticks = 0;
			this->m_remainder_x = 0.0f;
			this->m_remainder_y = 0.0f;

			// Spread around the configured value rather than using it flat. A hand does not react on a
			// metronome, and two engagements that start with the same delay are two engagements that
			// look like the same machine.
			const auto delay_ms = static_cast< float >( config.reaction_delay.value );
			const auto rolled = delay_ms > 0.0f
				? random::normal_clamped( delay_ms, delay_ms * 0.28f, delay_ms * 0.45f, delay_ms * 1.7f )
				: 0.0f;

			this->m_engage_ready_time = now + rolled / 1000.0f;

			// Held for the whole engagement, never re-centred. Gaussian so it clusters near the point
			// and clamped so it cannot wander off the hitbox. An aim that converges to exactly zero
			// error is the tell this exists to remove.
			const auto error = std::max( config.aim_error.value, 0.0f );
			this->m_error_x = error > 0.0f ? random::normal_clamped( 0.0f, error * 0.5f, -error, error ) : 0.0f;
			this->m_error_y = error > 0.0f ? random::normal_clamped( 0.0f, error * 0.5f, -error, error ) : 0.0f;
		}

		if ( now < this->m_engage_ready_time )
		{
			return false;
		}

		++this->m_engage_ticks;
		return true;
	}

	void legit::reset_engagement( )
	{
		this->m_engage_pawn = 0;
		this->m_engage_ready_time = 0.0f;
		this->m_engage_ticks = 0;
		this->m_error_x = 0.0f;
		this->m_error_y = 0.0f;
		this->m_remainder_x = 0.0f;
		this->m_remainder_y = 0.0f;
	}

	void legit::apply_aimbot( systems::input::usercmd* cmd, const target_result& tgt, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local )
	{
		// Nothing moves until the reaction delay for this engagement has run out. The aim used to begin
		// travelling on the same tick the target became valid, which no hand does.
		if ( !this->update_engagement( tgt.pawn, config ) )
		{
			return;
		}

		auto aim_angle = tgt.aim_angle;

		if ( config.rcs.value )
		{
			this->apply_rcs( aim_angle, aim_punch, config.rcs_min.value, config.rcs_max.value );
		}

		// Offset the destination, not the step, so the aim converges on a point beside the hitbox centre
		// and stays there. Offsetting the step would let the error average out to nothing over a few
		// ticks and put the crosshair back on the exact centre, which is the pattern being avoided.
		aim_angle.x += this->m_error_x;
		aim_angle.y += this->m_error_y;
		math::helpers::normalize_angles( aim_angle );

		if ( config.smooth.value > 0 )
		{
			auto delta = aim_angle - view_angles;
			math::helpers::normalize_angles( delta );

			const auto delta_length = std::sqrtf( delta.x * delta.x + delta.y * delta.y );
			if ( delta_length < 0.001f )
			{
				this->m_remainder_x = 0.0f;
				this->m_remainder_y = 0.0f;
				return;
			}

			// The old curve was inverted: at smooth = 5 it moved 6% of the remaining angle per tick at ten
			// degrees out and 20% at one degree, so it crawled hardest exactly when it was furthest away
			// and then snapped the last degree shut. In a duel you re-acquire a 5-15 degree delta every
			// tick and never leave the slow part, which is the delay being reported.
			//
			// This curve does the opposite, which is the shape a hand actually makes: wind up over the
			// first few ticks of an engagement, run fast while there is distance to cover, decelerate
			// into the point. The deceleration costs nothing to write -- every tick takes a fraction of
			// what remains, so the step shrinks on its own as the aim arrives.
			constexpr auto k_wide_angle{ 12.0f };     // degrees at which the travel term saturates
			constexpr auto k_close_scale{ 0.35f };    // travel term as the aim arrives
			constexpr auto k_windup_ticks{ 3.0f };    // ticks from first movement to full speed
			constexpr auto k_windup_floor{ 0.45f };
			constexpr auto k_min_scale{ 0.25f };      // floor, as a fraction of the configured speed

			const auto base_speed = 1.0f / static_cast< float >( config.smooth.value );

			const auto reach = std::clamp( delta_length / k_wide_angle, 0.0f, 1.0f );
			const auto travel = k_close_scale + ( 1.0f - k_close_scale ) * std::sqrtf( reach );

			const auto windup_t = std::clamp( static_cast< float >( this->m_engage_ticks - 1 ) / k_windup_ticks, 0.0f, 1.0f );
			const auto windup = k_windup_floor + ( 1.0f - k_windup_floor ) * windup_t;

			// The floor is a fraction of the configured speed, never an absolute step per tick. An
			// absolute floor would quietly override a high smooth value and make the slider stop meaning
			// anything past a certain point.
			auto smooth_factor = base_speed * std::max( travel * windup, k_min_scale );

			smooth_factor *= random::normal_clamped( 1.0f, 0.06f, 0.85f, 1.15f );
			smooth_factor = std::clamp( smooth_factor, 0.0f, 1.0f );

			const auto x_bias = random::normal_clamped( 1.0f, 0.02f, 0.95f, 1.05f );
			const auto y_bias = random::normal_clamped( 0.97f, 0.03f, 0.90f, 1.04f );

			auto move_x = delta.x * smooth_factor * x_bias;
			auto move_y = delta.y * smooth_factor * y_bias;

			if ( delta_length < 2.0f && delta_length > 0.3f && random::floating( 0.0f, 1.0f ) < 0.15f )
			{
				const auto overshoot = random::normal_clamped( 1.2f, 0.08f, 1.05f, 1.4f );
				move_x *= overshoot;
				move_y *= overshoot;
			}

			aim_angle = view_angles + math::vector3{ move_x, move_y, 0.0f };
			math::helpers::normalize_angles( aim_angle );
		}

		auto want_x = aim_angle.x - view_angles.x;
		auto want_y = aim_angle.y - view_angles.y;

		// Yaw wraps at +-180 and this subtraction does not. Looking at 179.5 with the aim landing on
		// -179.5 gives a raw difference of 359, and the mouse would be driven all the way round the long
		// way for what is a one-degree correction. Pitch has no seam, so only the yaw needs this.
		math::helpers::normalize_angle( want_y );

		want_x += this->m_remainder_x;
		want_y += this->m_remainder_y;

		const auto sensitivity = CONVAR ("sensitivity")->get<float>( );
		const auto fov_adjust = memory::read<float>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_hash ) );
		const auto deg_per_count = sensitivity * 0.022f * fov_adjust;

		const auto counts_x = std::roundf( want_x / deg_per_count );
		const auto counts_y = std::roundf( want_y / deg_per_count );

		this->m_remainder_x = want_x - counts_x * deg_per_count;
		this->m_remainder_y = want_y - counts_y * deg_per_count;

		systems::g_legit_input.add_mouse_delta( counts_x * deg_per_count, counts_y * deg_per_count );
	}

	void legit::apply_triggerbot( systems::input::usercmd* cmd, const math::vector3& shoot_position, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local )
	{
		const auto& ctx = g_shared.ctx( );
		const auto seed_mode = config.give_me_your_seed.value;

		if ( this->m_trigger_release_time > 0.0f )
		{
			if ( ctx.current_time >= this->m_trigger_release_time )
			{
				this->m_trigger_release_time = 0.0f;
				this->m_trigger_pending_pawn = 0;
				return;
			}

			cmd->buttons.value |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			return;
		}

		auto corrected_angles = view_angles;
		if ( config.rcs.value && aim_punch.length_sqr( ) > 0.0001f )
		{
			corrected_angles.x += aim_punch.x;
			corrected_angles.y += aim_punch.y;
			math::helpers::normalize_angles( corrected_angles );
		}

		math::vector3 bullet_dir{};
		std::uint32_t seed{};
		math::vector2 spread{};

		if ( seed_mode )
		{
			const auto tick_base = memory::read<std::int32_t>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
			seed = g_shared.get_spread_seed( corrected_angles, tick_base );
			spread = g_shared.calculate_spread( seed, ctx.inaccuracy, ctx.spread, ctx.recoil_index, ctx.item_def_idx, ctx.num_bullets );

			math::vector3 forward{}, left{}, up{};
			math::helpers::angle_vectors_left( corrected_angles, &forward, &left, &up );

			bullet_dir = ( forward + left * spread.x + up * spread.y ).normalized( );
		}

		auto hit_pawn{ 0ull };
		shared::penetration::result pen{};
		shared::lagcomp::record* hit_record{ nullptr };
		auto found{ false };

		// Record selection lives on the class now -- the aimbot's backtrack needs the same front/mid/back
		// window this used to build inline.

		for ( const auto& p : systems::g_entities.get_by_type( systems::entities::type::player ) )
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

			const auto team = memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) )
			{
				continue;
			}

			const auto health = memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			if ( health <= 0 )
			{
				continue;
			}

			if ( memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
			{
				continue;
			}

			const auto recs = gather_records( pawn, 3 );
			if ( !recs[ 0 ] )
			{
				continue;
			}

			const auto game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );

			for ( const auto rec : recs )
			{
				if ( !rec )
				{
					continue;
				}

				const auto skeleton = g_shared.lc( ).get_skeleton( *rec );

				if ( seed_mode )
				{
					for ( const auto& hb : hitbox_set )
					{
						if ( hb.bone < 0 || hb.bone >= 27 )
						{
							continue;
						}

						const auto& bone = skeleton[ hb.bone ];
						if ( bone.position.length_sqr( ) < 1.0f )
						{
							continue;
						}

						const auto capsule_start = bone.rotation.rotate_vector( hb.mins ) + bone.position;
						const auto capsule_end = bone.rotation.rotate_vector( hb.maxs ) + bone.position;
						const auto radius = hb.radius > 0.0f ? hb.radius * 0.9f : 1.8f;

						auto fraction{ 1.0f };
						if ( !g_shared.ray_vs_capsule( shoot_position, bullet_dir * ctx.range, capsule_start, capsule_end, radius, fraction ) )
						{
							continue;
						}

						const auto hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hb.index );

						if ( config.trigger_head_only.value && hitgroup != 1 )
						{
							continue;
						}

						const auto hit_point = shoot_position + bullet_dir * ctx.range * fraction;
						const auto pen_ctx = g_shared.pen( ).prepare_target( pawn, rec );

						shared::penetration::result candidate_pen{};
						if ( !g_shared.pen( ).run( shoot_position, hit_point, pen_ctx, local.pawn, local.team, candidate_pen ) )
						{
							continue;
						}

						if ( candidate_pen.penetrated && candidate_pen.damage < static_cast< float >( config.min_damage.value ) )
						{
							continue;
						}

						if ( !found || candidate_pen.damage > pen.damage )
						{
							hit_pawn = pawn;
							hit_record = rec;
							pen = candidate_pen;
							found = true;
						}

						break;
					}
				}
				else
				{
					const systems::hitboxes::entry* closest_hb{ nullptr };
					auto closest_fov{ FLT_MAX };

					for ( const auto& entry : hitbox_set )
					{
						if ( entry.bone < 0 || entry.bone >= 27 )
						{
							continue;
						}

						const auto& bone = skeleton[ entry.bone ];
						if ( bone.position.length_sqr( ) < 1.0f )
						{
							continue;
						}

						const auto center = bone.rotation.rotate_vector( ( entry.mins + entry.maxs ) * 0.5f ) + bone.position;
						const auto aim = math::helpers::calculate_angle( shoot_position, center );
						const auto fov = math::helpers::angle_distance( corrected_angles, aim );

						if ( fov > 2.0f )
						{
							continue;
						}

						if ( fov < closest_fov )
						{
							closest_fov = fov;
							closest_hb = &entry;
						}
					}

					if ( !closest_hb )
					{
						continue;
					}

					const auto& bone = skeleton[ closest_hb->bone ];
					const auto target_point = bone.rotation.rotate_vector( ( closest_hb->mins + closest_hb->maxs ) * 0.5f ) + bone.position;

					// Same smoke rule as the aim scan: don't let the triggerbot fire through a cloud
					// the player couldn't see through, unless aim-through-smoke is enabled.
					if ( !config.aim_through_smoke.value && line_crosses_smoke( shoot_position, target_point ) )
					{
						continue;
					}

					const auto pen_ctx = g_shared.pen( ).prepare_target( pawn, rec );

					shared::penetration::result candidate_pen{};
					if ( !g_shared.pen( ).run( shoot_position, target_point, pen_ctx, local.pawn, local.team, candidate_pen ) )
					{
						continue;
					}

					const auto visible = !candidate_pen.penetrated;
					if ( !visible )
					{
						if ( !config.autowall.value )
						{
							continue;
						}

						if ( candidate_pen.damage < static_cast< float >( config.min_damage.value ) )
						{
							continue;
						}
					}

					if ( !found || candidate_pen.damage > pen.damage )
					{
						hit_pawn = pawn;
						hit_record = rec;
						pen = candidate_pen;
						found = true;
					}
				}
			}
		}

		if ( !found )
		{
			this->m_trigger_pending_pawn = 0;
			return;
		}

		if ( config.trigger_head_only.value && pen.hitgroup != 1 )
		{
			this->m_trigger_pending_pawn = 0;
			return;
		}

		if ( pen.penetrated && pen.damage < static_cast< float >( config.min_damage.value ) )
		{
			this->m_trigger_pending_pawn = 0;
			return;
		}

		if ( !seed_mode )
		{
			const auto skeleton = g_shared.lc( ).get_skeleton( *hit_record );
			const auto game_scene_node = memory::read<std::uintptr_t>( hit_pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );

			const systems::hitboxes::entry* best_hb{ nullptr };

			for ( const auto& entry : hitbox_set )
			{
				if ( entry.index == pen.hitbox )
				{
					best_hb = &entry;
					break;
				}
			}

			if ( best_hb && best_hb->bone >= 0 && best_hb->bone < 28 )
			{
				hit_record->apply( );
				const auto hc = g_shared.calculate_hitchance( shoot_position, corrected_angles, *best_hb, skeleton[ best_hb->bone ], ctx.inaccuracy, ctx.spread );
				hit_record->restore( );

				const auto min_hc = static_cast< float >( config.trigger_hitchance.value ) / 100.0f;
				if ( hc < min_hc && !g_shared.is_max_accuracy( ctx.inaccuracy ) )
				{
					this->m_trigger_pending_pawn = 0;
					return;
				}
			}

			const auto delay_ms = static_cast< float >( config.trigger_delay.value );

			if ( this->m_trigger_pending_pawn != hit_pawn )
			{
				this->m_trigger_pending_pawn = hit_pawn;
				this->m_trigger_delay_start = ctx.current_time;
			}

			const auto elapsed_ms = ( ctx.current_time - this->m_trigger_delay_start ) * 1000.0f;
			if ( elapsed_ms < delay_ms )
			{
				return;
			}
		}

		g_shared.last_shoot_tick( ) = memory::read<std::int32_t>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );

		const auto record_time = cstypes::tick_fraction::from_value( hit_record->simulation_time / cstypes::tick_interval );
		const auto input_history_size = cmd->csgo_user_cmd.input_history_size( );
		const auto history_angles = seed_mode ? corrected_angles : math::vector3{ view_angles.x - aim_punch.x, view_angles.y - aim_punch.y, 0.0f };

		for ( auto i = 0; i < input_history_size; ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( !entry )
			{
				continue;
			}

			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( history_angles.x );
				angles->set_y( history_angles.y );
			}

			entry->set_render_tick_count( record_time.tick + 1 );
			entry->set_render_tick_fraction( 0.0f );

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

		cmd->buttons.value |= cstypes::command_buttons::in_attack;
		cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;

		if ( input_history_size > 0 )
		{
			cmd->csgo_user_cmd.set_attack1_start_history_index( input_history_size - 1 );
		}

		this->m_trigger_release_time = ctx.current_time + random::hold_duration( );
	}

	void legit::apply_rcs( math::vector3& aim_angle, const math::vector3& aim_punch, int rand_min, int rand_max ) const
	{
		if ( aim_punch.length_sqr( ) < 0.0001f )
		{
			return;
		}

		const auto factor = this->compute_rcs_factor( rand_min, rand_max );

		aim_angle.x -= aim_punch.x * factor;
		aim_angle.y -= aim_punch.y * factor;
		math::helpers::normalize_angles( aim_angle );
	}

	void legit::update_standalone_rcs( const math::vector3& view_angles, const math::vector3& aim_punch, int amount, int rand_min, int rand_max, bool apply, const systems::local::snapshot& local )
	{
		const auto shots_fired = memory::read<int>( local.pawn + SCHEMA( "C_CSPlayerPawn", "m_iShotsFired"_hash ) );
		if ( shots_fired > 1 )
		{
			const auto factor = this->compute_rcs_factor( rand_min, rand_max );
			const auto scale = static_cast< float >( amount ) / 100.0f;

			const auto punch_scaled = math::vector3
			{
				aim_punch.x * scale * factor,
				aim_punch.y * scale * factor,
				0.0f
			};

			if ( apply )
			{
				auto new_angles = view_angles;
				new_angles.x += this->m_old_punch.x - punch_scaled.x;
				new_angles.y += this->m_old_punch.y - punch_scaled.y;
				math::helpers::normalize_angles( new_angles );

				systems::g_input.set_view_angles( new_angles );
			}

			this->m_old_punch = punch_scaled;
		}
		else
		{
			this->m_old_punch = {};
		}
	}

	float legit::compute_rcs_factor( int rand_min, int rand_max ) const
	{
		const auto seed = static_cast< std::uint32_t >( g_shared.ctx( ).current_time * 1000.0f );
		const auto t = static_cast< float >( seed % 1000 ) / 1000.0f;
		const auto min_scale = static_cast< float >( rand_min ) / 100.0f;
		const auto max_scale = static_cast< float >( rand_max ) / 100.0f;
		return min_scale + ( max_scale - min_scale ) * t;
	}

	void legit::draw_fov( xdraw::draw_list& draw_list, const math::vector3& view_angles, const math::vector3& aim_punch, float fov_degrees, const config::col& color, bool rcs_active ) const
	{
		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto sw = static_cast< float >( screen_w );
		const auto sh = static_cast< float >( screen_h );

		const auto camera_fov_rad = math::helpers::deg_to_rad( systems::g_view.fov( ) );
		const auto aimbot_fov_rad = math::helpers::deg_to_rad( fov_degrees );
		const auto radius = std::tanf( aimbot_fov_rad ) / std::tanf( camera_fov_rad * 0.5f ) * ( sw * 0.5f );

		auto offset_x{ 0.0f };
		auto offset_y{ 0.0f };

		if ( rcs_active )
		{
			const auto punch_magnitude = aim_punch.length_sqr( );
			const auto current_time = g_shared.ctx( ).current_time;

			if ( punch_magnitude > 0.5f )
			{
				this->m_last_significant_punch_time = current_time;
			}

			const auto time_since = current_time - this->m_last_significant_punch_time;
			if ( time_since < 0.3f && punch_magnitude > 0.01f )
			{
				auto corrected = view_angles;
				corrected.x -= aim_punch.x;
				corrected.y -= aim_punch.y;
				math::helpers::normalize_angles( corrected );

				math::vector3 center_dir{}, corrected_dir{};
				math::helpers::angle_vectors_left( view_angles, &center_dir );
				math::helpers::angle_vectors_left( corrected, &corrected_dir );

				const auto render_origin = systems::g_frame_data.origin( );
				const auto cs = systems::g_view.project( render_origin + center_dir * 1000.0f );
				const auto ns = systems::g_view.project( render_origin + corrected_dir * 1000.0f );

				if ( systems::g_view.projection_valid( cs ) && systems::g_view.projection_valid( ns ) )
				{
					offset_x = ( cs.x - ns.x ) * 0.5f;
					offset_y = ( cs.y - ns.y ) * 0.5f;
				}
			}
		}

		const auto cx = sw * 0.5f + offset_x;
		const auto cy = sh * 0.5f + offset_y;
		const auto& c = color.value;

		draw_list.circle( cx, cy, radius, xdraw::color{ c.r, c.g, c.b, c.a }, 1.5f, 64 );
	}

} // namespace features::combat
