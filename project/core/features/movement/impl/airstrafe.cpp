#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>

namespace features::movement {

	void airstrafe::on_create_move( systems::input::usercmd* cmd )
	{
		g_diag.airstrafe_calls.fetch_add( 1, std::memory_order_relaxed );

		// Auto stop / jump scout is braking this tick; strafing would undo it.
		if ( features::combat::g_misc.autostop( ).braked_this_tick( ) )
		{
			return;
		}

		if ( features::movement::g_jumpbug.active_this_tick( ) )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto current_buttons = cmd->buttons.value;
		const bool shift_held = current_buttons & static_cast< std::uintptr_t >( cstypes::command_buttons::in_sprint );

		const auto& prestate = systems::g_prediction.pre( );
		const bool in_air = !( prestate.flags & cstypes::entity_flags::on_ground );

		if ( shift_held && in_air )
		{
			g_diag.airstrafe_shift_air.fetch_add( 1, std::memory_order_relaxed );
			const auto& vel = prestate.networked_velocity;
			float forward_move = 0.0f;
			float left_move = 0.0f;

			if ( vel.length_2d( ) > 10.0f )
			{
				const auto vel_yaw = std::atan2f( vel.y, vel.x )
					* ( 180.0f / std::numbers::pi_v<float> );

				const auto view_yaw = features::combat::g_misc.antiaim( ).movement_basis_yaw( base );
				const auto relative_yaw = ( vel_yaw - view_yaw )
					* ( std::numbers::pi_v<float> / 180.0f );

				const auto fwd_x = std::cosf( relative_yaw );
				const auto fwd_y = std::sinf( relative_yaw );

				forward_move = std::clamp( -fwd_x, -1.0f, 1.0f );
				left_move = std::clamp( -fwd_y, -1.0f, 1.0f ); // negated
			}

			base->set_forwardmove( forward_move );
			base->set_leftmove( left_move );

			// write subtick entries so the engine respects the input precisely 
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
			return;
		}

		const auto wants_stop = features::combat::g_rage.should_stop( ) || features::misc::g_projectile_trajectory.should_stop( );
		// Auto strafe on a server it cannot steer on (see test_strafer::is_active) strafes this way instead.
		const auto stands_in_for_strafer = settings::g_movement.m_test_strafer.enabled.value && !features::movement::g_test_strafer.is_active( );

		if ( !settings::g_movement.airstrafe.value && !stands_in_for_strafer && !wants_stop || features::combat::g_rage.is_firing_this_tick( ) )
		{
			g_diag.airstrafe_off_or_firing.fetch_add( 1, std::memory_order_relaxed );
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
			return;
		}

		if ( settings::g_movement.m_test_strafer.enabled.value && features::movement::g_test_strafer.handled_this_tick( ) )
		{
			return;
		}

		// Same as test_strafer: no steps of ours in a command carrying a bhop landing press.
		if ( features::movement::g_bhop.pressed_this_tick( ) )
		{
			return;
		}

		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			g_diag.airstrafe_ground.fetch_add( 1, std::memory_order_relaxed );
			return;
		}

		if ( current_buttons & static_cast< std::uintptr_t >( cstypes::command_buttons::in_sprint ) )
		{
			g_diag.airstrafe_sprint.fetch_add( 1, std::memory_order_relaxed );
			return;
		}

		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( !subtick_moves )
		{
			return;
		}

		if ( !wants_stop )
		{
			this->check_button( current_buttons, cstypes::command_buttons::in_moveleft );
			this->check_button( current_buttons, cstypes::command_buttons::in_moveright );
			this->check_button( current_buttons, cstypes::command_buttons::in_forward );
			this->check_button( current_buttons, cstypes::command_buttons::in_back );
			this->m_last_buttons = current_buttons;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		g_diag.airstrafe_ran.fetch_add( 1, std::memory_order_relaxed );

		const auto sv_airaccelerate = CONVAR ("sv_airaccelerate")->get<float>( );
		const auto sv_air_max_wishspeed = CONVAR ("sv_air_max_wishspeed")->get<float>( );
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_staminarecoveryrate = CONVAR ("sv_staminarecoveryrate")->get<float>( );

		const auto view_angles = this->m_angles;
		// Where the player is looking, not what the command carries — anti-aim has already
		// overwritten base->viewangles by the time this runs, and strafing against the fake yaw
		// is what sent the player in every direction but the one they were facing.
		const auto basis_yaw = features::combat::g_misc.antiaim( ).movement_basis_yaw( base );
		const auto cmd_move_backup = math::vector3{ base->forwardmove( ), base->leftmove( ), 0.0f };
		const auto effective_maxspeed = memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );

		constexpr auto subtick_count{ 32 };
		constexpr auto frame_time{ cstypes::tick_interval / static_cast< float >( subtick_count ) };

		auto yaw_offset{ 0.0f };

		if ( !wants_stop )
		{
			if ( this->m_last_pressed & cstypes::command_buttons::in_moveleft )
			{
				yaw_offset += 90.0f;
			}

			if ( this->m_last_pressed & cstypes::command_buttons::in_moveright )
			{
				yaw_offset -= 90.0f;
			}

			if ( this->m_last_pressed & cstypes::command_buttons::in_forward )
			{
				yaw_offset *= 0.5f;
			}
			else if ( this->m_last_pressed & cstypes::command_buttons::in_back )
			{
				yaw_offset = -yaw_offset * 0.5f + 180.0f;
			}
		}

		const auto has_direction_input = ( this->m_last_pressed & cstypes::command_buttons::in_moveleft ) || ( this->m_last_pressed & cstypes::command_buttons::in_moveright ) || ( this->m_last_pressed & cstypes::command_buttons::in_forward ) || ( this->m_last_pressed & cstypes::command_buttons::in_back );
		const auto effective_wants_stop = wants_stop || ( settings::g_movement.airstrafe_fully_directional.value && !has_direction_input );

		auto velocity = prestate.networked_velocity;
		auto last_impulses = prestate.last_movement_impulses;
		auto stamina = prestate.stamina;
		const auto surface_friction = prestate.surface_friction;

		if ( effective_wants_stop && velocity.length_2d( ) <= 10.0f )
		{
			this->rotate_to_stop( base, velocity );
			return;
		}

		if ( last_impulses.y < 0.0f )
		{
			this->m_side_switch = false;
		}
		else if ( last_impulses.y > 0.0f )
		{
			this->m_side_switch = true;
		}

		for ( auto i = 0; i < subtick_count; ++i )
		{
			base->set_forwardmove( cmd_move_backup.x );
			base->set_leftmove( cmd_move_backup.y );

			auto speed_2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );

			if ( stamina > 0.0f )
			{
				const auto speed_scale = std::clamp( 1.0f - ( stamina / 100.0f ), 0.0f, 1.0f );
				speed_2d *= speed_scale * speed_scale;
				stamina = std::fmaxf( stamina - ( frame_time * sv_staminarecoveryrate ), 0.0f );
			}

			velocity.z -= ( sv_gravity * frame_time );

			if ( speed_2d > 0.0001f )
			{
				math::vector3 forward_dir{}, right_dir{};
				math::helpers::angle_vectors_2d( basis_yaw, forward_dir, right_dir );

				math::vector3 wish_dir
				{
					( forward_dir.x * last_impulses.x * effective_maxspeed ) + ( right_dir.x * last_impulses.y * effective_maxspeed ),
					( forward_dir.y * last_impulses.x * effective_maxspeed ) + ( right_dir.y * last_impulses.y * effective_maxspeed ),
					0.0f
				};

				auto wish_speed = std::sqrtf( wish_dir.x * wish_dir.x + wish_dir.y * wish_dir.y );
				if ( wish_speed > 0.0001f )
				{
					wish_dir.x /= wish_speed;
					wish_dir.y /= wish_speed;
					wish_dir.z = 0.0f;
				}

				wish_speed = std::fminf( wish_speed, effective_maxspeed );

				const auto capped_wish = std::fminf( wish_speed, sv_air_max_wishspeed );
				const auto current_speed = velocity.x * wish_dir.x + velocity.y * wish_dir.y;
				const auto add_speed = capped_wish - current_speed;

				if ( add_speed > 0.0f )
				{
					const auto accel_speed = sv_airaccelerate * effective_maxspeed * frame_time * surface_friction;
					const auto half_accel = accel_speed * 0.5f;
					const auto gain = std::fminf( half_accel, add_speed );

					velocity.x += wish_dir.x * gain;
					velocity.y += wish_dir.y * gain;
				}

				/*if ( add_speed > 0.0f )
				{
					const auto accel_speed = sv_airaccelerate * effective_maxspeed * frame_time * surface_friction;
					const auto half_accel = accel_speed * 0.5f;

					if ( half_accel <= add_speed )
					{
						if ( accel_speed <= add_speed )
						{
							velocity.x += wish_dir.x * half_accel;
							velocity.y += wish_dir.y * half_accel;
						}
						else
						{
							const auto remainder = add_speed - half_accel;
							velocity.x += wish_dir.x * remainder;
							velocity.y += wish_dir.y * remainder;
						}
					}
				}*/
			}

			velocity.z += ( sv_gravity * frame_time ) * 0.5f;
			speed_2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );

			if ( speed_2d >= 10.0f )
			{
				base->set_forwardmove( 0.0f );
				base->set_leftmove( 0.0f );

				if ( effective_wants_stop )
				{
					this->rotate_to_stop( base, velocity );
				}
				else
				{
					const auto velocity_angle = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / std::numbers::pi_v<float> );
					const auto accel_speed = sv_airaccelerate * effective_maxspeed * frame_time * surface_friction;
					const auto half_accel = accel_speed * 0.5f;
					const auto optimal_floor = std::fmaxf( half_accel, sv_air_max_wishspeed - half_accel );
					const auto ideal_angle = std::clamp( std::atanf( optimal_floor / speed_2d ) * ( 180.0f / std::numbers::pi_v<float> ), 0.0f, 45.0f );

					auto target_yaw = view_angles.y + yaw_offset;
					math::helpers::normalize_angle( target_yaw );

					auto velocity_delta = target_yaw - velocity_angle;
					math::helpers::normalize_angle( velocity_delta );

					if ( ( std::fabsf( velocity_delta ) > 170.0f && speed_2d > 80.0f ) || ( velocity_delta > ideal_angle && speed_2d > 80.0f ) )
					{
						target_yaw = velocity_angle + ideal_angle;
						base->set_leftmove( -1.0f );
					}
					else if ( -ideal_angle <= velocity_delta || speed_2d <= 80.0f )
					{
						if ( this->m_side_switch )
						{
							target_yaw = target_yaw - ideal_angle;
							base->set_leftmove( -1.0f );
						}
						else
						{
							target_yaw = target_yaw + ideal_angle;
							base->set_leftmove( 1.0f );
						}
					}
					else
					{
						target_yaw = velocity_angle - ideal_angle;
						base->set_leftmove( 1.0f );
					}

					math::helpers::normalize_angle( target_yaw );

					this->rotate_movement( base, target_yaw, basis_yaw );
				}
			}

			const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
			if ( !step )
			{
				continue;
			}

			step->set_button( 0 );
			step->set_pressed( false );
			step->set_when( static_cast< float >( i ) / static_cast< float >( subtick_count ) );
			step->set_analog_forward_delta( base->forwardmove( ) - last_impulses.x );
			step->set_analog_left_delta( base->leftmove( ) - last_impulses.y );

			last_impulses.x += base->forwardmove( ) - last_impulses.x;
			last_impulses.y += base->leftmove( ) - last_impulses.y;

			if ( !effective_wants_stop )
			{
				this->m_side_switch = !this->m_side_switch;
			}
		}
	}

	void airstrafe::store_angles( )
	{
		this->m_angles = systems::g_input.get_view_angles( );
	}

	void airstrafe::check_button( std::uintptr_t current_buttons, std::uintptr_t button )
	{
		constexpr auto moveleft = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveleft );
		constexpr auto moveright = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveright );
		constexpr auto forward = static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward );
		constexpr auto back = static_cast< std::uintptr_t >( cstypes::command_buttons::in_back );

		if ( current_buttons & button && ( !( this->m_last_buttons & button ) || ( button & moveleft && !( this->m_last_pressed & moveright ) ) || ( button & moveright && !( this->m_last_pressed & moveleft ) ) || ( button & forward && !( this->m_last_pressed & back ) ) || ( button & back && !( this->m_last_pressed & forward ) ) ) )
		{
			if ( button & moveleft )
			{
				this->m_last_pressed &= ~moveright;
			}
			else if ( button & moveright )
			{
				this->m_last_pressed &= ~moveleft;
			}
			else if ( button & forward )
			{
				this->m_last_pressed &= ~back;
			}
			else if ( button & back )
			{
				this->m_last_pressed &= ~forward;
			}

			this->m_last_pressed |= button;
		}
		else if ( !( current_buttons & button ) )
		{
			this->m_last_pressed &= ~button;
		}
	}

	/// Re-express "hold this movement pair while looking down `target_yaw`" as the pair to send while the
	/// command is looking down `view_yaw`. Both frames are ( forward( yaw ), left( yaw ) ), so the
	/// conversion is the rotation between them -- and then both components are negated, which is not a
	/// typo and must not be "fixed".
	///
	/// The caller only ever arrives here with forwardmove 0 and leftmove +-1 (see the branches above), so
	/// the pair is a pure side, and it is written in the CS:GO sidemove sense: positive means *right*.
	/// CS2's field is m_flCmdLeftMove, positive left. The negation is that conversion, and the arithmetic
	/// it produces is the whole point of the feature. Holding W at speed, target_yaw comes in as
	/// view + ideal_angle with leftmove +1:
	///
	///     without the negation  ->  wish sits at view + ideal + 90, i.e. 95 degrees off the velocity
	///     with it              ->  wish sits at view + ideal - 90, i.e. 85 degrees off the velocity
	///
	/// `ideal_angle` is atan( optimal_floor / speed ) -- about 5 degrees at 340 u/s -- and it is measured
	/// *from the perpendicular*, because the air-accel gain peaks just short of 90 degrees off the
	/// velocity. 85 accelerates; 95 is past perpendicular and bleeds speed. The mirrored side_switch
	/// branch lands on -85 the same way, so the alternation gains on both halves of the zig-zag.
	void airstrafe::rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const
	{
		const auto forward_move = base->forwardmove( );
		const auto side_move = base->leftmove( );

		math::vector3 target_forward{}, target_right{};
		math::helpers::angle_vectors_2d( target_yaw, target_forward, target_right );

		math::vector3 view_forward{}, view_right{};
		math::helpers::angle_vectors_2d( view_yaw, view_forward, view_right );

		const auto tf = target_forward * forward_move;
		const auto tr = target_right * side_move;

		const auto corrected_forward = view_forward.dot( tf ) + view_forward.dot( tr );
		const auto corrected_side = view_right.dot( tf ) + view_right.dot( tr );

		base->set_forwardmove( std::clamp( -corrected_forward, -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( -corrected_side, -1.0f, 1.0f ) );
	}

	void airstrafe::rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const
	{
		const auto speed = velocity.length_2d( );
		const auto wish_yaw = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / std::numbers::pi_v<float> ) + 180.0f;

		{
			const auto& ctx = features::combat::g_shared.ctx( );
			const auto max_speed = ( ctx.valid && ctx.weapon_vdata ) ? memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) ) : 250.0f;

			base->set_forwardmove( std::clamp( speed / max_speed, 0.0f, 1.0f ) );
			base->set_leftmove( 0.0f );
		}

		const auto basis_yaw = features::combat::g_misc.antiaim( ).movement_basis_yaw( base );
		const auto rotation = ( basis_yaw - wish_yaw ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto fwd = base->forwardmove( );
		const auto side = base->leftmove( );

		base->set_forwardmove( std::clamp( std::cosf( rotation ) * fwd - std::sinf( rotation ) * side, -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( ( std::sinf( rotation ) * fwd + std::cosf( rotation ) * side ) * -1.0f, -1.0f, 1.0f ) );
	}

} // namespace features::movement