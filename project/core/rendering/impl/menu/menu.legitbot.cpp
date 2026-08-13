#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* hitbox_names_legit[ ]{ "head", "chest", "stomach", "arms", "legs" };

	} // namespace detail

	void menu::draw_legitbot( float group_w ) const
	{
		auto& s = settings::g_combat;
		auto& lb = s.m_legitbot;
		auto& wg = lb.groups[ this->m_subtab ];

		// This page overflowed once already and the cards lost their bottom rows in silence. It scrolls
		// now (menu::page_scroll), so the old "keep every column inside 391px" budget is a guideline
		// rather than a hard wall -- scrolling is the safety net, not the intended way to reach a knob.
		const page_scroll page{ *this, "##legitbot_page" };
		const auto col_w = page.col_w( );

		if ( !lb.enabled.value )
		{
			// Nothing else on the page, so the master card takes the whole body width.
			if ( xui::begin_child( "##legitbot_master", page.full_w( ) ) )
			{
				xui::checkbox( "enabled", lb.enabled );
				xui::end_child( );
			}

			return;
		}

		if ( xui::begin_child( "##legitbot_master", col_w ) )
		{
			xui::checkbox( "enabled", lb.enabled );
			xui::end_child( );
		}

		if ( xui::begin_child( "##legitbot_aimbot", col_w ) )
		{
			xui::checkbox( "aimbot", wg.aimbot );

			xui::slider_float( "fov", wg.fov, 0.5f, 30.0f, "%.1f°" );
			xui::slider_int( "smooth", wg.smooth, 0, 100, "%d" );
			xui::multicombo( "hitboxes", wg.hitboxes, detail::hitbox_names_legit, 5 );

			xui::checkbox( "visualize fov", wg.visualize_fov );

			if ( xui::begin_popup( "##fov_color_popup", 220.0f ) )
			{
				xui::color_picker( "color##fov", wg.fov_color );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		if ( xui::begin_child( "##legitbot_rcs", col_w ) )
		{
			// Both pairs are a randomised range around 100%, not an axis split -- compute_rcs_factor
			// picks a value between them per shot so the compensation never repeats exactly.
			xui::checkbox( "recoil control", wg.rcs );
			xui::slider_int( "strength min##rcs", wg.rcs_min, 0, 200, "%d%%" );
			xui::slider_int( "strength max##rcs", wg.rcs_max, 0, 200, "%d%%" );

			xui::checkbox( "standalone rcs", wg.standalone_rcs );
			xui::slider_int( "strength##srcs", wg.standalone_rcs_strength, 0, 100, "%d%%" );
			xui::slider_int( "strength min##srcs", wg.standalone_rcs_min, 0, 200, "%d%%" );
			xui::slider_int( "strength max##srcs", wg.standalone_rcs_max, 0, 200, "%d%%" );

			xui::end_child( );
		}

		page.right( );

		if ( xui::begin_child( "##legitbot_triggerbot", col_w ) )
		{
			xui::checkbox( "triggerbot", wg.triggerbot );
			xui::slider_int( "delay", wg.trigger_delay, 0, 250, "%dms" );
			xui::slider_int( "hit chance##trig", wg.trigger_hitchance, 0, 100, "%d%%" );
			xui::checkbox( "head only", wg.trigger_head_only );
			xui::checkbox( "seed mode", wg.give_me_your_seed );

			xui::end_child( );
		}

		// The human-motion knobs live on their own card rather than behind a popup on "aimbot": the last
		// time they sat in a popup they read as missing.
		if ( xui::begin_child( "##legitbot_tracking", col_w ) )
		{
			xui::slider_int( "reaction time", wg.reaction_delay, 0, 250, "%dms" );
			xui::slider_float( "aim error", wg.aim_error, 0.0f, 2.0f, "%.2f°" );
			xui::slider_int( "backtrack", wg.backtrack_ticks, 0, 3, "%d tick(s)" );
			xui::slider_int( "hit chance##aim", wg.aim_hitchance, 0, 100, "%d%%" );

			if ( wg.aim_hitchance.value <= 0 )
			{
				xui::text( "hit chance gate off", tokens::col_text_dim );
			}

			xui::end_child( );
		}

		if ( xui::begin_child( "##legitbot_other", col_w ) )
		{
			xui::checkbox( "autowall", wg.autowall );
			xui::slider_int( "min damage##legit", wg.min_damage, 1, 125, "%d" );
			xui::checkbox( "aim through smoke", wg.aim_through_smoke );

			xui::end_child( );
		}
	}

} // namespace rendering
