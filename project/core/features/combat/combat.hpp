#pragma once

#include <core/systems/systems.hpp>

namespace features::combat {

	class shared
	{
	public:
		class lagcomp
		{
		public:
			struct record
			{
				std::uintptr_t pawn{};
				std::uintptr_t game_scene_node{};
				std::uintptr_t bone_cache{};
				int bone_count{};

				systems::bones::data bones[ 128 ]{};
				systems::bones::data bones_backup[ 128 ]{};

				math::vector3 origin{};
				math::vector3 rotation{};

				// Networked eye yaw at the moment this record was taken. The resolver rotates the
				// recorded pose around this to guess where a desynced player's real hitbox sits;
				// nothing else reads it, so a build with the resolver off never touches it.
				float eye_yaw{};

				float simulation_time{};
				int tick{};
				bool valid{};
				bool was_valid{};
				bool is_applied{};
				bool extrapolated{};

				bool setup( std::uintptr_t pawn );
				[[nodiscard]] bool is_valid( ) const;

				void apply( );
				void restore( );
			};

			struct visual_record
			{
				math::vector3 origin{};
				std::array<systems::bones::data, 27> bones{};
			};

			struct extrapolation_data
			{
				math::vector3 origin{};
				math::vector3 velocity{};
				math::vector3 obb_mins{};
				math::vector3 obb_maxs{};
				std::uint32_t flags{};
				float sim_time{};
				float direction{};
			};

			void run( );
			void clear( );

			[[nodiscard]] record* get_oldest_valid( std::uintptr_t pawn );
			[[nodiscard]] record* get_oldest_was_valid( std::uintptr_t pawn );
			[[nodiscard]] std::optional<visual_record> get_oldest_was_valid_visual( std::uintptr_t pawn ) const;
			[[nodiscard]] std::vector<record*> get_valid_records( std::uintptr_t pawn );
			[[nodiscard]] std::array<systems::bones::data, 27> get_skeleton( const record& record ) const;

			[[nodiscard]] std::optional<record> extrapolate( std::uintptr_t pawn );

		private:
			void predict_movement( extrapolation_data& data, std::uintptr_t skip_entity ) const;

			std::unordered_map<std::uintptr_t, std::deque<record>> m_records{};
			mutable std::shared_mutex m_records_mtx{};
		};

		// Enemy anti-aim resolver. CS2 desync is not CSGO's LBY: from the client we cannot
		// measure how far the server's authoritative hitbox has rotated away from the pose we
		// recorded, so this is a brute-forcer rather than an oracle. Hypothesis 0 is always
		// "no correction" -- byte-for-byte the un-resolved aim path -- and we only start trying
		// rotated poses once a shot under the current hypothesis fails to draw blood. It can
		// never do worse than the resolver-off build unless a rotated guess actually connects.
		class resolver
		{
		public:
			// { 0, +desync, -desync, +2*desync, -2*desync }. Index 0 = no correction.
			static constexpr int k_hypothesis_count{ 5 };

			// Current hypothesis index for a pawn (0 if we have never missed it). Read-only and
			// insert-free, so the parallel scan workers may call it while single-threaded
			// create_move code owns all the writes.
			[[nodiscard]] int hypothesis( std::uintptr_t pawn ) const;

			// Yaw correction in degrees for a hypothesis index given the configured desync range.
			[[nodiscard]] static float yaw_delta( int hypo, float desync_range );

			// Yaw a copy of `src`'s pose about its pawn origin by `yaw_deg` (degrees, about Z).
			// The first bone_count bones have world position and orientation rotated; scale and
			// every other field are copied through, so the copy still names the same
			// game_scene_node and hitbox set. Shared by the resolver and safe-point evaluation.
			[[nodiscard]] static lagcomp::record rotate_pose( const lagcomp::record& src, float yaw_deg );

			// Record a committed shot so on_tick can later judge whether it landed. One in-flight
			// shot per pawn; a fresh shot supersedes the previous. Single-threaded (fire_gun).
			void on_fired( std::uintptr_t pawn, int hypo, int victim_health );

			// Advance one tick: a drop in a pending victim's health confirms its hypothesis and
			// clears the shot; a full grace window with no drop counts a miss and rotates that
			// pawn to the next hypothesis. Single-threaded (top of create_move).
			void on_tick( );

			void clear( );

		private:
			struct pending_shot
			{
				int hypo{};
				int victim_health{};
				int age{};
			};

			// Ticks to wait for the victim's health to fall before ruling a shot a miss. A
			// subtick shot's damage is acknowledged within a couple of ticks even under lag
			// compensation; 16 leaves generous room for the server round trip.
			static constexpr int k_miss_grace_ticks{ 16 };

			std::unordered_map<std::uintptr_t, int> m_hypothesis{};
			std::unordered_map<std::uintptr_t, pending_shot> m_pending{};
		};

		class penetration
		{
		public:
			struct weapon_data
			{
				float damage;
				float penetration;
				float range_modifier;
				float range;
				float armor_ratio;
				float headshot_multiplier;
			};

			struct damage_scales
			{
				float ct_head;
				float t_head;
				float ct_body;
				float t_body;
			};

			struct run_context
			{
				std::uintptr_t target_pawn{};
				int target_armor{};
				int target_team{};
				bool has_helmet{};
				damage_scales scales{};
				float armor_ratio{};
				float headshot_multiplier{};
				systems::hitboxes::set hitboxes{};
				lagcomp::record* record{};
				// Surfaces a bullet may pass through before the trace gives up. Filled from the
				// weapon group's penetration_layers by prepare_target; the default matches the
				// value that used to be hardcoded at both trace call sites.
				int max_layers{ 4 };
			};

			struct result
			{
				float damage{};
				int hitbox{ -1 };
				int hitgroup{ -1 };
				bool penetrated{};
			};

			void prepare( std::uintptr_t weapon_vdata, std::uintptr_t weapon );

			[[nodiscard]] run_context prepare_target( std::uintptr_t target_pawn, lagcomp::record* record ) const;
			// aimed_hitbox is the hitbox the caller pointed this trace at. When the engine's
			// trace_bullet confirms damage on the target pawn but the local ray-vs-bone pass
			// cannot name a hitbox -- which is what a deflected penetrating bullet looks like --
			// the result is attributed to this index instead of being thrown away. Pass -1 to
			// keep the old behaviour of discarding those hits.
			// log_contacts writes every surface the trace touched to the log (used once per fired shot).
			[[nodiscard]] bool run( const math::vector3& start, const math::vector3& end, const run_context& ctx, std::uintptr_t local_pawn, int local_team, result& out, int aimed_hitbox = -1, bool log_contacts = false ) const;
			[[nodiscard]] bool can( const math::vector3& start, const math::vector3& direction, float& out_damage, const systems::local::snapshot& local ) const;
			[[nodiscard]] float get_max_damage( int hitgroup, int target_armor, bool has_helmet, int target_team ) const;
			[[nodiscard]] const weapon_data& get_weapon_data( ) const { return this->m_weapon_data; }

		private:
			void scale_damage( int hitgroup, int armor, bool has_helmet, int team, float armor_ratio, float headshot_multiplier, const damage_scales& scales, float& damage ) const;
			// Clamped penetration_layers for the weapon the shared context currently holds.
			[[nodiscard]] int resolve_layer_cap( ) const;
			weapon_data m_weapon_data{};
		};

		class shoot_history
		{
		public:
			struct ring_entry
			{
				int tick{};
				float fraction{};
				math::vector3 position{};
			};

			struct eye_candidate
			{
				math::vector3 position{};
				int player_tick{};
				float player_frac{};
				int lerp_ticks_int{};
				float lerp_ticks_frac{};
				bool is_uninterpolated{};
			};

			struct eye_candidates
			{
				eye_candidate entries[ 2 ]{};
				int count{};
			};

			void snapshot( std::uintptr_t local_pawn, std::uintptr_t weapon_services );
			[[nodiscard]] eye_candidates get_candidates( ) const;
			[[nodiscard]] bool has_data( ) const { return this->m_count > 0; }
			[[nodiscard]] int client_tick( ) const { return this->m_client_tick; }
			[[nodiscard]] float client_tick_frac( ) const { return this->m_client_tick_frac; }
			[[nodiscard]] int server_tick( ) const { return this->m_server_tick; }
			[[nodiscard]] int lerp_ticks_int( ) const { return this->m_lerp_ticks_int; }
			[[nodiscard]] float lerp_ticks_frac( ) const { return this->m_lerp_ticks_frac; }
			[[nodiscard]] int count( ) const { return this->m_count; }
			[[nodiscard]] int oldest_tick( ) const { return this->m_count > 0 ? this->m_entries[ 0 ].tick : -1; }
			[[nodiscard]] int newest_tick( ) const { return this->m_count > 0 ? this->m_entries[ this->m_count - 1 ].tick : -1; }

		private:
			ring_entry m_entries[ 32 ]{};
			int m_count{};
			int m_client_tick{};
			float m_client_tick_frac{};
			int m_server_tick{};
			int m_lerp_ticks_int{};
			float m_lerp_ticks_frac{};
		};

		struct context
		{
			std::uintptr_t weapon{};
			std::uintptr_t weapon_services{};
			std::uintptr_t weapon_vdata{};
			std::uint32_t weapon_type{};
			std::uint16_t item_def_idx{};
			int num_bullets{};
			float recoil_index{};
			int current_tick{};
			int ticks_since_land{};
			float current_time{};
			float weapon_max_speed{};
			float range{};
			bool is_jump_scouting{};
			bool is_scoped{};
			bool valid{};

			float inaccuracy{};
			float spread{};
			// Vertical speed the inaccuracy above was predicted at. Logged next to the game's own at fire time.
			float inaccuracy_velocity_z{};
		};

		void update( );
		void invalidate_if_needed( );

		[[nodiscard]] context& ctx( ) { return this->m_ctx; }
		[[nodiscard]] penetration& pen( ) { return this->m_pen; }
		[[nodiscard]] lagcomp& lc( ) { return this->m_lc; }
		[[nodiscard]] resolver& res( ) { return this->m_resolver; }
		[[nodiscard]] shoot_history& sh( ) { return this->m_sh; }

		[[nodiscard]] bool autowalling( ) const { return this->m_autowalling; }
		[[nodiscard]] lagcomp::record* current_autowall_record( ) const { return this->m_current_autowall_record; }

		[[nodiscard]] int& last_shoot_tick( ) { return this->m_last_shoot_tick; }
		/// Called once a gun shot is committed to a command. Remembers the weapon's next attack tick as it
		/// read at that moment so can_shoot can tell a shot the game has not predicted yet from a ready gun.
		void note_gun_shot( int tick_base );

		[[nodiscard]] std::uint32_t get_spread_seed( const math::vector3& angles, int tick ) const;
		[[nodiscard]] math::vector2 calculate_spread( int seed, float accuracy, float spread, float recoil_index, int item_def_idx, int num_bullets ) const;
		[[nodiscard]] math::vector3 get_aim_punch( std::uintptr_t local_pawn ) const;
		/// The aim punch the weapon fire code adds to the view angles for a shot fired at this tick and fraction.
		/// get_aim_punch is the render-time value the camera uses; between shots they differ.
		[[nodiscard]] math::vector3 get_aim_punch_at( std::uintptr_t local_pawn, int tick, float fraction ) const;
		[[nodiscard]] float calculate_hitchance( const math::vector3& shoot_position, const math::vector3& aim_angle, const systems::hitboxes::entry& hitbox, const systems::bones::data& bone, float inaccuracy, float spread, int samples = 256 ) const;
		struct spread_solution
		{
			/// The angles the bullet is fired along before spread is applied: view angles plus aim punch.
			math::vector3 shot_angles{};
			std::uint32_t seed{};
			int evaluated{};
			/// Degrees between the shot angles and the nearest edge of the half-degree cell the seed was hashed from.
			float margin{};
			bool valid{};
		};

		/// Shot angles whose own spread seed deflects the bullet exactly onto aim_angle. `punch` is only used to
		/// keep the resulting view pitch inside what a command can carry.
		[[nodiscard]] spread_solution find_spread_correction( const math::vector3& aim_angle, int tick, const math::vector3& punch ) const;
		[[nodiscard]] math::vector3 get_eye_position( std::uintptr_t local_pawn ) const;
		[[nodiscard]] math::vector3 get_shoot_position( ) const;
		[[nodiscard]] math::vector3 get_interpolated_shoot_position( std::uintptr_t local_pawn, bool newest = false ) const;
		[[nodiscard]] int calculate_stop_ticks( const math::vector3& velocity, float max_speed, std::uintptr_t local_pawn ) const;
		[[nodiscard]] float get_spread( ) const;
		[[nodiscard]] float get_inaccuracy( bool update_accuracy_penalty ) const;
		[[nodiscard]] float get_inaccuracy_at_velocity( std::uintptr_t local_pawn, const math::vector3& velocity ) const;
		[[nodiscard]] float get_air_inaccuracy( float vertical_speed, float jump_initial, float jump_apex ) const;
		[[nodiscard]] bool can_shoot( systems::input::usercmd* cmd, std::uintptr_t local_controller, bool check_next_attack = true ) const;
		[[nodiscard]] bool is_max_accuracy( float inaccuracy ) const;
		[[nodiscard]] math::vector3 simulate_aim_punch( int recoil_index ) const;

		bool ray_vs_capsule( const math::vector3& ray_origin, const math::vector3& ray_dir, const math::vector3& capsule_a, const math::vector3& capsule_b, float radius, float& out_fraction ) const;

	private:
		context m_ctx{};
		penetration m_pen{};
		lagcomp m_lc{};
		resolver m_resolver{};
		shoot_history m_sh{};

		// Parallel rage workers must not overwrite each other's trace record.
		inline static thread_local bool m_autowalling{};
		inline static thread_local lagcomp::record* m_current_autowall_record{ nullptr };

		int m_last_shoot_tick{};
		// m_nNextPrimaryAttackTick when the last gun shot was committed, and the tick it was committed on.
		int m_last_shot_next_primary{ -1 };
		int m_last_gun_shot_tick{ -1 };
	};

	class misc
	{
	private:
		class antiaim
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void on_render( xdraw::draw_list& draw_list ) const;

			[[nodiscard]] bool has_modified_angles( ) const { return this->m_should_correct || this->m_modified_angles.y != this->m_old_angles.y; }
			[[nodiscard]] const math::vector3& get_modified_angles( ) const { return this->m_modified_angles; }

			/// Rotate the wish direction by the same delta the anti-aim applied to the view.
			/// Called late in CreateMove (after every other feature has had its say) so the
			/// correction sees the final yaw, not the yaw at anti-aim time.
			void apply_movement_correction( systems::input::usercmd* cmd );

			/// The yaw a movement feature should build its wish direction against: the one the
			/// player is actually looking down, not the fake the command is carrying. Every
			/// movement feature runs after on_create_move() has already overwritten
			/// base->viewangles, so reading the command there gives the spun yaw and sends the
			/// player sideways. apply_movement_correction() converts camera space back into
			/// sent-yaw space once, at the end of the tick.
			[[nodiscard]] float movement_basis_yaw( const proto::base_usercmd_pb* base ) const;

			/// Hand the movement basis over to a feature that writes a yaw *and* the movement pair to
			/// match it. Fastladder is the one case: it points the command 90 degrees off the camera and
			/// then sets forwardmove/leftmove for that rotated frame on purpose. Without this the
			/// end-of-pipeline correction would read a 90 degree delta, rotate the pair straight back
			/// into camera space and undo the climb. Anything that only moves the yaw -- anti-aim, the
			/// hide-shot, the grenade trajectory -- must not call this.
			void adopt_movement_basis_yaw( float yaw ) { this->m_basis_yaw = yaw; }

			/// True when this tick's command carries the anti-aim yaw. Read by the create_move diagnostics.
			[[nodiscard]] bool is_active( ) const noexcept { return this->m_antiaim_active; }

			/// Writes the movement-correction readout (the same numbers as the on-screen "movement debug" panel) to the log.
			void log_debug( ) const;

		private:
			/// Why a given tick's correction did or did not happen. Drawn on screen behind the
			/// "movement debug" toggle so a report of "it still goes the wrong way" comes with the
			/// numbers attached instead of another round of reading the pipeline by hand.
			enum class correction_state : int
			{
				idle,
				disarmed,
				strafer_owned,
				no_base,
				no_angles,
				no_delta,
				rotated,
				replayed
			};

			/// Rotate every analog subtick step by the same delta as the scalar pair. The steps
			/// carry deltas against the server's running m_flCmdForwardMove/m_flCmdLeftMove, so
			/// each one is reconstructed to an absolute, rotated, then re-differenced.
			///
			/// Returns the total the chain ends on -- what the server's running forward/left values will be
			/// once every step has been applied.
			[[nodiscard]] math::vector2 rotate_subtick_moves( proto::base_usercmd_pb* base, const math::vector2& start, float sin_delta, float cos_delta ) const;

			[[nodiscard]] float get_pitch( float view_pitch );
			[[nodiscard]] float get_yaw( const math::vector3& view_angles, const systems::local::snapshot& local );

			/// Offset contributed by the selected yaw mode. Advances jitter/spin/random state
			/// once per command tick, so a double CreateMove in one tick does not double-step it.
			[[nodiscard]] float get_mode_offset( );

			math::vector3 m_old_angles{};
			math::vector3 m_modified_angles{};

			/// The yaw the command carried before any feature touched it, captured on every tick at the
			/// top of `on_create_move` -- which is the first thing the pipeline runs. This is the only
			/// honest answer to "where is the player looking", and it is what both `movement_basis_yaw`
			/// and the correction delta are measured against. Anti-aim being switched off, or standing
			/// down for a tick, does not make it stale: it is refreshed either way.
			float m_basis_yaw{};

			int m_yaw_side{};
			bool m_should_correct{};
			bool m_antiaim_active{};
			float m_indicator_yaw{};

			/// Yaw-mode animation state.
			bool m_jitter_side{};
			int m_jitter_step{};
			float m_spin_yaw{};
			float m_random_yaw{};
			int m_random_step{};
			int m_last_yaw_tick{ -1 };

			/// CreateMove runs once per rendered frame, not once per command tick: at 300 fps on a 64
			/// tick server the same command is handed to the hook four or five times, and the yaw modes
			/// already guard against that (see get_mode_offset). The rotation had no such guard, so the
			/// wish direction was rotated by the anti-aim delta once per frame -- 2x, 3x, 4x delta --
			/// which is why "hold W" landed anywhere but forward and why the direction changed with
			/// framerate rather than with the fix being applied. These remember the tick we last
			/// rotated on and both sides of that rotation, so a repeat call restores the player's own
			/// pair before rotating it again exactly once. The floats are compared bit-for-bit on
			/// purpose: they are our own output read straight back out of the protobuf, not a
			/// re-derivation of it.
			int m_corrected_tick{ -1 };

			/// The value m_flCmdLeftMove takes when the player is holding A (IN_MOVELEFT). The rotation and the
			/// button sync both depend on it. The default is the test strafer's convention (A is negative,
			/// so positive leftmove is toward the right), and it is replaced once, from the first untouched
			/// A or D press the game hands us, so a wrong default cannot survive a real keypress.
			float m_left_key_sign{ -1.0f };
			bool m_left_sign_learned{};
			math::vector2 m_corrected_in{};
			math::vector2 m_corrected_out{};

			/// Snapshot for the on-screen readout. Written on every return path of the correction.
			correction_state m_dbg_state{ correction_state::idle };
			float m_dbg_camera_yaw{};
			float m_dbg_sent_yaw{};
			float m_dbg_delta{};
			math::vector2 m_dbg_in{};
			math::vector2 m_dbg_out{};
			int m_dbg_steps{};
			int m_dbg_calls{};
		};

		class duckpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void on_override_view( std::uintptr_t view_setup );

		private:
			bool m_was_active{};
			bool m_fake_stand_active{};
		};

		class quickpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void reset_if_needed( );

		private:
			static constexpr std::uint32_t invalid_effect_index{ static_cast<std::uint32_t>( -1 ) };

			void create_particle( );
			void update_particle( );
			void release_particle( );
			void reset( );

			math::vector3 m_saved_origin{};
			bool m_should_retrack{};
			bool m_fired{};
			bool m_active{};
			std::uint32_t m_particle_effect{ invalid_effect_index };
			bool m_particle_loaded{};
			std::uintptr_t m_prev_movement_bits{};
		};

		class autostop
		{
		public:
			/// Called before the ragebot (after_rage = false) and after it (after_rage = true).
			void on_create_move( systems::input::usercmd* cmd, bool after_rage );

			/// Movement features that run later leave the command alone on a tick auto stop braked.
			[[nodiscard]] bool braked_this_tick( ) const { return this->m_braked_this_tick; }

		private:
			void apply_brake( systems::input::usercmd* cmd, proto::base_usercmd_pb* base, float wish_x, float wish_y, float magnitude );

			bool m_braked_this_tick{};
		};

		class jumpscout
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );

			/// In the air with the scout and jump scout on.
			[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
			/// The air inaccuracy has come down to the configured part of the way to the apex.
			[[nodiscard]] bool apex_ready( ) const;

		private:
			[[nodiscard]] bool has_ssg_08( ) const;

			bool m_active_this_tick{};
		};

		antiaim m_antiaim{};
		duckpeek m_duckpeek{};
		quickpeek m_quickpeek{};
		autostop m_autostop{};
		jumpscout m_jumpscout{};

	public:
		[[nodiscard]] antiaim& antiaim( ) { return this->m_antiaim; }
		[[nodiscard]] duckpeek& duckpeek( ) { return this->m_duckpeek; }
		[[nodiscard]] quickpeek& quickpeek( ) { return this->m_quickpeek; }
		[[nodiscard]] autostop& autostop( ) { return this->m_autostop; }
		[[nodiscard]] jumpscout& jumpscout( ) { return this->m_jumpscout; }
	};

	class rage
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool should_stop( ) const noexcept { return this->m_should_stop; }
		/// Whether the ragebot resolved a valid target this tick. autostop gates its
		/// "only stop when aiming at someone" behaviour on this; legit has no such requirement.
		[[nodiscard]] bool has_target( ) const noexcept { return this->m_target.valid; }
		[[nodiscard]] bool is_firing_this_tick( ) const noexcept { return this->m_firing_this_tick; }
		[[nodiscard]] bool is_cocking_revolver( ) const noexcept { return this->m_revolver_cock_ticks > 0; }
		[[nodiscard]] bool should_release_duck_for_shot( ) const noexcept { return this->m_release_duck_for_shot; }
		[[nodiscard]] bool duckpeek_wants_reduck( ) const noexcept { return this->m_duckpeek_reduck; }
		void clear_duckpeek_reduck( ) noexcept { this->m_duckpeek_reduck = false; }

		static constexpr auto k_max_lagcomp_records{ 16 };
		// Upper bound on records scanned per candidate. The per-weapon backtrack_records
		// setting picks the live count (spread evenly across the valid window); this is
		// only the ceiling that sizes the index buffer. Against a choke/fake-lag peeker
		// the real hitbox sits in the ticks between newest and oldest, so we need the
		// headroom to reach them -- scanning just the two extremes walks right past it.
		static constexpr auto k_max_scan_records{ k_max_lagcomp_records };

	private:
		struct aim_context
		{
			math::vector3 view_angles{};
			math::vector3 velocity{};

			float predicted_inaccuracy{};
			float spread{};

			float weapon_max_speed{};
			float accurate_threshold{};
			bool on_ground{};
			bool is_scoped{};
		};

		struct stop_prediction
		{
			math::vector3 eye{};
			float inaccuracy{};
		};

		struct candidate
		{
			std::uintptr_t pawn{};
			int health{};
			int armor{};
			float min_damage{};
			std::array<shared::lagcomp::record*, k_max_lagcomp_records> records{};
			int record_count{};

			// Backing store for resolver-rotated poses. Filled lazily in scan_players when the
			// resolver puts this pawn on a non-zero hypothesis; scan_hits point their `record`
			// here, so it has to outlive the scan. Reserved to record_count up front so the
			// push per record never reallocates and invalidates an earlier hit's pointer.
			std::vector<shared::lagcomp::record> resolver_poses{};
		};

		struct scan_hit
		{
			math::vector3 position{};
			math::vector3 aim_angle{};
			float damage{};
			float score{};
			float fov{};
			int hitbox_index{};
			int hitgroup{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool is_center{};
			bool penetrated{};
			// Still deals min_damage at both desync extremes (±desync_range). Only computed
			// when a safe-point / baim_lethal setting is on; false means "unknown or unsafe".
			bool is_safe{};
			bool is_backstab{};
			int attack_type{};
			shared::shoot_history::eye_candidate source_eye{};

			std::uintptr_t pawn{};
			int health{};
			shared::lagcomp::record* record{};
		};

		struct target
		{
			scan_hit hit{};
			float hitchance{};
			float score{};
			bool valid{};

			[[nodiscard]] bool is_lethal( ) const noexcept
			{
				return this->hit.damage >= static_cast< float >( this->hit.health );
			}
		};

		struct knife_info
		{
			bool can_slash{};
			bool can_stab{};
			bool charged{};
			float armor_ratio{};
		};

		[[nodiscard]] aim_context build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const;
		[[nodiscard]] std::optional<stop_prediction> predict_stop( const aim_context& ctx, const math::vector3& current_eye, const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<candidate> gather_candidates( const systems::local::snapshot& local, float max_distance_sq = 0.0f ) const;

		void run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire = true );
		void run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );

		[[nodiscard]] std::vector<scan_hit> scan_players( const math::vector3& eye, float inaccuracy, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<scan_hit> scan_player( const math::vector3& eye, float inaccuracy, const aim_context& ctx, candidate& cand, shared::lagcomp::record* record, const systems::local::snapshot& local ) const;
		[[nodiscard]] target select_best( const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy ) const;
		[[nodiscard]] float evaluate_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy ) const;
		
		[[nodiscard]] float get_standing_inaccuracy( const systems::local::snapshot& local, const aim_context& ctx ) const;

		[[nodiscard]] std::vector<scan_hit> scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;

		[[nodiscard]] knife_info get_knife_info( const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<scan_hit> scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;

		void fire_gun( systems::input::usercmd* cmd, const target& tgt, bool was_forced, const math::vector3& shoot_eye, const systems::local::snapshot& local, bool want_subtick );
		void fire_melee( systems::input::usercmd* cmd, const target& tgt, const systems::local::snapshot& local );

		[[nodiscard]] std::vector<math::vector3> generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& bone_rot, float pointscale, const math::vector3& shoot_pos, float inaccuracy ) const;
		[[nodiscard]] bool should_stop_movement( const aim_context& ctx ) const;
		[[nodiscard]] float get_min_damage( const settings::combat::ragebot::weapon_group& config, int target_health, bool override_active ) const;
		[[nodiscard]] float get_knife_damage( float raw, int armor, float armor_ratio ) const;
		[[nodiscard]] systems::tracing::result trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;
		[[nodiscard]] systems::tracing::result trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;

		enum class penetration_crosshair_state : std::uint8_t
		{
			unavailable,
			blocked,
			penetrable
		};

		void update_penetration_crosshair( const systems::local::snapshot& local );
		void draw_penetration_crosshair( xdraw::draw_list& draw_list ) const;

		/// Whether a subtick-aligned shot is possible right now: the setting is on and the gun
		/// itself is ready. Touches nothing, so run_gun can ask before it has decided anything.
		[[nodiscard]] bool can_subtick_shot( const systems::input::usercmd* cmd ) const;

		/// Converts the shot into a press/release pair of subtick steps so it lands on an exact
		/// sub-tick offset instead of the tick boundary. Returns true when it took ownership of
		/// the attack, in which case fire_gun must not touch the legacy button fields.
		///
		/// Call this only at a point of no return. It was previously invoked from run_gun before
		/// fire_gun ran, and fire_gun can still bail (no valid record, no spread correction) --
		/// which left the attack pulse in the command with none of the aim work behind it, so the
		/// bullet left along whatever yaw the command happened to carry. That is the phantom shot.
		[[nodiscard]] bool emit_subtick_shot( systems::input::usercmd* cmd );

		/// Counters behind the periodic "rage:" log line. They answer the one question the ragebot cannot
		/// answer from the outside: when a target was hittable and the gun did not fire, which gate held it.
		/// Only ever touched from the game thread, so plain integers.
		struct decision_diag
		{
			int frames{}, cant_shoot{}, no_candidates{}, no_hit{}, targets{}, held_hitchance{}, held_duck{}, held_brake{}, stop_frames{}, fired{};
			float hc_sum{}, hc_short_sum{};
			int latency_sum{}, latency_max{}, latency_n{};
			unsigned long long last_report{};
		};

		decision_diag m_diag{};

		/// Why scanned points were thrown away, for the same log line. The scan runs on the thread pool, hence
		/// atomics; best_damage is the highest damage any point reached, accepted or not, which says at a glance
		/// whether min damage or the walls are what kept the gun quiet.
		struct scan_diag
		{
			std::atomic<int> candidates{}, extrapolated{}, records{}, points{}, fov{}, no_damage{}, below_min{}, head_group{}, accepted{}, best_damage{};
		};

		mutable scan_diag m_scan_diag{};
		int m_first_target_tick{ -1 };
		void report_decision_diag( );

		bool m_should_stop{};
		bool m_firing_this_tick{};
		bool m_release_duck_for_shot{};
		bool m_duckpeek_reduck{};

		// Last target resolved by run_gun / run_taser / run_knife. Cleared at the top of
		// on_create_move so a holster, death or lost target releases anything gated on it.
		target m_target{};

		std::uint8_t m_knife_attack{};
		bool m_zeus_fired{};

		int m_revolver_cock_ticks{};
		std::atomic<penetration_crosshair_state> m_penetration_crosshair_state{ penetration_crosshair_state::unavailable };

		std::vector<shared::lagcomp::record> m_extrapolated_records{};

		struct debug_point
		{
			math::vector3 position{};
			int hitbox_index{};
			bool is_center{};
		};

		mutable std::vector<debug_point> m_debug_points{};
		mutable std::mutex m_debug_mtx{};
	};

	class legit
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );
		void invalidate_if_needed( );

		/// Runs once per rendered frame, ahead of the game's own mouse handling. The aim moves the view here
		/// rather than in create_move: a 64 Hz step shows up as stutter at any frame rate above the tick
		/// rate, and the target's rendered bones move every frame, not every tick.
		void on_frame_input( std::uintptr_t csgo_input, int slot, float frametime );

		enum class wallbang_state : std::uint8_t
		{
			none,
			soon,
			now
		};

		/// For the esp: whether this pawn can be wallbanged from here, or from where the current movement
		/// is heading. Safe to call from the render thread.
		[[nodiscard]] wallbang_state wallbang_preview( std::uintptr_t pawn ) const;

		[[nodiscard]] bool has_target( ) const noexcept { return this->m_target.has_target( ); }

		/// The triggerbot has someone under the crosshair and trigger auto stop is on; auto stop brakes for it.
		[[nodiscard]] bool wants_stop( ) const noexcept { return this->m_wants_stop; }

	private:
		struct scan_point
		{
			math::vector3 position{};
			float damage{};
			float fov{};
			int hitgroup{};
			std::size_t cfg_index{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool visible{};
			bool valid{};
		};

		struct target_result
		{
			std::uintptr_t controller{};
			std::uintptr_t pawn{};
			scan_point best_point{};
			math::vector3 aim_angle{};
			float hitchance{};
			float score{};
			float fov{};
			int health{};
			shared::lagcomp::record* record{};

			/// The point came off the newest record, so the aim can follow the live, rendered bone instead of
			/// a snapshot that only updates once per tick.
			bool newest{};

			[[nodiscard]] bool has_target( ) const noexcept { return this->best_point.valid; }
		};

		/// What on_frame_input steers toward between two create_moves. Re-armed every tick the scan still
		/// finds the target; a tick without it disarms it.
		struct track_state
		{
			/// The entity cache holds controllers, not pawns, so the controller is what proves the target is
			/// still in the game; the pawn is checked against the controller's current pawn handle.
			std::uintptr_t controller{};
			std::uintptr_t pawn{};
			int bone_index{ -1 };
			math::vector3 local_center{};
			bool has_hitbox{};
			math::vector3 fixed_point{};
			bool live{};
			int smooth{};
			bool active{};
			std::chrono::steady_clock::time_point refreshed{};

			math::vector3 prev_desired{};
			bool has_prev{};
		};

		struct preview_entry
		{
			std::uintptr_t pawn{};
			wallbang_state state{};
		};

		[[nodiscard]] target_result find_target( const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const;
		/// sticky_bone is the bone already being tracked on this pawn, or -1. It keeps the point unless another
		/// one ranks strictly higher (visible over occluded, head over body), so damage and crosshair-distance
		/// ties can't flip the aim between two hitboxes every tick.
		[[nodiscard]] scan_point scan_player( std::uintptr_t pawn, shared::lagcomp::record* record, const systems::hitboxes::set& hitboxes, const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local, int sticky_bone ) const;

		/// Newest record first, then the middle and oldest of the valid window, capped at max_records.
		/// Shared by the aimbot's backtrack and the triggerbot, which had this inline as a lambda.
		[[nodiscard]] static std::array<shared::lagcomp::record*, 3> gather_records( std::uintptr_t pawn, int max_records );

		/// Ordering for candidate aim points, both across hitboxes within a record and across backtrack
		/// records. Visibility outranks everything, then the head, then damage, then crosshair distance.
		[[nodiscard]] static bool better_point( const scan_point& a, const scan_point& b );

		/// Chance the shot would connect if it were taken from tgt.aim_angle right now. Measured at the
		/// candidate angle rather than the current view angle on purpose: the aim has to be allowed to
		/// travel before it can be judged, and a gate that read the current angle would refuse to let
		/// the flick start and then never rise above zero.
		[[nodiscard]] float evaluate_hitchance( const target_result& tgt, const math::vector3& shoot_position, int samples ) const;

		/// True once the reaction delay for this engagement has elapsed. Starts a fresh engagement --
		/// new delay, new aim error, wind-up back to zero -- whenever the target pawn changes.
		[[nodiscard]] bool update_engagement( std::uintptr_t pawn, const settings::combat::legitbot::weapon_group& config );
		void reset_engagement( );

		/// Hands the scan's winner to on_frame_input once the reaction delay has run out.
		void arm_tracking( const target_result& tgt, const settings::combat::legitbot::weapon_group& config );
		void apply_triggerbot( systems::input::usercmd* cmd, const math::vector3& shoot_position, const math::vector3& view_angles, const math::vector3& aim_punch, float rcs_scale, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );

		/// Takes the recoil out of the shot's input history angles, scaled by rcs_scale. The camera is never
		/// touched.
		void apply_recoil_control( systems::input::usercmd* cmd, float rcs_scale, const systems::local::snapshot& local ) const;

		void update_wallbang_preview( const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );

		void draw_fov( xdraw::draw_list& draw_list, float fov_degrees, const config::col& color ) const;

		target_result m_target{};
		track_state m_track{};

		/// Diagnostics: ticks the aim key has been held, and frames on_frame_input actually moved the view.
		int m_diag_ticks{};
		int m_diag_steered{};

		std::vector<preview_entry> m_preview{};
		mutable std::mutex m_preview_mtx{};
		int m_preview_counter{};

		float m_remainder_x{};
		float m_remainder_y{};

		/// Per-engagement human-motion state, re-rolled by update_engagement whenever the target
		/// changes. m_engage_ticks drives the wind-up ramp; the two errors are held constant for the
		/// whole engagement so the aim settles somewhere slightly off centre and stays there.
		std::uintptr_t m_engage_pawn{};
		float m_engage_ready_time{};
		float m_engage_seen_time{};
		int m_engage_ticks{};
		float m_error_x{};
		float m_error_y{};

		float m_trigger_delay_start{};
		float m_trigger_delay_ms{};
		float m_trigger_release_time{};
		std::uintptr_t m_trigger_pending_pawn{};
		bool m_wants_stop{};
	};

} // namespace features::combat
