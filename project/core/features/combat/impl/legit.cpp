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
		// Disarmed up front; only a tick whose scan still finds the target re-arms it. Every early return
		// below therefore stops the per-frame aim as well.
		this->m_track.active = false;
		this->m_wants_stop = false;

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

		if ( settings::g_combat.m_legitbot.visualize_aimbot.value )
		{
			this->update_wallbang_preview( config, local );
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

		const auto rcs_scale = config.rcs.value ? static_cast< float >( std::clamp( config.rcs_strength.value, 0, 100 ) ) / 100.0f : 0.0f;

		// Where the next bullet leaves before spread: the crosshair plus whatever recoil the compensation
		// leaves in. At 100% that is the crosshair itself.
		auto detection_angles = view_angles;
		if ( config.rcs.value && aim_punch.length_sqr( ) > 0.0001f )
		{
			detection_angles.x += aim_punch.x * ( 1.0f - rcs_scale );
			detection_angles.y += aim_punch.y * ( 1.0f - rcs_scale );
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
					this->arm_tracking( this->m_target, config );
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

		// About once a second while the key is held: did the scan find anyone, did it arm, did the frames steer.
		if ( config.aimbot.value && this->m_diag_ticks++ % 64 == 0 )
		{
			logging::console::print( xs( "[legit] aim held: target={} hc={:.2f} armed={} steered_frames={}\n" ),
				this->m_target.has_target( ), this->m_target.hitchance, this->m_track.active, this->m_diag_steered );
			this->m_diag_steered = 0;
		}

		if ( config.triggerbot.value && g_shared.can_shoot( cmd, local.controller ) )
		{
			this->apply_triggerbot( cmd, shoot_position, view_angles, aim_punch, rcs_scale, config, local );
		}

		// Last, so a press the triggerbot just added is compensated like the player's own. Not behind the
		// can_shoot gate: that compares the command's client tick with the next-attack tick, and the client
		// tick can trail the tick the server fires on, so mid-spray the correction kept landing on the
		// command after the bullet instead of the one carrying it. Every held-attack command is compensated.
		this->apply_recoil_control( cmd, rcs_scale, local );
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
			this->draw_fov( draw_list, config.fov.value, config.fov_color );
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
			this->m_track.active = false;

			const std::scoped_lock lock{ this->m_preview_mtx };
			this->m_preview.clear( );
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

			const auto sticky_bone = this->m_track.pawn == pawn ? this->m_track.bone_index : -1;

			for ( const auto record : records )
			{
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto candidate = this->scan_player( pawn, record, hitbox_set, shoot_position, view_angles, config, local, sticky_bone );
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
				best.controller = p.ptr;
				best.pawn = pawn;
				best.aim_angle = aim;
				best.hitchance = 1.0f;
				best.score = score;
				best.fov = fov;
				best.health = health;
				best.record = chosen;
				best.best_point = point;
				best.newest = chosen == records[ 0 ];
			}
		}

		return best;
	}

	legit::scan_point legit::scan_player( std::uintptr_t pawn, shared::lagcomp::record* record, const systems::hitboxes::set& hitboxes, const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local, int sticky_bone ) const
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
		scan_point sticky{};

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

			if ( candidate.bone_index == sticky_bone && ( !sticky.valid || better_point( candidate, sticky ) ) )
			{
				sticky = candidate;
			}

			if ( !best.valid || better_point( candidate, best ) )
			{
				best = candidate;
			}
		}

		const auto rank = [ ]( const scan_point& p ) { return ( p.visible ? 2 : 0 ) + ( p.hitgroup == 1 ? 1 : 0 ); };
		if ( sticky.valid && rank( sticky ) >= rank( best ) )
		{
			return sticky;
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
		this->m_track = {};
	}

	void legit::arm_tracking( const target_result& tgt, const settings::combat::legitbot::weapon_group& config )
	{
		// Nothing moves until the reaction delay for this engagement has run out. The aim used to begin
		// travelling on the same tick the target became valid, which no hand does.
		if ( !this->update_engagement( tgt.pawn, config ) )
		{
			return;
		}

		auto& t = this->m_track;
		const auto& point = tgt.best_point;

		// A new pawn or bone is a new destination. The motion term in on_frame_input must not read the jump
		// from the old point to the new one as the target moving.
		if ( t.pawn != tgt.pawn || t.bone_index != point.bone_index || t.live != tgt.newest )
		{
			t.has_prev = false;
		}

		t.controller = tgt.controller;
		t.pawn = tgt.pawn;
		t.bone_index = point.bone_index;
		t.has_hitbox = point.hitbox.index >= 0;
		t.local_center = t.has_hitbox ? ( point.hitbox.mins + point.hitbox.maxs ) * 0.5f : math::vector3{};
		t.fixed_point = point.position;
		t.live = tgt.newest;
		t.smooth = config.smooth.value;
		t.refreshed = std::chrono::steady_clock::now( );
		t.active = true;
	}

	void legit::on_frame_input( std::uintptr_t csgo_input, int slot, float frametime )
	{
		auto& t = this->m_track;

		const auto disarm = [ & ]
			{
				t.active = false;
				t.has_prev = false;
			};

		if ( slot != 0 || !t.active )
		{
			t.has_prev = false;
			return;
		}

		// Armed by the last create_move. A few frames between ticks are normal; longer means the game stopped
		// running commands (alt-tab, round end) and the target is no longer vetted.
		if ( std::chrono::steady_clock::now( ) - t.refreshed > std::chrono::milliseconds( 60 ) )
		{
			disarm( );
			return;
		}

		const auto& ctx = g_shared.ctx( );
		if ( !ctx.valid || !settings::g_combat.m_legitbot.enabled.value || !settings::g_combat.m_legitbot.get_group( ctx.weapon_type ).aimbot.value )
		{
			disarm( );
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn )
		{
			disarm( );
			return;
		}

		// The cache only holds controllers -- checking the pawn against it failed every frame and the aim
		// never moved. A controller still cached, still alive and still owning this pawn means the target
		// is the one the last scan vetted.
		const auto target_alive = systems::g_entities.exists( t.controller )
			&& memory::safe_read<bool>( t.controller + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ).value_or( false )
			&& systems::g_entities.lookup( memory::safe_read<std::uint32_t>( t.controller + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) ).value_or( 0 ) ) == t.pawn;
		if ( !target_alive )
		{
			disarm( );
			return;
		}

		auto point = t.fixed_point;
		if ( t.live )
		{
			// The bone as it is rendered this frame. The record the scan used changes once per tick, and
			// chasing it is what made the aim step instead of glide.
			const auto bone = systems::g_bones.get( t.pawn, static_cast< std::uint32_t >( t.bone_index ) );
			if ( bone.position.length_sqr( ) < 1.0f )
			{
				disarm( );
				return;
			}

			point = t.has_hitbox ? bone.rotation.rotate_vector( t.local_center ) + bone.position : bone.position;
		}

		const auto scene = memory::safe_read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
		if ( !scene )
		{
			return;
		}

		const auto origin = memory::safe_read<math::vector3>( scene + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto view_offset = memory::safe_read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
		if ( !origin.has_value( ) || !view_offset.has_value( ) )
		{
			return;
		}

		auto desired = math::helpers::calculate_angle( *origin + *view_offset, point );

		// Offset the destination, not the step, so the aim settles beside the hitbox centre and stays there.
		desired.x += this->m_error_x;
		desired.y += this->m_error_y;
		math::helpers::normalize_angles( desired );

		auto& va_pitch = *reinterpret_cast< float* >( csgo_input + 1672 );
		auto& va_yaw = *reinterpret_cast< float* >( csgo_input + 1676 );

		auto delta = desired - math::vector3{ va_pitch, va_yaw, 0.0f };
		math::helpers::normalize_angles( delta );

		auto move = delta;

		if ( t.smooth > 1 )
		{
			// The slider keeps its meaning: smooth N closes 1/N of the remaining angle per 64 Hz tick. Here the
			// same decay is spread over the frames inside a tick, so the motion is continuous at any frame rate.
			// The per-step random scaling and overshoot that used to sit here are gone; they were the shake.
			const auto dt = std::clamp( frametime, 0.0f, 0.1f );
			const auto keep_per_tick = 1.0f - 1.0f / static_cast< float >( t.smooth );
			auto follow = 1.0f - std::pow( keep_per_tick, dt / cstypes::tick_interval );

			// Ease in over the first three ticks of an engagement instead of starting at full speed.
			const auto windup_t = std::clamp( static_cast< float >( this->m_engage_ticks - 1 ) / 3.0f, 0.0f, 1.0f );
			follow *= 0.45f + 0.55f * windup_t;

			move = delta * follow;

			// The decay alone always trails a moving target by a fixed angle. Adding the target's own motion since
			// the last frame removes that lag, so a strafing head stays under the crosshair instead of being chased.
			if ( t.has_prev )
			{
				auto motion = desired - t.prev_desired;
				math::helpers::normalize_angles( motion );

				if ( std::fabsf( motion.x ) < 5.0f && std::fabsf( motion.y ) < 5.0f )
				{
					move = move + motion * ( 1.0f - follow );
				}
			}
		}

		t.prev_desired = desired;
		t.has_prev = true;

		// Whole mouse counts at the player's sensitivity, the remainder carried into the next frame, so the view
		// only ever moves by amounts a mouse could have produced.
		const auto sensitivity = CONVAR ("sensitivity")->get<float>( );
		const auto fov_adjust = memory::safe_read<float>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_hash ) ).value_or( 1.0f );
		const auto deg_per_count = sensitivity * 0.022f * fov_adjust;

		auto step_x = move.x;
		auto step_y = move.y;

		if ( deg_per_count > 0.0001f )
		{
			const auto want_x = move.x + this->m_remainder_x;
			const auto want_y = move.y + this->m_remainder_y;

			step_x = std::roundf( want_x / deg_per_count ) * deg_per_count;
			step_y = std::roundf( want_y / deg_per_count ) * deg_per_count;

			this->m_remainder_x = want_x - step_x;
			this->m_remainder_y = want_y - step_y;
		}

		va_pitch = std::clamp( va_pitch + step_x, -89.0f, 89.0f );
		va_yaw += step_y;
		++this->m_diag_steered;
	}

	void legit::apply_triggerbot( systems::input::usercmd* cmd, const math::vector3& shoot_position, const math::vector3& view_angles, const math::vector3& aim_punch, float rcs_scale, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local )
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

			// Stay stopped for the length of the click, so the shot that just went out isn't followed by a step.
			this->m_wants_stop = config.trigger_autostop.value;
			return;
		}

		// Where the bullet actually leaves: the crosshair plus the recoil apply_recoil_control leaves in. This used
		// to add the full punch exactly when recoil control was on, which tested the spot the compensation was
		// about to move the shot away from.
		auto corrected_angles = view_angles;
		if ( aim_punch.length_sqr( ) > 0.0001f )
		{
			corrected_angles.x += aim_punch.x * ( 1.0f - rcs_scale );
			corrected_angles.y += aim_punch.y * ( 1.0f - rcs_scale );
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
					// The crosshair has to be on the player, not near them. This used to accept any hitbox centre within
					// 2 degrees, so the delay started while the crosshair was still sliding onto the target and had
					// usually run out by the time it got there -- which is why every delay setting felt instant.
					math::vector3 forward{};
					math::helpers::angle_vectors_left( corrected_angles, &forward );

					const systems::hitboxes::entry* closest_hb{ nullptr };
					auto closest_fraction{ FLT_MAX };

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

						const auto capsule_start = bone.rotation.rotate_vector( entry.mins ) + bone.position;
						const auto capsule_end = bone.rotation.rotate_vector( entry.maxs ) + bone.position;
						const auto radius = entry.radius > 0.0f ? entry.radius : 1.8f;

						auto fraction{ 1.0f };
						if ( !g_shared.ray_vs_capsule( shoot_position, forward * ctx.range, capsule_start, capsule_end, radius, fraction ) )
						{
							continue;
						}

						if ( fraction < closest_fraction )
						{
							closest_fraction = fraction;
							closest_hb = &entry;
						}
					}

					if ( !closest_hb )
					{
						continue;
					}

					// A few units past the capsule surface, so the penetration trace ends inside the hitbox rather than
					// exactly on its edge.
					const auto target_point = shoot_position + forward * ( ctx.range * closest_fraction + 4.0f );

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

		// From the first tick on target, so the stop runs alongside the reaction delay rather than after it.
		// Auto stop's second pass (after the legitbot) brakes on this same command.
		this->m_wants_stop = config.trigger_autostop.value;

		// The delay runs from the first tick the crosshair is on this player and is not restarted by anything
		// below: a hit chance that dips mid-delay holds the shot, it doesn't send the wait back to zero. Seed mode
		// used to skip the delay entirely.
		if ( this->m_trigger_pending_pawn != hit_pawn )
		{
			this->m_trigger_pending_pawn = hit_pawn;
			this->m_trigger_delay_start = ctx.current_time;

			const auto mode = std::clamp( config.trigger_mode.value, 0, 2 );
			const auto [range_min, range_max] = settings::combat::legitbot::k_trigger_delay_range[ mode ];
			const auto base = static_cast< float >( std::clamp( config.trigger_delay( ).value, range_min, range_max ) );

			// Legit and semi vary each reaction a little (about 15% and 8%); blatant is exact.
			const auto spread = mode == 0 ? 0.15f : mode == 1 ? 0.08f : 0.0f;
			this->m_trigger_delay_ms = spread > 0.0f && base > 0.0f
				? random::normal_clamped( base, base * spread, base * ( 1.0f - 2.0f * spread ), base * ( 1.0f + 2.0f * spread ) )
				: base;
		}

		if ( ( ctx.current_time - this->m_trigger_delay_start ) * 1000.0f < this->m_trigger_delay_ms )
		{
			return;
		}

		// Hold the shot until the stop has brought the speed into the accurate range: a third of the weapon's
		// max speed, the same line the game draws between standing and moving accuracy. Only on the ground --
		// in the air there is nothing to stop against, and waiting would just never fire.
		if ( config.trigger_autostop.value )
		{
			const auto& prestate = systems::g_prediction.pre( );
			const auto on_ground = ( prestate.flags & cstypes::entity_flags::on_ground ) != 0;
			if ( on_ground && ctx.weapon_max_speed > 0.0f && prestate.networked_velocity.length_2d( ) > ctx.weapon_max_speed * 0.34f )
			{
				return;
			}
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
					return;
				}
			}
		}

		g_shared.last_shoot_tick( ) = memory::read<std::int32_t>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );

		const auto record_time = cstypes::tick_fraction::from_value( hit_record->simulation_time / cstypes::tick_interval );
		const auto input_history_size = cmd->csgo_user_cmd.input_history_size( );
		// The plain view angles. The recoil is taken off afterwards by apply_recoil_control, at the strength the
		// detection above assumed. Writing the punched angles here (seed mode did) had the server add the punch
		// a second time.
		const auto history_angles = view_angles;

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

	void legit::apply_recoil_control( systems::input::usercmd* cmd, float rcs_scale, const systems::local::snapshot& local ) const
	{
		if ( rcs_scale <= 0.0f || !( cmd->buttons.value & cstypes::command_buttons::in_attack ) )
		{
			return;
		}

		// The ragebot writes its own fully compensated angles on the ticks it fires.
		if ( g_rage.is_firing_this_tick( ) )
		{
			return;
		}

		// Only the angles the server fires the bullet along are changed. The view angles the camera renders
		// from stay exactly where the player's mouse put them -- no pull-down, no shake. The old version
		// moved the view itself, by a factor re-rolled every tick, which is where the shaking came from.
		const auto render_punch = g_shared.get_aim_punch( local.pawn );
		const auto history_size = cmd->csgo_user_cmd.input_history_size( );

		for ( auto i = 0; i < history_size; ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( !entry )
			{
				continue;
			}

			// The fire code adds the punch as it stands at the shot's own time, a fraction of a tick away from
			// the one the camera shows while a spray is still kicking. Same fallback as the ragebot: if the two
			// disagree by degrees, the shot-time call has changed under us and the render value is safer.
			auto punch = g_shared.get_aim_punch_at( local.pawn, entry->player_tick_count( ), entry->player_tick_fraction( ) );
			const auto sane = std::isfinite( punch.x ) && std::isfinite( punch.y ) &&
				std::fabsf( punch.x - render_punch.x ) < 3.0f && std::fabsf( punch.y - render_punch.y ) < 3.0f;
			if ( !sane )
			{
				punch = render_punch;
			}

			if ( punch.length_sqr( ) < 0.000001f )
			{
				continue;
			}

			const auto angles = entry->mutable_view_angles( );
			if ( !angles )
			{
				continue;
			}

			angles->set_x( std::clamp( angles->x( ) - punch.x * rcs_scale, -89.0f, 89.0f ) );
			angles->set_y( std::remainderf( angles->y( ) - punch.y * rcs_scale, 360.0f ) );
		}
	}

	void legit::update_wallbang_preview( const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local )
	{
		// Two penetration runs per enemy per eye position adds up on a full server, and this is a colour cue:
		// 16 updates a second is plenty.
		if ( ++this->m_preview_counter % 4 != 0 )
		{
			return;
		}

		std::vector<preview_entry> next{};

		const auto publish = [ & ]
			{
				const std::scoped_lock lock{ this->m_preview_mtx };
				this->m_preview = std::move( next );
			};

		const auto local_scene = local.pawn ? memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ) : 0;
		if ( !local.is_alive || !local_scene )
		{
			publish( );
			return;
		}

		const auto eye = memory::read<math::vector3>( local_scene + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) ) +
			memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );

		auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
		velocity.z = 0.0f;

		// Where the current movement puts the eye over the next half second, stopped at the first wall. These
		// are the spots you are about to walk into, never ones behind geometry you can't pass through.
		std::array<math::vector3, 3> ahead{};
		auto ahead_count{ 0 };

		if ( velocity.length_sqr( ) > 30.0f * 30.0f )
		{
			const auto hull = math::vector3{ 8.0f, 8.0f, 8.0f };
			auto last = eye;

			for ( const auto seconds : { 0.15f, 0.3f, 0.5f } )
			{
				const auto wanted = eye + velocity * seconds;
				const auto tr = systems::g_tracing.trace_hull( eye, wanted, hull * -1.0f, hull, local.pawn );
				const auto reached = eye + ( wanted - eye ) * tr.fraction;

				if ( reached.distance_sqr( last ) > 4.0f * 4.0f )
				{
					ahead[ ahead_count++ ] = reached;
					last = reached;
				}

				if ( tr.fraction < 1.0f )
				{
					break;
				}
			}
		}

		const auto min_damage = static_cast< float >( std::max( config.min_damage.value, 1 ) );

		for ( const auto& p : systems::g_entities.get_by_type( systems::entities::type::player ) )
		{
			if ( !p.ptr || p.ptr == local.controller || !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
			{
				continue;
			}

			const auto pawn = systems::g_entities.lookup( memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) ) );
			if ( !pawn || pawn == local.pawn )
			{
				continue;
			}

			if ( !local.is_this_other_team( memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) ) ) ||
				memory::read<std::int32_t>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 )
			{
				continue;
			}

			const auto records = g_shared.lc( ).get_valid_records( pawn );
			if ( records.empty( ) || !records.front( ) || !records.front( )->valid )
			{
				continue;
			}

			const auto record = records.front( );
			const auto skeleton = g_shared.lc( ).get_skeleton( *record );
			const auto hitbox_set = systems::g_hitboxes.query( memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ) );

			// Head and upper chest: the two points a wallbang is usually taken at.
			std::array<math::vector3, 2> points{};
			auto point_count{ 0 };

			for ( const auto bone_id : { cstypes::bone_ids::head, cstypes::bone_ids::spine_3 } )
			{
				const auto& bone = skeleton[ bone_id ];
				if ( bone.position.length_sqr( ) < 1.0f )
				{
					continue;
				}

				const systems::hitboxes::entry* hb{ nullptr };
				for ( const auto& entry : hitbox_set )
				{
					if ( entry.bone == static_cast< int >( bone_id ) )
					{
						hb = &entry;
						break;
					}
				}

				points[ point_count++ ] = hb ? bone.rotation.rotate_vector( ( hb->mins + hb->maxs ) * 0.5f ) + bone.position : bone.position;
			}

			if ( point_count == 0 )
			{
				continue;
			}

			const auto pen_ctx = g_shared.pen( ).prepare_target( pawn, record );

			enum class reach : std::uint8_t { none, visible, wallbang };
			const auto test = [ & ]( const math::vector3& from )
				{
					auto out = reach::none;

					for ( auto i = 0; i < point_count; ++i )
					{
						shared::penetration::result pen{};
						if ( !g_shared.pen( ).run( from, points[ i ], pen_ctx, local.pawn, local.team, pen ) )
						{
							continue;
						}

						if ( !pen.penetrated )
						{
							return reach::visible;
						}

						if ( pen.damage >= min_damage )
						{
							out = reach::wallbang;
						}
					}

					return out;
				};

			// A target in plain sight is not a wallbang and keeps its normal esp colour.
			const auto now = test( eye );
			if ( now == reach::visible )
			{
				continue;
			}

			if ( now == reach::wallbang )
			{
				next.push_back( { pawn, wallbang_state::now } );
				continue;
			}

			for ( auto i = 0; i < ahead_count; ++i )
			{
				if ( test( ahead[ i ] ) == reach::wallbang )
				{
					next.push_back( { pawn, wallbang_state::soon } );
					break;
				}
			}
		}

		publish( );
	}

	legit::wallbang_state legit::wallbang_preview( std::uintptr_t pawn ) const
	{
		const std::scoped_lock lock{ this->m_preview_mtx };

		for ( const auto& entry : this->m_preview )
		{
			if ( entry.pawn == pawn )
			{
				return entry.state;
			}
		}

		return wallbang_state::none;
	}

	void legit::draw_fov( xdraw::draw_list& draw_list, float fov_degrees, const config::col& color ) const
	{
		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto sw = static_cast< float >( screen_w );
		const auto sh = static_cast< float >( screen_h );

		const auto camera_fov_rad = math::helpers::deg_to_rad( systems::g_view.fov( ) );
		const auto aimbot_fov_rad = math::helpers::deg_to_rad( fov_degrees );
		const auto radius = std::tanf( aimbot_fov_rad ) / std::tanf( camera_fov_rad * 0.5f ) * ( sw * 0.5f );

		// Centred on the crosshair. Recoil control no longer moves the view, so there is no punch offset to
		// chase -- the circle used to slide around during a spray for exactly that reason.
		const auto& c = color.value;
		draw_list.circle( sw * 0.5f, sh * 0.5f, radius, xdraw::color{ c.r, c.g, c.b, c.a }, 1.5f, 64 );
	}

} // namespace features::combat
