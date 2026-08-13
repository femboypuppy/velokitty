#pragma once

#include <filesystem>
#include <core/systems/systems.hpp>

namespace features::changer {

	class econ_item_system
	{
	public:
		enum class item_category : std::uint8_t
		{
			gun,
			knife,
			glove,
			agent,
			other
		};

		struct paint_kit
		{
			int id{};
			std::string name{};
			std::string desc_token{};
			std::string name_token{};
			std::string localized_name{};
			float wear_min{};
			float wear_max{};
			bool legacy_model{};
			std::uint8_t rarity{};
		};

		struct item_def
		{
			std::int16_t def_index{};
			std::string item_class{};
			std::string name{};
			std::string localized_name{};
			std::string model_player{};
			std::string image_inventory{};
			int loadout_slot{};
			std::uint32_t used_by_classes{};
			item_category category{};
			std::uint8_t rarity{};

			[[nodiscard]] int team( ) const
			{
				if ( ( this->used_by_classes & 0xc ) == 0xc )
				{
					return 0;
				}

				if ( this->used_by_classes & 4 )
				{
					return 2;
				}

				if ( this->used_by_classes & 8 )
				{
					return 3;
				}

				return 0;
			}
		};

		struct skin_entry
		{
			std::int16_t def_index{};
			int paint_kit_id{};
		};

		struct skin_image
		{
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv{};
			int width{};
			int height{};
		};

		[[nodiscard]] bool initialize( );

		[[nodiscard]] const std::vector<paint_kit>& paint_kits( ) const { return this->m_paint_kits; }
		[[nodiscard]] const std::vector<item_def>& item_defs( ) const { return this->m_item_defs; }

		[[nodiscard]] const std::vector<const item_def*>& knives( ) const { return this->m_knives; }
		[[nodiscard]] const std::vector<const item_def*>& gloves( ) const { return this->m_gloves; }
		[[nodiscard]] const std::vector<const item_def*>& agents( ) const { return this->m_agents; }
		[[nodiscard]] const std::vector<const item_def*>& guns( ) const { return this->m_guns; }
		[[nodiscard]] const std::vector<skin_entry>& skins( ) const { return this->m_skins; }

		[[nodiscard]] const item_def* find_def( std::int16_t def_index ) const;
		[[nodiscard]] const paint_kit* find_paint_kit( int id ) const;
		[[nodiscard]] const skin_image* get_skin_image( const std::string& image_inventory );
		[[nodiscard]] const skin_image* get_skin_image( std::int16_t def_index, int paint_kit_id );

		[[nodiscard]] int combined_rarity( std::int16_t def_index, int paint_kit_id ) const;

		void flush_skin_images( );

	private:
		enum class image_state : std::uint8_t
		{
			idle,
			loading,
			decoded,
			ready,
			failed
		};

		struct image_entry
		{
			skin_image image{};
			std::atomic<image_state> state{ image_state::idle };
			std::vector<std::vector<std::uint8_t>> mip_buffers{};
			std::uint32_t width{};
			std::uint32_t height{};
			DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
		};

		bool parse_item_defs( std::uintptr_t schema );
		bool parse_paint_kits( std::uintptr_t schema );
		void build_indices( );
		void resolve_localized_names( );
		bool build_vpk_index( );
		void build_skin_index( );

		void request_decode( const std::string& image_inventory );
		bool finalize_texture( image_entry& entry );

		[[nodiscard]] item_category classify( const char* item_class, int loadout_slot );
		[[nodiscard]] std::vector<std::byte> read_vpk( const std::string& path );
		[[nodiscard]] bool decode_vtex( std::span<const std::byte> data, image_entry& out );
		[[nodiscard]] std::string build_skin_image_path( const item_def* def, const paint_kit* pk ) const;

		std::vector<paint_kit> m_paint_kits{};
		std::vector<item_def> m_item_defs{};

		std::vector<const item_def*> m_knives{};
		std::vector<const item_def*> m_gloves{};
		std::vector<const item_def*> m_agents{};
		std::vector<const item_def*> m_guns{};
		std::vector<skin_entry> m_skins{};

		std::unordered_map<std::int16_t, std::size_t> m_def_index_map{};
		std::unordered_map<int, std::size_t> m_paint_kit_map{};

		struct vpk_file_entry
		{
			std::uint16_t archive_index{};
			std::uint32_t offset{};
			std::uint32_t length{};
		};

		std::unordered_map<std::string, vpk_file_entry> m_vpk_index{};
		bool m_vpk_indexed{};
		std::filesystem::path m_vpk_directory{};

		std::unordered_map<std::string, std::unique_ptr<image_entry>> m_image_cache{};
		std::mutex m_image_mutex{};

		std::unordered_map<std::uint16_t, std::ifstream> m_archive_handles{};
		std::mutex m_vpk_mutex{};
	};

	class agents
	{
	public:
		void on_frame_stage_notify( );

	private:
		void cycle_weapon_owners( std::uintptr_t pawn );

		std::string m_original_model{};
		std::uintptr_t m_tracked_pawn{};
		std::uintptr_t m_applied_handle{};
		std::int16_t m_applied_def{};
		bool m_overridden{};
		int m_tracked_team{};
	};

	/// A local-only player model loaded from %APPDATA%\velocity\models instead of from the game's VPKs.
	///
	/// Two halves that have to agree with each other:
	///
	///   - the picker half is main-thread: `refresh` walks the folder, fills `m_entries` for the menu
	///     and builds the lookup table; `on_frame_stage_notify` applies the selection to the pawn.
	///   - the VFS half runs on the engine's own filesystem threads. When the resource system asks for
	///     a path that exists in no VPK, `hooks::utility::service_read` calls `resolve` and streams the
	///     file off disk. That is the whole trick: the engine never learns the model came from outside
	///     the game, so nothing about it is networked and nobody else sees it.
	///
	/// `resolve` is the only member the filesystem thread touches and the only one that takes the lock.
	/// It early-outs on an atomic when the folder is empty, which is the common case, so a user with no
	/// custom models pays one relaxed load per file the game loads and nothing more.
	class custom_models
	{
	public:
		struct entry
		{
			/// Path relative to the models folder, forward-slashed, original case. This is what the
			/// config stores -- an absolute path would carry one machine's drive letter into a shared
			/// config and resolve to nothing on the next. For a model inside a .vpk this is the archive's
			/// own relative path, then "::", then the path inside it.
			std::string relative{};

			/// What the menu row shows: the stem, plus the parent folder when there is one, because
			/// half the addons on the internet are called "model.vmdl_c".
			std::string display{};

			/// The *uncompiled* game path handed to set_player_model, e.g. "models/velocity/miku.vmdl".
			/// No trailing _c: the resource system appends it when it opens the file.
			std::string game_path{};

			std::uintmax_t size{};

			/// True when the bytes live inside a .vpk rather than as a file on disk. Only the menu cares;
			/// resolution treats both the same.
			bool packed{};

			/// How many of this model's own materials and textures are not in the folder, counted at scan
			/// time out of its external reference list. Non-zero means the model must not be applied: CS2
			/// answers a material it cannot load with "attempting to render with error material" as a
			/// FATAL ERROR and closes the game.
			int missing_files{};

			/// The first one, for the menu to name. A count on its own does not tell the user which
			/// content pack they are missing.
			std::string missing_sample{};
		};

		enum class status : std::uint8_t
		{
			idle,
			no_directory,
			no_models,
			missing_selection,
			missing_content,
			bound,
			bind_failed
		};

		void on_frame_stage_notify( );

		/// Re-walks the folder. Cheap enough to call from a button, far too expensive for a frame.
		void refresh( );

		/// Absolute disk path for a game-relative request, or empty when the path is not ours. Called
		/// from the engine's filesystem thread.
		[[nodiscard]] std::wstring resolve( std::string_view request ) const;

		/// Same question for a request that lives inside a mounted .vpk: fills `out` with the whole file
		/// and returns true, or leaves it alone and returns false. Also called from the filesystem thread,
		/// so it copies the index entry under the lock and does the read outside it.
		[[nodiscard]] bool read_packed( std::string_view request, std::vector<std::uint8_t>& out ) const;

		/// Hot-path guard for the read hook: false means the table is empty, so skip the lookup.
		[[nodiscard]] bool has_files( ) const noexcept { return this->m_has_files.load( std::memory_order_relaxed ); }

		/// True while a custom model is selected and meant to be on the pawn. `agents` backs off on
		/// this so the two features do not overwrite each other's model every frame.
		[[nodiscard]] bool active( ) const noexcept { return this->m_active; }

		/// The list as it was last published. A snapshot rather than a reference into the live vector:
		/// the menu iterates this on the render thread while the game thread can be inside `refresh`,
		/// and a vector that reallocates under a running loop is a crash with no useful log line.
		/// Callers keep the returned pointer alive for as long as they read from it, which is what makes
		/// the swap in `refresh` safe.
		[[nodiscard]] std::shared_ptr<const std::vector<entry>> entries( ) const;

		[[nodiscard]] status state( ) const noexcept { return this->m_status.load( std::memory_order_relaxed ); }
		[[nodiscard]] const char* status_text( ) const;
		[[nodiscard]] int uncompiled_count( ) const noexcept { return this->m_uncompiled.load( std::memory_order_relaxed ); }
		[[nodiscard]] bool scanned( ) const noexcept { return this->m_scanned.load( std::memory_order_relaxed ); }

		[[nodiscard]] static std::wstring directory( );
		[[nodiscard]] static std::string directory_utf8( );

	private:
		/// One file inside a mounted .vpk. `preload` is the head of the file the tree carries inline --
		/// small files live entirely there and have a zero `length`, which is why the two are concatenated
		/// rather than treated as alternatives.
		struct packed_file
		{
			std::wstring archive{};
			std::uint64_t offset{};
			std::uint32_t length{};
			std::vector<std::uint8_t> preload{};
		};

		/// A copy of the selected row, not a pointer into the list. The apply path holds it across
		/// `bind_resource` and two engine calls, and a refresh on the render thread in that window used to
		/// leave it pointing at freed strings.
		[[nodiscard]] std::optional<entry> find_selected( ) const;

		/// Where one scanned model's bytes come from. Built during a refresh and thrown away with it, so
		/// the dependency scan can read a model back without caring whether it was loose or packed.
		/// A zero `length` with an empty `preload` means "the whole file at `path`".
		struct scan_source
		{
			std::wstring path{};
			std::uint64_t offset{};
			std::uint32_t length{};
			std::vector<std::uint8_t> preload{};
		};

		/// Counts the model's own materials and textures that nothing in the folder can serve, and writes
		/// the count onto the entry. Reads the tables being built rather than the published ones, because
		/// this runs inside `refresh` before they are swapped in.
		void scan_dependencies( entry& target, const scan_source& source,
			const std::unordered_map<std::string, std::wstring>& loose,
			const std::unordered_map<std::string, packed_file>& packed ) const;

		/// Reads one .vpk's directory tree and registers every file in it. Returns false when the file is
		/// not a VPK at all, which is the normal answer for the `_000.vpk` siblings sitting next to a
		/// `_dir.vpk` -- those are raw payload with no header, so the signature check is what skips them.
		bool mount_vpk( const std::filesystem::path& archive, const std::string& archive_relative,
			std::unordered_map<std::string, packed_file>& packed, std::vector<scan_source>& sources,
			std::vector<entry>& entries, int& uncompiled );

		/// Pushes the model through the resource system and reads the binding back, so the menu can say
		/// "loaded" or "failed to load" instead of leaving the user staring at an unchanged player.
		[[nodiscard]] bool bind_resource( const std::string& game_path ) const;

		void cycle_weapon_owners( std::uintptr_t pawn ) const;
		void reset_tracking( );

		/// The published list. Replaced wholesale under `m_list_mutex`, never edited in place, so a reader
		/// holding an older snapshot keeps reading valid memory until it drops its reference.
		std::shared_ptr<const std::vector<entry>> m_entries{};
		mutable std::mutex m_list_mutex{};

		/// Held for the whole of `refresh`. Both threads can ask for a scan -- the game thread on its first
		/// live frame, the render thread when the page opens or the button is pressed -- and two scans
		/// interleaving would publish half of each.
		std::mutex m_scan_mutex{};

		/// Game-relative path *with* the _c the engine actually asks for -> absolute disk path. Every
		/// file under the folder goes in here, not just the .vmdl_c: a model drags its .vmesh_c, its
		/// .vmat_c and its textures along behind it.
		std::unordered_map<std::string, std::wstring> m_vfs{};

		/// The same table for files that live inside a .vpk. Kept separate because serving one means a
		/// seek and a sized read rather than opening a path, and the read hook needs to know which.
		/// Guarded by `m_vfs_mutex` as well -- the two are swapped together.
		std::unordered_map<std::string, packed_file> m_packed{};

		mutable std::mutex m_vfs_mutex{};
		std::atomic<bool> m_has_files{ false };

		std::string m_original_model{};
		std::string m_applied_relative{};
		std::string m_bound_path{};

		/// The selection we last refused to apply for missing content, so the reason is logged once
		/// instead of once per frame.
		std::string m_blocked_relative{};

		std::uintptr_t m_tracked_pawn{};
		std::uintptr_t m_applied_handle{};
		bool m_overridden{};
		bool m_active{};
		bool m_bound{};

		/// Written by whichever thread scanned or applied last and read by the menu on another, so all
		/// four are atomic. None of them is worth a lock: a frame that reads the previous value shows a
		/// stale status line for 16 milliseconds.
		std::atomic<bool> m_scanned{ false };
		std::atomic<int> m_uncompiled{};
		std::atomic<status> m_status{ status::idle };

		/// Bumped by every published scan. The apply path compares it against what it last saw and drops
		/// its bind bookkeeping when it moves, because a rescan can move or delete the file the selection
		/// points at -- and `refresh` cannot write that bookkeeping itself from another thread.
		std::atomic<std::uint32_t> m_revision{};
		std::uint32_t m_seen_revision{};

		/// The same idea for map changes, read off `systems::g_entities.generation( )`. A precache is only
		/// good for the world that was loaded when it ran, so the bind has to be redone on the new map --
		/// and `set_player_model` with a path whose content is no longer precached is the "render with
		/// error material" fatal box, not a missing texture.
		std::uint32_t m_seen_generation{};
	};

	class gloves
	{
	public:
		void on_frame_stage_notify( );

	private:
		struct original_state
		{
			std::uint16_t def_index{};
			std::uint64_t item_id{};
			std::uint32_t id_high{};
			std::uint32_t id_low{};
			std::uint32_t account_id{};
			bool restore_custom_material{};
			bool initialized{};
			bool disallow_soc{};
			bool captured{};
		};

		struct attribute_state
		{
			float value{};
			bool present{};
		};

		[[nodiscard]] bool capture_original( std::uintptr_t item_view );
		[[nodiscard]] bool read_paint_attributes( std::uintptr_t item_view, std::array<attribute_state, 3>& attributes ) const;
		[[nodiscard]] bool restore_paint_attributes( std::uintptr_t item_view ) const;
		[[nodiscard]] bool paint_attributes_match( std::uintptr_t item_view, const settings::changer::applied_skin& skin ) const;
		void apply( std::uintptr_t pawn, std::uintptr_t item_view, int team, const econ_item_system::item_def& def, const settings::changer::applied_skin& skin, std::uint32_t account_id );
		void restore( std::uintptr_t pawn, std::uintptr_t item_view, int team );
		void refresh( std::uintptr_t pawn, std::uintptr_t item_view, int team ) const;
		void reset( );

		original_state m_original{};
		std::array<attribute_state, 3> m_original_attributes{};
		std::uintptr_t m_tracked_pawn{};
		bool m_overridden{};
	};

	class guns
	{
	public:
		void on_frame_stage_notify( );

	private:
		void apply( std::uintptr_t weapon, std::uintptr_t iv, std::uint32_t handle, std::uint32_t active_handle, std::uintptr_t pawn, const settings::changer::applied_skin* skin, std::uint32_t account_id );
		void rebuild_paint( std::uintptr_t weapon, std::uint32_t handle, std::uint32_t active_handle, std::uintptr_t pawn, const econ_item_system::paint_kit* pk );
		void update_view_model( std::uintptr_t pawn, const econ_item_system::paint_kit* pk );
		[[nodiscard]] std::uintptr_t find_hud_model_weapon( std::uintptr_t pawn );
		void clear_hud_icon( std::uintptr_t iv );

		std::uint32_t m_last_active_handle{};
		std::uintptr_t m_tracked_pawn{};

		/// Keyed on the whole skin, not just the paint kit: editing wear, seed or StatTrak leaves
		/// the paint kit equal, and the old int-keyed cache read that as "already applied" and
		/// skipped the write.
		std::unordered_map<std::uint32_t, settings::changer::applied_skin> m_applied_weapons{};

		std::uint32_t m_last_skin_revision{};
		int m_throttle{};
	};

	class knives
	{
	public:
		void on_frame_stage_notify( );

	private:
		struct original_state
		{
			std::uint16_t def_index{};
			std::uint32_t id_high{};
			std::uint32_t id_low{};
			std::uint32_t account_id{};
			bool initialized{};
			int paint_kit{};
			int seed{};
			float wear{};
			int stattrak{};
			bool captured{};
		};

		void capture_original( std::uintptr_t weapon, std::uintptr_t iv );
		void apply( std::uintptr_t weapon, std::uintptr_t iv, const econ_item_system::item_def* def, const settings::changer::applied_skin* skin, std::uint32_t account_id, std::uintptr_t active_weapon, std::uintptr_t pawn );
		void restore( std::uintptr_t weapon, std::uintptr_t iv, std::uintptr_t active_weapon, std::uintptr_t pawn );
		void update_model( std::uintptr_t weapon, std::uintptr_t iv, std::uint16_t def_index );
		void update_view_model( std::uintptr_t pawn, const econ_item_system::paint_kit* pk );
		[[nodiscard]] std::uintptr_t find_hud_model_weapon( std::uintptr_t pawn );
		void rebuild_paint( std::uintptr_t weapon, std::uintptr_t active_weapon, std::uintptr_t pawn, const econ_item_system::paint_kit* pk );
		void clear_hud_icon( std::uintptr_t iv );

		original_state m_original{};
		std::uint32_t m_last_active_handle{};
		std::uintptr_t m_tracked_pawn{};
		bool m_overridden{};

		/// The live-state check in on_frame_stage_notify only compares the subclass token and the
		/// paint kit, so an edit to wear, seed or StatTrak alone looks "already applied". Watching
		/// the revision is what forces those through.
		std::uint32_t m_last_skin_revision{};
		int m_throttle{};
	};

} // namespace features::changer
