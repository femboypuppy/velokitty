#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace {

		/// Component-wise rather than `a.val == b.val`. The preset table is `constexpr` and built through
		/// the r/g/b/a constructor, so `val` is the union's inactive member there -- reading it back is the
		/// kind of thing that works everywhere and is still wrong on paper.
		[[nodiscard]] bool same_color( xdraw::color a, xdraw::color b )
		{
			return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
		}

		/// In `xui::style::gradient_direction` order. The combo writes an index and `sync_theme_style`
		/// casts it, so the two lists have to stay in this order.
		constexpr const char* gradient_dir_names[ ]{ "horizontal", "vertical", "diagonal", "diagonal reverse" };

		/// The four corner colours a two-stop ramp resolves to for a given direction, matching
		/// `xui::gradient_fill` exactly -- the previews on this page paint themselves rather than going
		/// through the style, because the style is synced at the top of the frame and would leave every
		/// preview one frame behind the control that changed it.
		struct gradient_corners
		{
			xdraw::color tl, tr, br, bl;
		};

		[[nodiscard]] gradient_corners corners_for( int dir, xdraw::color a, xdraw::color b )
		{
			const auto mid = xdraw::color{
				static_cast< std::uint8_t >( ( static_cast< int >( a.r ) + b.r ) / 2 ),
				static_cast< std::uint8_t >( ( static_cast< int >( a.g ) + b.g ) / 2 ),
				static_cast< std::uint8_t >( ( static_cast< int >( a.b ) + b.b ) / 2 ),
				static_cast< std::uint8_t >( ( static_cast< int >( a.a ) + b.a ) / 2 ) };

			switch ( dir )
			{
			case 1:  return { a, a, b, b };
			case 2:  return { a, mid, b, mid };
			case 3:  return { mid, b, mid, a };
			default: return { a, b, b, a };
			}
		}

	} // namespace

	void menu::draw_theme( float group_w ) const
	{
		( void )group_w;

		auto& gui = settings::g_gui;

		// Which preset the palette *is*, or -1 once it has been hand-edited. The stored `preset` index
		// alone would lie -- it survives a picker edit, and a dot filled in while the colours no longer
		// match it is worse than no dot at all. A lambda rather than a free function because `palette` is
		// private to this class.
		const auto matching_preset = [ & ]( )
			{
				for ( auto i = 0; i < k_preset_count; ++i )
				{
					const auto& p = k_presets[ i ];

					if ( same_color( gui.accent.value, p.accent )
						&& same_color( gui.accent_2.value, p.accent_2 )
						&& same_color( gui.background.value, p.background )
						&& same_color( gui.text.value, p.text )
						&& same_color( gui.text_dim.value, p.text_dim )
						&& same_color( gui.card.value, p.card )
						&& same_color( gui.elevated.value, p.elevated ) )
					{
						return i;
					}
				}

				return -1;
			};

		const auto subtab = this->m_subtab;

		const page_scroll page{ *this, "##theme_page" };
		const auto col_w = page.col_w( );

		if ( subtab == 0 )
		{
			if ( xui::begin_child( "presets##theme_presets", col_w ) )
			{
				const auto inner_w = xui::layout::avail( ).first;
				const auto interactive = !xui::ctx( ).overlay_blocking( );
				const auto& input = xui::ctx( ).input;
				auto& dl = xui::draw::current( );

				const auto active_preset = matching_preset( );

				xui::text( active_preset >= 0 ? k_preset_names[ active_preset ] : "custom", tokens::col_text_dim );

				constexpr auto dot_h{ 28.0f };
				constexpr auto dot_gap{ 8.0f };

				const auto row = xui::layout::item( inner_w, dot_h );

				// Each dot is drawn as a squircle at radius = half its size, which is a circle, filled with
				// the preset's own two accent stops. A flat single-colour dot could not show which presets
				// have a contrasting secondary, and the secondary is half of what the gradient toggle does.
				const auto dot_w = std::min( dot_h, ( inner_w - dot_gap * ( k_preset_count - 1 ) ) / k_preset_count );

				for ( auto i = 0; i < k_preset_count; ++i )
				{
					const auto cell = xui::rect{ std::floorf( row.x + ( dot_w + dot_gap ) * i ), row.y, dot_w, dot_h };
					const auto hovered = interactive && input.in_rect( cell );

					if ( hovered && input.mouse_clicked )
					{
						this->apply_theme_preset( i );
					}

					const auto grow = xui::anim::lerp( xui::fnv1a( "theme_dot" ) + static_cast< std::uintptr_t >( i ), hovered ? 1.0f : 0.0f, 14.0f );
					const auto ring = xui::anim::lerp( xui::fnv1a( "theme_ring" ) + static_cast< std::uintptr_t >( i ), active_preset == i ? 1.0f : 0.0f, 12.0f );

					const auto inset = 4.0f - grow * 2.0f;
					const auto size = std::max( dot_w - inset * 2.0f, 2.0f );
					const auto x = cell.x + ( cell.w - size ) * 0.5f;
					const auto y = cell.y + ( cell.h - size ) * 0.5f;

					const auto& p = k_presets[ i ];
					dl.rect_filled_gradient( x, y, size, size, p.accent, p.accent_2, p.accent_2, p.accent, xdraw::corner_radius{ size * 0.5f } );

					if ( ring > 0.01f )
					{
						const auto r = std::min( cell.w, cell.h ) * 0.5f - 0.5f;
						dl.circle( cell.center_x( ), cell.center_y( ), r, tokens::col_text.alpha( static_cast< std::uint8_t >( 210.0f * ring ) ), 1.4f );
					}
				}

				xui::layout::separator( );

				xui::color_picker( "accent", gui.accent );
				xui::color_picker( "accent secondary", gui.accent_2 );
				xui::checkbox( "accent gradient", gui.accent_gradient );

				// Only offered while the gradient is on: a direction for a solid fill is a control that
				// does nothing, and a row that does nothing is worse than a row that is not there.
				if ( gui.accent_gradient.value )
				{
					xui::combo( "direction", gui.gradient_dir.value, gradient_dir_names, 4 );
				}

				xui::layout::separator( );

				xui::text( "preview", tokens::col_text_dim );

				// Reads the settings rather than `style` on purpose. The style is synced once at the top of
				// the frame, so going through `accent_fill` here would leave the preview a frame behind the
				// checkbox directly above it, which reads as the toggle being broken.
				const auto strip = xui::layout::item( inner_w, 24.0f );
				const auto strip_r = xui::ctx( ).style.button_rounding;

				if ( gui.accent_gradient.value )
				{
					const auto c = corners_for( gui.gradient_dir.value, gui.accent.value, gui.accent_2.value );
					dl.rect_filled_gradient( strip.x, strip.y, strip.w, strip.h, c.tl, c.tr, c.br, c.bl, xdraw::corner_radius{ strip_r } );
				}
				else
				{
					dl.rect_filled( strip.x, strip.y, strip.w, strip.h, gui.accent.value, xdraw::corner_radius{ strip_r } );
				}

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "palette##theme_palette", col_w ) )
			{
				xui::text( "palette", tokens::col_text_dim );

				xui::layout::separator( );

				xui::color_picker( "background", gui.background );
				xui::color_picker( "card", gui.card );
				xui::color_picker( "elevated", gui.elevated );
				xui::color_picker( "text", gui.text );
				xui::color_picker( "text dim", gui.text_dim );

				xui::layout::separator( );

				// The alpha channels on card and elevated are what make the menu translucent, so a picker
				// dragged to 255 there is a legitimate opaque-panel choice rather than a mistake. Worth
				// saying, because the opacity slider on the next page then appears to do nothing.
				xui::text( "card and elevated carry the panel alpha", tokens::col_text_dim );

				xui::end_child( );
			}
		}
		else if ( subtab == 1 )
		{
			if ( xui::begin_child( "menu##theme_menu", col_w ) )
			{
				xui::text( "menu", tokens::col_text_dim );

				xui::layout::separator( );

				xui::slider_int( "opacity", gui.opacity, 10, 100, "%d%%" );
				xui::slider_float( "animation speed", gui.anim_speed, 0.2f, 3.0f, "%.2fx" );
				xui::slider_float( "rounding", gui.rounding, 0.0f, 2.0f, "%.2fx" );

				xui::layout::separator( );

				xui::text( "world", tokens::col_text_dim );

				xui::checkbox( "esp gradient", gui.esp_gradient );

				// Naming the surface it touches, because "esp gradient" on its own promises the whole esp.
				// It rides the box fill only: the outline is one pixel wide, where a vertical ramp shows on
				// the two side edges and nowhere else, and the health and ammo bars already own a gradient
				// that ramps by health rather than by theme -- overwriting that would cost a real feature.
				xui::text( "box fill fades into the secondary accent", tokens::col_text_dim );

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "preview##theme_widget_preview", col_w ) )
			{
				xui::text( "live preview", tokens::col_text_dim );

				xui::layout::separator( );

				// Hand-drawn lookalikes rather than real widgets. A real checkbox needs a `xui::setting` to
				// bind to, and a throwaway one would register itself with the bind system and show up in the
				// keybind list as a nameless row. These read the same `style` the real widgets do, so
				// rounding, palette and gradient all land here the moment they land there.
				const auto inner_w = xui::layout::avail( ).first;
				const auto& st = xui::ctx( ).style;
				auto& dl = xui::draw::current( );

				{
					const auto box = xui::layout::item( inner_w, st.checkbox_size );
					const auto s = st.checkbox_size;

					dl.rect_filled( box.x, box.y, s, s, st.checkbox_bg, xdraw::corner_radius{ st.checkbox_rounding } );
					xui::accent_fill( dl, box.x + 3.0f, box.y + 3.0f, s - 6.0f, s - 6.0f, st.checkbox_mark, std::max( st.checkbox_rounding - 1.5f, 0.0f ) );

					const auto th = xdraw::measure_text( "checkbox" ).second;
					dl.text( std::floorf( box.x + s + 8.0f ), std::floorf( box.y + ( s - th ) * 0.5f ), "checkbox", st.text );
				}

				{
					const auto track = xui::layout::item( inner_w, st.slider_h );

					dl.rect_filled( track.x, track.y, track.w, st.slider_h, st.slider_track, xdraw::corner_radius{ st.slider_rounding } );
					xui::accent_fill( dl, track.x, track.y, track.w * 0.62f, st.slider_h, st.slider_fill, st.slider_rounding );
				}

				{
					const auto btn = xui::layout::item( std::max( inner_w * 0.55f, 60.0f ), 26.0f );

					xui::accent_fill( dl, btn.x, btn.y, btn.w, btn.h, tokens::col_accent, st.button_rounding );

					const auto [ tw, th ] = xdraw::measure_text( "button" );
					dl.text( std::floorf( btn.x + ( btn.w - tw ) * 0.5f ), std::floorf( btn.y + ( btn.h - th ) * 0.5f ), "button", tokens::col_dark );
				}

				xui::layout::separator( );

				xui::text( "opacity fades panels, not text", tokens::col_text_dim );

				xui::end_child( );
			}
		}
		else if ( subtab == 2 )
		{
			if ( xui::begin_child( "layout##theme_layout", col_w ) )
			{
				xui::text( "window", tokens::col_text_dim );

				xui::layout::separator( );

				xui::slider_float( "width", gui.menu_w, k_min_menu_w, k_max_menu_w, "%.0fpx" );
				xui::slider_float( "height", gui.menu_h, k_min_menu_h, k_max_menu_h, "%.0fpx" );
				xui::checkbox( "drag to resize", gui.menu_resizable );

				// Naming the corner, because a 16px grip with no chrome of its own is not discoverable.
				xui::text( "grip sits in the bottom right corner", tokens::col_text_dim );

				xui::layout::separator( );

				xui::text( "spacing", tokens::col_text_dim );

				xui::slider_float( "density", gui.density, 0.6f, 1.6f, "%.2fx" );

				// The honest way to say what density buys: it is the setting that decides whether a page
				// needs its scrollbar at all.
				xui::text( "under 1.0x fits more rows per page", tokens::col_text_dim );

				xui::end_child( );
			}

			page.right( );

			if ( xui::begin_child( "chrome##theme_chrome", col_w ) )
			{
				xui::text( "chrome", tokens::col_text_dim );

				xui::layout::separator( );

				xui::slider_float( "border", gui.border, 0.0f, 3.0f, "%.1fpx" );
				xui::color_picker( "border color", gui.border_color );
				xui::checkbox( "legacy skin", gui.legacy_skin );
				xui::checkbox( "background blur", gui.window_blur );

				if ( !gui.window_blur.value )
				{
					// The blur is what makes the panel alphas read as frosted glass. Without it the menu
					// sits on a flat plate, so a palette tuned against the blur will look wrong here.
					xui::text( "blur off draws an opaque backdrop", tokens::col_text_dim );
				}

				xui::layout::separator( );

				const auto inner_w = xui::layout::avail( ).first;

				if ( xui::button( "reset layout", inner_w, 26.0f ) )
				{
					gui.menu_w.value = 700.0f;
					gui.menu_h.value = 450.0f;
					gui.density.value = 1.0f;
					gui.border.value = 0.0f;
					gui.opacity.value = 100;
					gui.rounding.value = 1.0f;
					gui.anim_speed.value = 1.0f;
				}

				// Deliberately not the palette: someone who has spent ten minutes on their colours and wants
				// their spacing back should not lose the colours to get it.
				xui::text( "geometry only, palette untouched", tokens::col_text_dim );

				xui::end_child( );
			}
		}
	}

} // namespace rendering
