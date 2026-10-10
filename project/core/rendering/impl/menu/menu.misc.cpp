#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* sound_types[ ]{ "shop click", "home click", "bell", "killcard", "bullet casing", "coin pickup", "item drop", "popcan", "key press", "custom" };
		constexpr auto k_sound_type_count{ static_cast< int >( std::size( sound_types ) ) };

		void draw_custom_sound_picker( config::str& file_setting, std::string_view combo_label, std::string_view preview_id, float preview_volume )
		{
			const auto files = features::misc::impacts::list_custom_sounds( );

			static std::vector<std::string> cached_files{};
			static std::vector<const char*> cached_ptrs{};
			cached_files = files;
			cached_ptrs.clear( );
			cached_ptrs.reserve( cached_files.size( ) );

			for ( const auto& file : cached_files )
			{
				cached_ptrs.push_back( file.c_str( ) );
			}

			if ( !cached_ptrs.empty( ) )
			{
				auto selected{ 0 };
				for ( auto i = 0; i < static_cast< int >( cached_files.size( ) ); ++i )
				{
					if ( cached_files[ static_cast< std::size_t >( i ) ] == file_setting.value )
					{
						selected = i;
						break;
					}
				}

				if ( xui::combo( combo_label, selected, cached_ptrs.data( ), static_cast< int >( cached_ptrs.size( ) ) ) )
				{
					file_setting = cached_files[ static_cast< std::size_t >( selected ) ];
				}
			}

			xui::text_input( "file", file_setting.value, 64, "hit.wav" );

			if ( xui::button( preview_id, 96.0f, 22.0f ) )
			{
				features::misc::g_impacts.play_custom_sound( file_setting.value, preview_volume );
			}
		}
		constexpr const char* marker_types[ ]{ "classic", "damage", "both" };
		constexpr const char* impact_types[ ]{ "overlay", "sparks", "both" };

		constexpr const char* primary_weapons[ ]{ "none", "rifle", "scoped rifle", "scout", "awp", "auto sniper" };
		constexpr const char* secondary_weapons[ ]{ "none", "dual elites", "five-seven/tec-9", "deagle", "revolver" };
		constexpr const char* grenade_names[ ]{ "molotov", "he grenade", "smoke", "flashbang", "decoy" };

		constexpr const char* hat_types[ ]{ "kasa", "bucket" };

		// Order matches settings::misc::name_changer::clantag_style.
		constexpr const char* clantag_styles[ ]{ "fixed", "typewriter", "marquee" };

	} // namespace detail

	void menu::draw_misc( float group_w ) const
	{
		auto& m = settings::g_misc;

		const auto subtab = this->m_subtab;

		// The impacts card alone is taller than the body. Inside the scroll region the column keeps its
		// full height and scrolls; before, the rows past the edge were clipped. See menu::page_scroll.
		const page_scroll page{ *this, "##misc_page" };
		const auto col_w = page.col_w( );

		if ( subtab == 0 )
		{
			auto& impacts = m.m_impacts;
			auto& traj = m.m_projectile_trajectory;
			auto& dlights = m.m_dlight;
			auto& pen = settings::g_combat.m_penetration_crosshair;
			auto& mov = settings::g_movement;
			auto& ab = m.m_autobuy;

			if ( xui::begin_child( "impacts##misc_impacts", col_w ) )
			{
				xui::checkbox( "hit logs", impacts.hit_log );
				if ( xui::begin_popup( "##hitlog_popup", 220.0f ) )
				{
					xui::slider_float( "duration##hl", impacts.hit_log_duration, 0.5f, 10.0f, "%.1fs" );
					xui::end_popup( );
				}

				xui::checkbox( "console logs", impacts.console_log );
				xui::checkbox( "chat logs", impacts.chat_log );

				xui::checkbox( "miss logs", impacts.miss_log );
				if ( xui::begin_popup( "##misslog_popup", 220.0f ) )
				{
					xui::slider_float( "duration##ml", impacts.miss_log_duration, 0.5f, 10.0f, "%.1fs" );
					xui::end_popup( );
				}

				xui::checkbox( "hit sound", impacts.hit_sound );
				if ( xui::begin_popup( "##hitsound_popup", 220.0f ) )
				{
					xui::combo( "type##hs", impacts.hit_sound_type.value, detail::sound_types, detail::k_sound_type_count );

					xui::slider_float( "volume##hs", impacts.hit_sound_volume, 1.0f, 100.0f, "%.0f%%" );

					if ( impacts.hit_sound_type.value == settings::misc::impacts::sound_type::custom )
					{
						detail::draw_custom_sound_picker( impacts.custom_hit_sound, "sound##hs", "preview##hs", impacts.hit_sound_volume.value );
					}

					xui::end_popup( );
				}

				xui::checkbox( "hit marker", impacts.hit_marker );
				if ( xui::begin_popup( "##hitmarker_popup", 220.0f ) )
				{
					xui::combo( "type##hm", impacts.hit_marker_type.value, detail::marker_types, 3 );

					xui::slider_float( "duration##hm", impacts.hit_marker_duration, 0.1f, 5.0f, "%.1fs" );
					xui::color_picker( "color##hm", impacts.hit_marker_color );
					xui::end_popup( );
				}

				xui::checkbox( "hit effect", impacts.hit_effect );
				if ( xui::begin_popup( "##hitfx_popup", 220.0f ) )
				{
					xui::color_picker( "color##hitfx", impacts.hit_effect_color );
					xui::slider_float( "duration##hitfx", impacts.hit_effect_duration, 0.1f, 5.0f, "%.1fs" );
					xui::slider_float( "strength##hitfx", impacts.hit_effect_strength, 1.0f, 100.0f, "%.0f%%" );
					xui::end_popup( );
				}

				xui::checkbox( "death sound", impacts.death_sound );
				if ( xui::begin_popup( "##deathsound_popup", 220.0f ) )
				{
					xui::combo( "type##ds", impacts.death_sound_type.value, detail::sound_types, detail::k_sound_type_count );

					xui::slider_float( "volume##ds", impacts.death_sound_volume, 1.0f, 100.0f, "%.0f%%" );

					if ( impacts.death_sound_type.value == settings::misc::impacts::sound_type::custom )
					{
						detail::draw_custom_sound_picker( impacts.custom_death_sound, "sound##ds", "preview##ds", impacts.death_sound_volume.value );
					}

					xui::end_popup( );
				}

				xui::checkbox( "death effect", impacts.death_effect );
				if ( xui::begin_popup( "##deathfx_popup", 220.0f ) )
				{
					xui::color_picker( "color##deathfx", impacts.death_effect_color );
					xui::end_popup( );
				}

				xui::checkbox( "discord rich presence", m.m_discord_rpc.enabled );

				xui::checkbox ("scoreboard weapons", m.m_scoreboard_weapons.enabled);
				if (xui::begin_popup ("##scoreboardeq_popup", 220.0f)) {
					xui::color_picker ("color##scoreboardeq", m.m_scoreboard_weapons.color);
					xui::end_popup ();
				}

				xui::checkbox( "bullet impacts", impacts.bullet_impact_effect );
				if ( xui::begin_popup( "##bulletfx_popup", 220.0f ) )
				{
					xui::combo( "type##bulletfx", impacts.bullet_impact_effect_type.value, detail::impact_types, 3 );

					const auto type = impacts.bullet_impact_effect_type.value;
					const auto show_overlay = type == settings::misc::impacts::bullet_impact_type::overlay || type == settings::misc::impacts::bullet_impact_type::both;
					const auto show_sparks = type == settings::misc::impacts::bullet_impact_type::sparks || type == settings::misc::impacts::bullet_impact_type::both;

					if ( show_overlay )
					{
						xui::slider_float( "duration##bulletfx", impacts.bullet_impact_effect_duration, 0.1f, 5.0f, "%.1fs" );
						xui::color_picker( "fill##bulletfx", impacts.bullet_impact_effect_fill_color );
						xui::color_picker( "edge##bulletfx", impacts.bullet_impact_effect_edge_color );

						xui::checkbox( "glow##bulletfx", impacts.bullet_impact_effect_glow );
						if ( impacts.bullet_impact_effect_glow )
						{
							xui::slider_float( "glow strength##bulletfx", impacts.bullet_impact_effect_glow_strength, 0.1f, 1.0f, "%.2f" );
						}
					}

					if ( show_sparks )
					{
						xui::color_picker( "spark##bulletfx", impacts.bullet_impact_effect_color_spark );
					}

					xui::end_popup( );
				}

				xui::checkbox( "bullet tracers", impacts.bullet_tracers );
				if ( xui::begin_popup( "##tracers_popup", 220.0f ) )
				{
					xui::slider_float( "duration##tracer", impacts.bullet_tracer_duration, 0.1f, 5.0f, "%.1fs" );
					xui::color_picker( "color##tracer", impacts.bullet_tracer_color );
					xui::end_popup( );
				}

				xui::end_child( );
			}

			if ( xui::begin_child( "visuals##misc_visuals", col_w ) )
			{
				xui::checkbox( "projectile trajectory", traj.enabled );
				if ( xui::begin_popup( "##traj_popup", 220.0f ) )
				{
					xui::checkbox( "straight throw", traj.straight_throw );
					xui::color_picker( "held color", traj.held_color );
					xui::color_picker( "thrown color", traj.thrown_color );
					xui::color_picker( "will damage held color", traj.will_deal_damage_held_color );
					xui::color_picker( "will damage thrown color", traj.will_deal_damage_thrown_color );
					xui::end_popup( );
				}

				xui::checkbox( "dynamic light", dlights.enabled );
				if ( xui::begin_popup( "##dlight_popup", 220.0f ) )
				{
					xui::color_picker( "color##dl", dlights.color );
					xui::slider_float( "radius##dl", dlights.radius, 50.0f, 15000.0f, "%.0f" );
					xui::slider_float( "z offset##dl", dlights.z_offset, 0.0f, 100.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::checkbox( "penetration crosshair", pen.enabled );
				if ( xui::begin_popup( "##pen_popup", 220.0f ) )
				{
					xui::checkbox( "glow##pen", pen.glow );
					xui::slider_float( "glow strength##pen", pen.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::color_picker( "can penetrate##pen", pen.can_penetrate_fill );
					xui::color_picker( "can pen outline##pen", pen.can_penetrate_outline );
					xui::color_picker( "blocked##pen", pen.blocked_fill );
					xui::color_picker( "blocked outline##pen", pen.blocked_outline );
					xui::end_popup( );
				}

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "movement##misc_movement", col_w ) )
			{
				xui::checkbox( "bhop", mov.bhop );
				xui::checkbox( "airstrafe", mov.airstrafe );

				if ( xui::begin_popup( "##airstrafe_popup", 220.0f ) )
				{
					xui::checkbox( "fully directional", mov.airstrafe_fully_directional );
					xui::checkbox( "speed cap##as", mov.strafe_speed_cap );
					xui::slider_float( "cap##as", mov.strafe_speed_cap_percent, 90.0f, 130.0f, "%.0f%% of max" );
					xui::end_popup( );
				}

				xui::checkbox( "auto strafe", mov.m_test_strafer.enabled );

				// Same setting as in the airstrafe popup: one cap, whichever strafer is running.
				if ( xui::begin_popup( "##autostrafe_popup", 220.0f ) )
				{
					xui::checkbox( "speed cap##ts", mov.strafe_speed_cap );
					xui::slider_float( "cap##ts", mov.strafe_speed_cap_percent, 90.0f, 130.0f, "%.0f%% of max" );
					xui::end_popup( );
				}

				// The quantized maths is meaningless when the server is not quantizing, and its yaw steps are
				// ignored when the server drops subtick view angles. Airstrafe strafes in its place there, so
				// say which one is running instead of looking enabled and doing nothing.
				if ( mov.m_test_strafer.enabled.value && !features::movement::g_test_strafer.is_active( ) )
				{
					xui::text( "server blocks it, using airstrafe", tokens::col_text_dim );
				}

				xui::checkbox( "jumpbug", mov.jumpbug );
				xui::checkbox( "fastladder", mov.fastladder );
				xui::checkbox( "edgejump", mov.edgejump );
				xui::checkbox( "edgestop", mov.edgestop );
				xui::checkbox( "edgebug", mov.edgebug );
				if ( xui::begin_popup( "##edgebug_popup", 240.0f ) )
				{
					static const char* edgebug_modes[] = { "0: loose", "1: edge trace (default)", "2: no jump held", "3: min speed", "4: strict vz" };
					xui::combo( "mode##eb", mov.edgebug_mode.value, edgebug_modes, 5 );
					xui::slider_int( "passes##eb", mov.edgebug_passes, 1, 5, "%d" );
					xui::checkbox( "jump steps##eb", mov.edgebug_include_jump_steps );
					xui::end_popup( );
				}
				xui::checkbox( "slowwalk", mov.slowwalk );

				if ( xui::begin_popup( "##slowwalk_popup", 220.0f ) )
				{
					xui::slider_float( "speed", mov.slowwalk_speed, 1.0f, 100.0f, "%.2fs" );
					xui::end_popup( );
				}


				xui::end_child( );
			}

			if ( xui::begin_child( "other##misc_other", col_w ) )
			{
				xui::checkbox( "reveal radar", m.reveal_radar );
				xui::checkbox( "preserve killfeed", m.preserve_killfeed );
				xui::checkbox( "disable game logs", m.disable_game_logs );

				xui::checkbox( "auto buy", ab.enabled );
				if ( xui::begin_popup( "##autobuy_popup", 220.0f ) )
				{
					xui::combo( "primary##ab", ab.primary_weapon, detail::primary_weapons, 6 );
					xui::combo( "secondary##ab", ab.secondary_weapon, detail::secondary_weapons, 5 );
					xui::checkbox( "armor##ab", ab.armor );
					xui::checkbox( "defuser##ab", ab.defuser );
					xui::checkbox( "taser##ab", ab.taser );
					xui::multicombo( "grenades##ab", ab.grenades, detail::grenade_names, 5 );
					xui::end_popup( );
				}

				xui::checkbox( "clantag", m.m_name_changer.clantag );
				if ( xui::begin_popup( "##clantag_popup", 240.0f ) )
				{
					xui::text_input( "tag##ct", m.m_name_changer.clantag_text.value, 24, "aimwhere" );
					xui::combo( "style##ct", m.m_name_changer.clantag_mode.value, detail::clantag_styles, 3 );
					xui::checkbox( "brackets##ct", m.m_name_changer.clantag_brackets );

					// Only the animated styles step; a fixed tag is submitted once and then sits there, so
					// a speed slider under it would read as a control that does nothing.
					if ( m.m_name_changer.clantag_mode.value != settings::misc::name_changer::clantag_style::fixed )
					{
						xui::slider_float( "speed##ct", m.m_name_changer.clantag_speed, 0.25f, 20.0f, "%.2f/s" );
					}

					xui::end_popup( );
				}

				xui::checkbox( "override name", m.m_name_changer.override_name );
				if ( xui::begin_popup( "##override_name_popup", 220.0f ) )
				{
					xui::text_input( "name##nc", m.m_name_changer.name.value, 32, "player name..." );
					xui::end_popup( );
				}

				xui::checkbox( "watermark", m.m_watermark.enabled );
				if ( xui::begin_popup( "##watermark_popup", 200.0f ) )
				{
					xui::checkbox( "fps##wm",      m.m_watermark.show_fps );
					xui::checkbox( "ping##wm",     m.m_watermark.show_ping );
					xui::checkbox( "time##wm",     m.m_watermark.show_time );
					xui::checkbox( "user##wm",     m.m_watermark.show_user );
					xui::checkbox( "map##wm",      m.m_watermark.show_map );
					xui::checkbox( "tick rate##wm",m.m_watermark.show_tick );
					xui::checkbox( "velocity##wm", m.m_watermark.show_velocity );
					xui::end_popup( );
				}

				xui::end_child( );
			}
		}

		if ( subtab == 1 )
		{
			auto& rem = m.m_removals;

			if ( xui::begin_child( "removals##misc_removals", col_w ) )
			{
				xui::checkbox( "remove crosshair", rem.crosshair );
				xui::checkbox( "remove scope", rem.scope );
				xui::checkbox( "remove overhead", rem.overhead );
				xui::checkbox( "remove legs", rem.legs );
				xui::checkbox( "remove recoil", rem.recoil );
				xui::checkbox( "remove skybox fog", rem.skybox_fog );
				xui::checkbox( "remove 3d skybox", rem.skybox_3d );
				xui::checkbox( "remove decals", rem.decals );
				xui::checkbox( "remove smoke", rem.smoke );
				xui::slider_float( "flash alpha##flash", rem.flash_alpha, 0.0f, 100.0f, "%.0f%%" );

				xui::end_child( );
			}
		}

		if ( subtab == 2 )
		{
			auto& cam = m.m_camera;
			auto& vm = m.m_viewmodel_adjust;

			if ( xui::begin_child( "camera##misc_camera", col_w ) )
			{
				xui::checkbox( "custom fov", cam.change_fov );
				if ( xui::begin_popup( "##fov_popup", 220.0f ) )
				{
					xui::slider_float( "fov", cam.fov, 60.0f, 150.0f, "%.0f" );
					xui::checkbox( "scoped fov override", cam.scoped_fov_override );
					xui::slider_float( "scoped fov", cam.scoped_fov, 10.0f, 90.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::checkbox( "thirdperson", cam.thirdperson );
				if ( xui::begin_popup( "##tp_popup", 220.0f ) )
				{
					xui::slider_float( "distance", cam.thirdperson_distance, 35.0f, 200.0f, "%.0f" );
					xui::slider_float( "hull size", cam.thirdperson_hull_size, 0.0f, 20.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::checkbox( "aspect ratio", cam.change_aspect_ratio );
				if ( xui::begin_popup( "##ar_popup", 220.0f ) )
				{
					xui::slider_float( "ratio##ar", cam.aspect_ratio, 1.0f, 1.78f, "%.3f" );
					xui::end_popup( );
				}

				// The player picker opens beside the menu while this is ticked.
				xui::checkbox( "spectate", cam.spectate );

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "viewmodel##misc_viewmodel", col_w ) )
			{
				xui::checkbox( "viewmodel adjust", vm.enabled );
				if ( xui::begin_popup( "##vm_popup", 220.0f ) )
				{
					xui::slider_float( "offset x", vm.offset_x, -10.0f, 10.0f, "%.1f" );
					xui::slider_float( "offset y", vm.offset_y, -10.0f, 10.0f, "%.1f" );
					xui::slider_float( "offset z", vm.offset_z, -10.0f, 10.0f, "%.1f" );
					xui::slider_float( "fov", vm.fov, 30.0f, 90.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::end_child( );
			}
		}

		if ( subtab == 3 )
		{
			auto& hud = m.m_hud;

			if ( xui::begin_child( "hud##misc_hud", col_w ) )
			{
				xui::checkbox( "crosshair overlay", hud.m_crosshair.enabled );
				if ( xui::begin_popup( "##xhair_popup", 220.0f ) )
				{
					xui::slider_float( "size##xhair", hud.m_crosshair.size, 0.5f, 10.0f, "%.1f" );
					xui::slider_float( "outline##xhair", hud.m_crosshair.outline, 0.0f, 4.0f, "%.1f" );
					xui::color_picker( "color##xhair", hud.m_crosshair.color );
					xui::color_picker( "outline color##xhair", hud.m_crosshair.outline_color );
					xui::end_popup( );
				}

				xui::checkbox( "scope overlay", hud.m_scope.enabled );
				if ( xui::begin_popup( "##scope_popup", 220.0f ) )
				{
					xui::slider_float( "line length", hud.m_scope.line_length, 10.0f, 500.0f, "%.0f" );
					xui::slider_float( "gap##scope", hud.m_scope.gap, 0.0f, 50.0f, "%.0f" );
					xui::slider_float( "thickness##scope", hud.m_scope.thickness, 0.5f, 5.0f, "%.2f" );
					xui::slider_float( "anim speed", hud.m_scope.anim_speed, 1.0f, 30.0f, "%.0f" );
					xui::color_picker( "color##scope", hud.m_scope.color );
					xui::checkbox( "fade in##scope", hud.m_scope.fade_in );

					xui::layout::separator( );

					xui::checkbox( "glow##scope", hud.m_scope.glow );
					xui::slider_float( "glow strength##scope", hud.m_scope.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::checkbox( "velocity counter", hud.m_velocity.counter );
				xui::checkbox( "velocity chart", hud.m_velocity.chart );
				if ( xui::begin_popup( "##velocity_hud_popup", 220.0f ) )
				{
					xui::color_picker( "color##velocity", hud.m_velocity.color );
					xui::slider_float( "bottom offset", hud.m_velocity.bottom_offset, 20.0f, 200.0f, "%.0f" );
					xui::slider_float( "chart width", hud.m_velocity.chart_width, 120.0f, 320.0f, "%.0f" );
					xui::slider_float( "chart height", hud.m_velocity.chart_height, 24.0f, 80.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "hat##misc_hud_hat", col_w ) )
			{
				xui::checkbox( "hat", hud.m_hat.enabled );
				if ( xui::begin_popup( "##hat_popup", 220.0f ) )
				{
					xui::combo( "type##hat", hud.m_hat.type.value, detail::hat_types, 2 );
					xui::color_picker( "color##hat", hud.m_hat.color );
					xui::color_picker( "secondary color##hat", hud.m_hat.secondary_color );
					xui::checkbox( "glow##hat", hud.m_hat.glow );
					xui::slider_float( "glow strength##hat", hud.m_hat.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::end_child( );
			}
		}
	}

	void menu::draw_spectate_window( float reveal )
	{
		if ( !settings::g_misc.m_camera.spectate.value )
		{
			return;
		}

		constexpr auto k_w{ 230.0f };
		constexpr auto k_h{ 330.0f };

		// First open: dock against the menu's right edge, or its left one if that would leave the screen. After
		// that it stays wherever it was dragged.
		if ( this->m_spec_x < 0.0f )
		{
			const auto [screen_w, screen_h] = xdraw::viewport_size( );
			this->m_spec_x = this->m_x + this->m_w + 8.0f;
			if ( this->m_spec_x + k_w > static_cast< float >( screen_w ) )
			{
				this->m_spec_x = std::max( 0.0f, this->m_x - k_w - 8.0f );
			}

			this->m_spec_y = this->m_y;
		}

		auto w = k_w;
		auto h = k_h;
		if ( !xui::begin_window( "##spectate", this->m_spec_x, this->m_spec_y, w, h, false, k_w, k_h, reveal ) )
		{
			return;
		}

		const auto& style = xui::ctx( ).style;
		auto& cam = features::misc::g_camera;

		if ( xui::begin_child( "spectate##spectate_list", k_w - style.window_pad_x * 2.0f, k_h - style.window_pad_y * 2.0f, true ) )
		{
			struct row
			{
				std::uintptr_t controller{};
				std::string name{};
				bool enemy{};
				bool alive{};
			};

			std::vector<row> rows{};
			const auto local = systems::g_local.get( );

			for ( const auto& player : systems::g_entities.get_by_type( systems::entities::type::player ) )
			{
				if ( !player.ptr || player.ptr == local.controller )
				{
					continue;
				}

				// Unassigned and spectator slots have no pawn to look through.
				const auto team = memory::read<int>( player.ptr + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
				if ( team < 2 )
				{
					continue;
				}

				const auto name_ptr = memory::read<std::uintptr_t>( player.ptr + SCHEMA( "CCSPlayerController", "m_sSanitizedPlayerName"_hash ) );

				row r{};
				r.controller = player.ptr;
				r.name = name_ptr ? memory::read_string( name_ptr, 63 ) : std::string{ "?" };
				r.enemy = local.is_this_other_team( team );
				r.alive = memory::read<bool>( player.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) );
				rows.push_back( std::move( r ) );
			}

			const auto current = cam.spectate_target( );
			const auto [avail_w, avail_h] = xui::layout::avail( );

			if ( current && xui::button( "stop spectating##spec_stop", avail_w, 20.0f ) )
			{
				cam.set_spectate_target( 0 );
			}

			const auto draw_group = [ & ]( bool enemies )
				{
					xui::text( enemies ? "enemies" : "teammates", enemies ? xdraw::color{ 235, 110, 110, 255 } : xdraw::color{ 120, 170, 240, 255 } );

					auto any{ false };
					for ( const auto& r : rows )
					{
						if ( r.enemy != enemies )
						{
							continue;
						}

						any = true;

						// Picking the one already watched goes back to your own view.
						auto label = std::string( r.controller == current ? "> " : "" ) + std::string( xui::truncate( r.name, avail_w - 60.0f ) );
						if ( !r.alive )
						{
							label += " (dead)";
						}

						label += "##spec_" + std::to_string( r.controller );

						if ( xui::button( label, avail_w, 20.0f ) )
						{
							cam.set_spectate_target( r.controller == current ? 0 : r.controller );
						}
					}

					if ( !any )
					{
						xui::text( "none", tokens::col_text_dim );
					}
				};

			draw_group( true );
			xui::layout::spacing( 4.0f );
			draw_group( false );

			xui::end_child( );
		}

		xui::end_window( );
	}

} // namespace rendering