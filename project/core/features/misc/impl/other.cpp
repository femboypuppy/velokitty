#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
namespace features::misc {
	namespace {

		[[nodiscard]] std::string controller_name( std::uintptr_t controller )
		{
			if ( !controller )
			{
				return {};
			}

			const auto name_ptr = memory::safe_read<std::uintptr_t>(
				controller + SCHEMA( "CCSPlayerController", "m_sSanitizedPlayerName"_hash ) ).value_or( 0 );
			return memory::read_string( name_ptr, 127 );
		}

		void submit_name_change( const std::string& display_name )
		{
			if ( display_name.empty( ) )
			{
				return;
			}

			other::s_display_name = display_name;
			other::s_name_change_pending = true;
			memory::call<void>( PATTERN( patterns::engine_client_cmd ), addresses::globals::source2engine_to_client, 0, xs( "setinfo name x" ), 0x7ffef001, std::numeric_limits<double>::quiet_NaN( ), 0ull );
			other::s_name_change_pending = false;
		}

		/// Strip everything out of a user-typed tag that has no business travelling inside a player name.
		///
		/// The tag ends up as the front of a string the game copies into the `name` userinfo value, so a
		/// control character or a stray newline would either be dropped somewhere unpredictable or cut the
		/// rest of the name off. Leading and trailing spaces go too: they are invisible in the text box,
		/// but they widen the tag on the scoreboard and make the marquee look like it stalls.
		[[nodiscard]] std::string sanitize_clantag( std::string_view raw )
		{
			std::string out{};
			out.reserve( raw.size( ) );

			for ( const auto c : raw )
			{
				const auto uc = static_cast<unsigned char>( c );
				if ( uc < 0x20 || uc == 0x7f || c == '"' )
				{
					continue;
				}

				out += c;
			}

			const auto first = out.find_first_not_of( ' ' );
			if ( first == std::string::npos )
			{
				return {};
			}

			return out.substr( first, out.find_last_not_of( ' ' ) - first + 1 );
		}

		/// The frame of the clan tag animation that belongs to the current tick.
		///
		/// Driving the phase off the game's tick counter rather than a wall clock is deliberate: the
		/// animation freezes while the game is paused or between servers, so it never burns name changes
		/// on a connection that cannot carry them, and it comes back in step instead of having run on
		/// ahead. Returns an empty string when there is nothing worth submitting, and the caller then
		/// leaves the name alone.
		[[nodiscard]] std::string build_clantag( const settings::misc::name_changer& cfg )
		{
			using style = settings::misc::name_changer::clantag_style;

			const auto text = sanitize_clantag( cfg.clantag_text.value );
			if ( text.empty( ) )
			{
				return {};
			}

			const auto mode = cfg.clantag_mode.value;

			// One step per `1 / speed` seconds. Floored at a whole tick because that is the finest grain
			// the tick counter offers, and ceilinged well short of a name change per tick -- the animation
			// re-submits the name on every step and servers do not take kindly to that rate.
			const auto steps_per_second = std::clamp( cfg.clantag_speed.value, 0.25f, 20.0f );
			const auto ticks_per_step = std::clamp( static_cast<int>( std::lroundf( 1.0f / ( steps_per_second * cstypes::tick_interval ) ) ), 1, 4096 );

			const auto global_vars = memory::safe_read<std::uintptr_t>( addresses::globals::global_vars ).value_or( 0 );
			const auto current_tick = global_vars
				? memory::safe_read<int>( global_vars + 0x44 ).value_or( 0 )
				: 0;

			std::string visible{};

			switch ( mode )
			{
			case style::typewriter:
			{
				// Type the tag out one character at a time, then take it back the same way. The phase
				// walks 0 -> len -> 0, so the count is twice the length and the second half mirrors.
				const auto length = static_cast<int>( text.size( ) );
				const auto phase_count = length * 2;

				auto phase = current_tick / ticks_per_step % phase_count;
				if ( phase < 0 )
				{
					phase += phase_count;
				}

				const auto reveal = ( phase <= length ) ? phase : phase_count - phase;
				visible = text.substr( 0, static_cast<std::size_t>( reveal ) );
				break;
			}

			case style::marquee:
			{
				// Scroll the tag through a fixed-width window. The separator is what keeps the wrap
				// readable: without it the tail of the string runs straight into its own head.
				const auto reel = text + "   ";
				const auto width = text.size( );
				const auto phase_count = static_cast<int>( reel.size( ) );

				auto phase = current_tick / ticks_per_step % phase_count;
				if ( phase < 0 )
				{
					phase += phase_count;
				}

				visible.reserve( width );
				for ( std::size_t i = 0; i < width; ++i )
				{
					visible += reel[ ( static_cast<std::size_t>( phase ) + i ) % reel.size( ) ];
				}

				// A window that has scrolled onto the separator can be all spaces, and a tag of nothing
				// but spaces is a bracket pair with a hole in it.
				if ( visible.find_first_not_of( ' ' ) == std::string::npos )
				{
					return {};
				}

				break;
			}

			case style::fixed:
			default:
				visible = text;
				break;
			}

			if ( visible.empty( ) )
			{
				return {};
			}

			if ( !cfg.clantag_brackets.value )
			{
				// Without brackets the padding the marquee window carries would just be a gap between the
				// tag and the name, so trim it back off. Inside brackets it is the thing that makes the
				// scroll legible, which is why the trim only happens here.
				return sanitize_clantag( visible );
			}

			std::string wrapped{ "[" };
			wrapped.reserve( visible.size( ) + 2 );
			wrapped += visible;
			wrapped += ']';
			return wrapped;
		}

	} // namespace

	void other::on_round_start( )
	{
		this->do_autobuy( );
	}

	void other::on_player_death( std::uintptr_t event )
	{
		if ( !event )
		{
			return;
		}

		const auto attacker_key = cstypes::event_hash{ 0, "attacker" };
		const auto attacker = memory::call<std::uintptr_t>( PATTERN (patterns::game_event_get_controller), event, &attacker_key );
	}

	void other::on_frame_stage_notify( )
	{
		this->do_player_alpha_changing( );
		this->do_reveal_radar( );
		this->do_name_changing( );
		// Viewmodel adjust lives in hooks::cheat::viewmodel_get_offset_fov: the game clamps the cvars it reads.
	}

	void other::do_reveal_radar( ) const
	{
		if ( !settings::g_misc.reveal_radar.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_valid( ) )
		{
			return;
		}

		const auto spotted_state_offset = SCHEMA( "C_CSPlayerPawn", "m_entitySpottedState"_hash );
		const auto spotted_offset = SCHEMA( "EntitySpottedState_t", "m_bSpotted"_hash );

		for ( const auto& player : systems::g_entities.get_by_type( systems::entities::type::player ) )
		{
			const auto controller = player.ptr;
			if ( !controller || !memory::read<bool>( controller + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
			{
				continue;
			}

			const auto pawn_handle = memory::read<std::uint32_t>( controller + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
			const auto pawn = systems::g_entities.lookup( pawn_handle );
			if ( !pawn || pawn == local.view_pawn( ) )
			{
				continue;
			}

			const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) )
			{
				continue;
			}

			memory::write<bool>( pawn + spotted_state_offset + spotted_offset, true );
		}
	}

	void other::do_autobuy( ) const
	{
		if ( !settings::g_misc.m_autobuy.enabled )
		{
			return;
		}

		std::string cmd {};

		switch ( settings::g_misc.m_autobuy.primary_weapon )
		{
		case 1: cmd += xs( "buy ak47; buy m4a1; " ); break;
		case 2: cmd += xs( "buy sg556; buy aug; " ); break;
		case 3: cmd += xs( "buy ssg08; " ); break;
		case 4: cmd += xs( "buy awp; " ); break;
		case 5: cmd += xs( "buy g3sg1; buy scar20; " ); break;
		}

		if ( settings::g_misc.m_autobuy.armor )
		{
			cmd += xs( "buy vesthelm; buy vest; " );
		}

		if ( settings::g_misc.m_autobuy.taser )
		{
			cmd += xs( "buy taser; " );
		}

		if ( settings::g_misc.m_autobuy.defuser )
		{
			cmd += xs( "buy defuser; " );
		}

		switch ( settings::g_misc.m_autobuy.secondary_weapon )
		{
		case 1: cmd += xs( "buy elite; " ); break;
		case 2: cmd += xs( "buy fiveseven; buy tec9; " ); break;
		case 3: cmd += xs( "buy deagle; " ); break;
		case 4: cmd += xs( "buy revolver; " ); break;
		}

		for ( auto i = 0; i < 5; ++i )
		{
			if ( !settings::g_misc.m_autobuy.grenades[ i ] )
			{
				continue;
			}

			switch ( i )
			{
			case 0: cmd += xs( "buy molotov; buy incgrenade; " ); break;
			case 1: cmd += xs( "buy hegrenade; " ); break;
			case 2: cmd += xs( "buy smokegrenade; " ); break;
			case 3: cmd += xs( "buy flashbang; " ); break;
			case 4: cmd += xs( "buy decoy; " ); break;
			}
		}

		if ( !cmd.empty( ) )
		{
			memory::call<void>(PATTERN (patterns::engine_client_cmd), addresses::globals::source2engine_to_client, 0, cmd.c_str( ), 0x7ffef001, std::numeric_limits<double>::quiet_NaN( ), 0ull );
		}
	}

	void other::do_player_alpha_changing( )
	{
		const auto local = systems::g_local.get( );

		// Every branch below either reads a schema field off local.pawn or hands it to a game function.
		// A snapshot pawn can outlive the object -- see camera::update_fov_sensitivity, where that read
		// produced a captured access violation -- and handing a dead pawn to client.dll faults inside the
		// game instead of here, which is strictly harder to diagnose. One probe up front covers the lot.
		// Dropping m_is_alpha_changed with it is correct: the object that carried the override is gone.
		const auto pawn_alive =
			local.pawn &&
			memory::safe_read<bool>( local.pawn + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) ).has_value( );
		if ( !pawn_alive )
		{
			this->m_is_alpha_changed = false;
			return;
		}

		if ( !local.is_alive )
		{
			if ( this->m_is_alpha_changed )
			{
				memory::call<void>( PATTERN (patterns::game_event_get_string), local.pawn, 255 );
			}

			this->m_is_alpha_changed = false;
			return;
		}

		if ( !settings::g_esp.m_local_alpha.enabled.value )
		{
			if ( this->m_is_alpha_changed )
			{
				this->m_is_alpha_changed = false;
				memory::call<void>( PATTERN (patterns::game_event_get_string), local.pawn, 255 );
			}

			return;
		}

		const auto is_scoped = memory::safe_read<bool>( local.pawn + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) ).value_or( false );
		const auto should_apply = !settings::g_esp.m_local_alpha.only_scoped.value || is_scoped;

		if ( should_apply )
		{
			this->m_is_alpha_changed = true;
			const auto alpha = static_cast< std::uint8_t >( settings::g_esp.m_local_alpha.opacity.value * 255.0f );
			memory::call<void>( PATTERN (patterns::game_event_get_string), local.pawn, alpha );
		}
		else
		{
			if ( this->m_is_alpha_changed )
			{
				this->m_is_alpha_changed = false;
				memory::call<void>( PATTERN (patterns::game_event_get_string), local.pawn, 255 );
			}
		}
	}

	void other::do_name_changing( )
	{
		const auto local = systems::g_local.get( );
		const auto& cfg = settings::g_misc.m_name_changer;

		// The aimwhere badge marker rides on the name, so the name changer now always runs: even with the clan
		// tag and override both off, it re-submits your real name with the marker appended. That is the whole
		// detection channel -- other users read it off your name, nothing is sent anywhere.
		const auto enabled = true;

		if ( !enabled )
		{
			if ( this->m_name_changer_active && local.controller && !this->m_original_name.empty( ) )
			{
				submit_name_change( this->m_original_name );
			}

			this->m_name_changer_active = false;
			this->m_name_changer_controller = 0;
			this->m_original_name.clear( );
			this->m_last_sent_name.clear( );
			other::s_display_name.clear( );
			return;
		}

		if ( !local.controller )
		{
			return;
		}

		if ( !this->m_name_changer_active )
		{
			this->m_original_name = controller_name( local.controller );
			if ( this->m_original_name.empty( ) )
			{
				this->m_original_name = xs( "Player" );
			}

			this->m_name_changer_active = true;
			this->m_name_changer_controller = local.controller;
			this->m_last_sent_name.clear( );
		}
		else if ( this->m_name_changer_controller != local.controller )
		{
			// Keep the captured real name across map loads, where the controller may be recreated.
			this->m_name_changer_controller = local.controller;
			this->m_last_sent_name.clear( );
		}

		const auto& configured_name = cfg.name.value;
		const auto& base_name = cfg.override_name.value && !configured_name.empty( )
			? configured_name
			: this->m_original_name;

		std::string display_name = base_name;
		if ( cfg.clantag.value )
		{
			const auto tag = build_clantag( cfg );
			if ( !tag.empty( ) )
			{
				display_name.reserve( base_name.size( ) + tag.size( ) + 1 );
				display_name = tag;
				display_name += ' ';
				display_name += base_name;
			}
		}

		// The aimwhere marker goes last, after the name and any clan tag. Strip either marker first so a
		// hidden -> visible flip replaces rather than stacks, then append whichever the self-test settled on.
		{
			const auto erase_all = [ &display_name ]( const char* m )
			{
				const std::string needle{ m };
				for ( auto pos = display_name.find( needle ); pos != std::string::npos; pos = display_name.find( needle ) )
				{
					display_name.erase( pos, needle.size( ) );
				}
			};
			erase_all( k_aimwhere_marker_hidden );
			erase_all( k_aimwhere_marker_visible );

			const std::string marker{ g_aimwhere_marker_visible.load( ) ? k_aimwhere_marker_visible : k_aimwhere_marker_hidden };

			// CS2 caps the networked name, so if the base already crowds it out, trim the base rather than drop
			// the marker -- the suffix is what makes the badge work. Keep headroom under the game's ~32 bytes.
			constexpr std::size_t k_name_budget{ 30 };
			if ( display_name.size( ) + marker.size( ) > k_name_budget && display_name.size( ) > marker.size( ) )
			{
				auto cut = k_name_budget - marker.size( );

				// Never cut inside a UTF-8 character, or a name like the CJK one would come out corrupt. The
				// continuation bytes of a character are 0b10xxxxxx; walk back off them to the lead byte.
				while ( cut > 0 && ( static_cast<unsigned char>( display_name[ cut ] ) & 0xC0 ) == 0x80 )
				{
					--cut;
				}

				display_name.resize( cut );
			}

			display_name += marker;
		}

		// With a static name there is only ever one submit, and the server can drop or overwrite it (on
		// connect, or a change sent before it accepts them). The animated clan tag hid this by resubmitting
		// constantly, which is why the badge only worked with it on. Every few seconds, check what the server
		// actually holds for us and resend if the marker is missing.
		const auto now = std::chrono::steady_clock::now( );
		if ( !this->m_last_sent_name.empty( ) && now >= this->m_next_marker_check )
		{
			this->m_next_marker_check = now + std::chrono::seconds( 5 );

			const auto networked = controller_name( local.controller );
			if ( !networked.empty( ) && !aimwhere_name_marked( networked ) )
			{
				this->m_last_sent_name.clear( );
			}
		}

		if ( display_name == this->m_last_sent_name )
		{
			return;
		}

		submit_name_change( display_name );
		this->m_last_sent_name = std::move( display_name );
	}

	void other::do_kill_feed_preservation( )
	{
		const auto local = systems::g_local.get ();

		if (!local.pawn || !local.is_alive) {
			return;
		}

		const auto hud_element = memory::call<std::uintptr_t>(PATTERN (patterns::find_hud_element), xs( "CCSGO_HudDeathNotice" ) );
		if ( !hud_element )
		{
			return;
		}

		memory::write<float> (hud_element + 0x58, settings::g_misc.preserve_killfeed ? 1000.0f : 1.5f);

		float spawntime = memory::read<float> (local.pawn + SCHEMA ("C_CSPlayerPawnBase", "m_flLastSpawnTimeIndex"_hash));
		if ( m_last_spawntime != spawntime )
		{
			const auto clear_death_notices = PATTERN (patterns::hud_death_notice_clear);
			if ( clear_death_notices )
			{
				memory::call<void>( clear_death_notices, hud_element - 0x20 );
			}

			m_last_spawntime = spawntime;
		}
	}

} // namespace features::misc
