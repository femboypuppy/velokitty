#pragma once

namespace features::movement {

	/// Counters for the periodic create_move log line. Each one is bumped at the point a feature either
	/// bails out or actually does its work, so a feature that "does nothing" shows *which* gate stopped it.
	struct diagnostics
	{
		std::atomic<std::uint32_t> bhop_calls{}, bhop_autobhop_convar{}, bhop_no_jump_key{}, bhop_on_ground{}, bhop_air_jump_held{}, bhop_no_landing{}, bhop_scheduled{}, bhop_retry{};
		std::atomic<std::uint32_t> airstrafe_calls{}, airstrafe_shift_air{}, airstrafe_off_or_firing{}, airstrafe_ground{}, airstrafe_sprint{}, airstrafe_ran{};
		std::atomic<std::uint32_t> strafer_calls{}, strafer_inactive{}, strafer_ground{}, strafer_ran{}, strafer_yield{};
	};

	inline diagnostics g_diag{};
	class bhop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );

		/// A landing press went into this command. Strafers stay out of it: with their steps in the same
		/// command the game no longer credits the press at its fraction, and the hop misses the window.
		[[nodiscard]] bool pressed_this_tick( ) const { return this->m_pressed_this_tick; }

	private:
		void reset( );
		[[nodiscard]] bool press( systems::input::usercmd* cmd, int tick, float when );

		bool m_pressed_this_tick{};
		int m_command{};
		// Jump has been held since the air, so every press of this cycle is ours to time.
		bool m_cycle{};
		int m_ground_tick{ -1 };
		int m_pending_tick{ -1 };
		float m_pending_when{};
		double m_last_press{ -1.0e9 };

		// The last press made for a landing, written to the log once the jump it made is in the air.
		struct hop_record
		{
			const char* path{};
			float snap{};
			float contact{};
			float speed{};
			float when{};
			bool logged{ true };
		} m_hop{};
	};

	class airstrafe
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_angles( );

	private:
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		void rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const;
		void rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const;

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		bool m_side_switch{};
		math::vector3 m_angles{};
	};

	class jumpbug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
		[[nodiscard]] float landing_fraction( ) const { return this->m_landing_fraction; }

	private:
		[[nodiscard]] float get_impulse_mul( std::uintptr_t local_pawn ) const;

		float m_landing_fraction{ 1.0f };
		bool m_active_this_tick{ false };
	};

	class fastladder
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgejump
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgestop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgebug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }

	private:
		bool m_active_this_tick{ false };
	};

	class slowwalk
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class test_strafer
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool is_active( ) const;
		[[nodiscard]] bool handled_this_tick( ) const { return this->m_handled_this_tick; }

	private:
		void quantized_path( systems::input::usercmd* cmd );
		[[nodiscard]] bool apply_yaw_subtick( proto::base_usercmd_pb* base, float when, float yaw_delta ) const;
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		[[nodiscard]] static math::vector2 movement_from_buttons( std::uintptr_t pressed );

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		int m_substep_counter{};
		bool m_handled_this_tick{};
	};

} // namespace features::movement