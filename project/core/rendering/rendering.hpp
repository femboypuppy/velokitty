#pragma once

#include <string>
#include <vector>

namespace rendering {

	/// The aimwhere mark: crosshair ring and ticks around a question mark, one colour, drawn white on a
	/// 32-unit canvas so a tint colours it. This is the heavy-stroke cut that stays legible down to
	/// 12px -- the menu pill, the watermark and the ESP badge all load it at their own size.
	namespace brand {

		inline constexpr auto mark = R"(<svg width="32" height="32" viewBox="0 0 32 32" xmlns="http://www.w3.org/2000/svg"><path d="M27.3 19.6A12 12 0 0 1 19.6 27.3M12.4 27.3A12 12 0 0 1 4.7 19.6M4.7 12.4A12 12 0 0 1 12.4 4.7M19.6 4.7A12 12 0 0 1 27.3 12.4M16 2V6.2M16 25.8V30M2 16H6.2M25.8 16H30" stroke="white" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round" fill="none"/><path d="M12.7 13.3C12.7 11.3 14.2 10 16 10C17.8 10 19.3 11.2 19.3 13C19.3 14.6 18.2 15.3 17.1 16C16.4 16.5 16 17 16 18" stroke="white" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round" fill="none"/><circle cx="16" cy="21.9" r="1.8" fill="white"/></svg>)";

	} // namespace brand

	class context
	{
	public:
		bool initialize( IDXGISwapChain* swap_chain );
		void shutdown( );
		void on_present( IDXGISwapChain* swap_chain );
		void on_resize_buffers( );
		void on_resize_buffers_post( IDXGISwapChain* swap_chain );

		[[nodiscard]] HWND get_window( ) const { return this->m_window; }
		[[nodiscard]] ID3D11Device* get_device( ) const { return this->m_device; }
		[[nodiscard]] ID3D11DeviceContext* get_context( ) const { return this->m_context; }
		[[nodiscard]] bool is_initialized( ) const { return this->m_initialized; }
		[[nodiscard]] bool ui_assets_ready( ) const { return this->m_ui_assets_ready; }
        ID3D11RenderTargetView* get_rtv( ) const { return this->m_rtv; }

	private:
		void create_rtv( IDXGISwapChain* swap_chain );
		void setup_zdraw( HWND window );
		bool try_bind_ui_assets( );

		ID3D11Device* m_device{ nullptr };
		ID3D11DeviceContext* m_context{ nullptr };
		ID3D11RenderTargetView* m_rtv{ nullptr };
		HWND m_window{ nullptr };
		bool m_initialized{ false };
		bool m_ui_assets_ready{ false };
	};

    class menu
    {
    public:
        void initialize_graphics( );

        void draw( );
        void shutdown( ) const;

        void toggle( ) { this->m_open = !this->m_open; }
		[[nodiscard]] bool is_open( ) const { return this->m_open; }
		void apply_saved_cursor( );

        enum class tab : int
        {
            ragebot, legitbot, player, world, skins, misc, config, theme, count
        };

    private:
		bool draw_intro( );
        void draw_side_bar( float h );
        void draw_legacy_tabs( float h );
        void draw_top_bar( float w );
        void try_load_user_avatar( );
        void apply_theme_preset( int preset ) const;
        void sync_theme_style( ) const;
        void draw_search_results( float x, float y, float w, float h );
        void rebuild_search_index( );
        void close_search( );
        void activate_search_result( std::size_t index );

        void draw_ragebot( float group_w ) const;
        void draw_legitbot( float group_w ) const;
        void draw_player( float group_w ) const;
        void draw_world( float group_w ) const;
        void draw_skins( float group_w ) const;
        void draw_misc( float group_w ) const;
        void draw_config( float group_w );
        void draw_theme( float group_w ) const;
        void draw_unload( );

        /// The spectate player picker: a small window of its own beside the menu, shown while
        /// misc > camera > spectate is ticked.
        void draw_spectate_window( float reveal );

        /// A transparent scroll region covering the page body, closed on scope exit.
        ///
        /// Pages used to stack their cards straight into the menu window, where `begin_child` clamps
        /// each card to whatever vertical space is left in the parent and then clips it -- so a page
        /// taller than the body silently lost its bottom rows, and once the space ran out completely
        /// the clamp was skipped and the card drew outside the window. Inside this region a column may
        /// exceed the body: it scrolls instead of being cut.
        ///
        /// The region cancels its own padding out of its geometry, so a page that fits lands on exactly
        /// the pixels it landed on before. Column cursors inside it are region-relative, which is what
        /// `left( )` and `right( )` are for -- cards must still be given an explicit width, since
        /// `avail( )` inside the region spans both columns.
        class page_scroll
        {
        public:
            page_scroll( const menu& owner, std::string_view id );
            ~page_scroll( );

            page_scroll( const page_scroll& ) = delete;
            page_scroll& operator=( const page_scroll& ) = delete;

            void left( ) const;
            void right( ) const;

            [[nodiscard]] float col_w( ) const { return this->m_col_w; }
            [[nodiscard]] float full_w( ) const { return this->m_full_w; }

        private:
            float m_col_w{};
            float m_full_w{};
            float m_pad_x{};
            float m_pad_y{};
            float m_fallback_x{};
            float m_fallback_y{};

            /// How far the region is scrolled, read once out of the container the constructor opened.
            /// `left( )` and `right( )` set an absolute cursor, so they have to subtract this by hand --
            /// without it every column starts back at the top of the region and the page never moves.
            float m_scroll{};
            bool m_open{};
        };

        /// Window size bounds. The lower pair is where the two-column body stops being usable (the cards
        /// get narrower than their own labels); the upper pair is well past any sensible desktop but keeps
        /// a stuck resize drag from running the menu off the screen entirely.
        static constexpr auto k_min_menu_w{ 600.0f };
        static constexpr auto k_min_menu_h{ 340.0f };
        static constexpr auto k_max_menu_w{ 1600.0f };
        static constexpr auto k_max_menu_h{ 1000.0f };

        bool m_open{ true };
        bool m_config_modal_open{};
        bool m_config_cloud_refresh_pending{};
        bool m_config_advanced_open{};
        bool m_last_open{ true };
        float m_open_anim{ 1.0f };
        std::uint8_t m_saved_relative_mouse{};
		bool m_has_saved_cursor{};
		int m_saved_cursor_x{};
		int m_saved_cursor_y{};

        float m_x{ 100.0f };
        float m_y{ 100.0f };
        float m_w{ 700.0f };
        float m_h{ 450.0f };
        float m_body_x{};
        float m_body_y{};
        float m_body_w{};
        float m_body_h{};

        /// Spectate window position; x below zero means not placed yet.
        float m_spec_x{ -1.0f };
        float m_spec_y{};

        /// The unload page, opened from the avatar in the sidebar. It is a page of its own rather than a
        /// tab, so it never appears in the tab strip or in search.
        bool m_unload_open{};

        int m_tab{};
        int m_subtab{};
        int m_subtab_pill_tab{ -1 };
        float m_subtab_pill_x{ -1.0f };
		float m_intro_elapsed{};
		float m_intro_assets_ready_at{ -1.0f };
		float m_intro_bar_phase{};
		bool m_intro_base_graphics_ready{};
		bool m_intro_finished{};
		bool m_search_open{};
		float m_user_avatar_retry_delay{};
		std::string m_search_query{};
		std::vector<std::size_t> m_search_visible_indices{};

		struct search_entry
		{
			std::string name{};
			std::string category{};
			std::string name_lower{};
			std::string category_lower{};
			int tab{};
			int subtab{};
			int bind_key{};
		};
		std::vector<search_entry> m_search_entries{};

        struct textures
        {
            struct entry
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource{};
                int width{};
                int height{};
            };

            /// One-colour mark for the glass skin's logo pill.
            entry logo{};

            /// The two-tone mark at the head of the legacy tab column: ring and ticks in the accent, the
            /// question mark in the text colour. Two textures because a tint is one colour per image.
            entry logo_ring{};
            entry logo_glyph{};

            /// The same mark and the "aim" / "where" wordmark at splash size, for the intro.
            entry intro_ring{};
            entry intro_glyph{};
            entry intro_aim{};
            entry intro_where{};

            entry user{};
            entry tabs[ 8 ]{};
            entry search{};
            entry settings{};
            entry cfg_folder_on{};
            entry cfg_folder_off{};
            entry cfg_cloud_on{};
            entry cfg_cloud_off{};
            entry cfg_plus{};
        } m_textures{};

        static constexpr auto k_max_subtabs{ 7 };

        struct subtab_info
        {
            const char* names[ k_max_subtabs ]{};
            int count{};
        };

        static constexpr subtab_info k_subtab_defs[ static_cast< int >( tab::count ) ]
        {
            { { "pistol", "smg", "rifle", "shotgun", "sniper", "lmg", "global" }, 7 },
            { { "pistol", "smg", "rifle", "shotgun", "sniper", "lmg" }, 6 },
            { { "enemies", "allies", "local" },                         3 },
            { { "esp", "scene", "weather" },                            3 },
            { { "guns", "knives", "gloves", "agents", "models" },       5 },
            { { "main", "removals", "camera", "hud" },                  4 },
            { { "general" },                                            1 },
            { { "colors", "visuals", "layout" },                        3 }
        };

        /// The shipped palettes, indexed by `settings::gui::preset`. `apply_theme_preset` copies a row
        /// into the settings and the theme tab paints its preset dots from the same row, so the two can
        /// never drift -- they used to carry independent copies of the same seven colours.
        struct palette
        {
            xdraw::color accent, accent_2, background, text, text_dim, card, elevated;
        };

        static constexpr palette k_presets[ ]
        {
            { { 173, 192, 255, 255 }, { 130, 158, 255, 255 }, {  17,  17,  17, 255 }, { 221, 229, 255, 235 }, { 173, 192, 255,  82 }, {  17,  17,  17,  82 }, { 31, 31, 35, 118 } },
            { { 190, 164, 255, 255 }, { 148, 132, 255, 255 }, {  18,  17,  23, 255 }, { 235, 229, 255, 235 }, { 190, 164, 255,  88 }, {  18,  17,  23,  88 }, { 34, 31, 43, 125 } },
            { { 122, 224, 181, 255 }, {  96, 202, 208, 255 }, {  14,  20,  19, 255 }, { 222, 246, 238, 235 }, { 122, 224, 181,  88 }, {  14,  20,  19,  88 }, { 25, 39, 35, 125 } },
            { { 255, 171, 112, 255 }, { 255, 122, 122, 255 }, {  22,  17,  15, 255 }, { 255, 232, 214, 235 }, { 255, 171, 112,  88 }, {  22,  17,  15,  88 }, { 43, 32, 26, 125 } },
            { { 210, 214, 224, 255 }, { 160, 166, 180, 255 }, {  18,  18,  20, 255 }, { 232, 235, 242, 235 }, { 210, 214, 224,  82 }, {  18,  18,  20,  92 }, { 35, 35, 39, 126 } }
        };

        static constexpr auto k_preset_count{ static_cast< int >( std::size( k_presets ) ) };

        static constexpr const char* k_preset_names[ k_preset_count ]
        {
            "aimwhere", "amethyst", "seafoam", "ember", "graphite"
        };
    };

	class widgets
	{
	public:
		void draw( );

		static inline std::string s_map_name{};

	private:
		void watermark( xdraw::draw_list& draw_list );
		void keybinds( xdraw::draw_list& draw_list );
	};

	class fonts
	{
	public:
		enum class size : std::uint8_t
		{
			petite,
			normal,
			big,
			count
		};

		struct family_t
		{
			std::array<xdraw::font*, static_cast< std::size_t >( size::count )> sizes{ };

			xdraw::font* operator[]( size size ) const { return this->sizes[ static_cast< std::size_t >( size ) ]; }
			xdraw::font*& operator[]( size size ) { return this->sizes[ static_cast< std::size_t >( size ) ]; }
		};

		void initialize( );

		family_t inter_medium{};
		family_t inter_bold{};
		family_t smallest_pixel7{};

		/// The legacy skin's menu font: Windows' own Verdana at 12px, Tahoma if Verdana is missing, null if
		/// neither loads (the menu then keeps the default font).
		xdraw::font* legacy_menu{};

	private:
		void load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes );
	};

	inline context g_context{};
	inline menu g_menu{};
	inline widgets g_widgets{};
	inline fonts g_fonts{};

} // namespace rendering
