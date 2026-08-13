#include <pch/pch.hpp>
#include <ShlObj.h>
#include <filesystem>

#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>

namespace features::changer {

	namespace custom_model_detail {

		/// Only a compiled model can be selected. The uncompiled ones are counted and reported instead,
		/// because "I put my .vmdl in the folder and nothing happened" is otherwise a silent dead end.
		constexpr std::string_view k_model_extension{ ".vmdl_c" };
		constexpr std::string_view k_source_extension{ ".vmdl" };

		/// Source 2 resolves content under these. An extracted addon keeps the mesh and material paths
		/// its .vmdl_c was compiled against, and those are absolute game paths -- so a file sitting at
		/// <models>\miku\models\characters\miku.vmesh_c has to be answerable as
		/// "models/characters/miku.vmesh_c", or the model loads as an untextured blob with no geometry.
		constexpr std::string_view k_content_roots[ ]
		{
			"models/",
			"materials/",
			"particles/",
			"sounds/",
			"soundevents/",
			"characters/",
			"animgraphs/",
			"panorama/",
			"scripts/",
			"maps/"
		};

		/// Where a loose file with no tree around it gets exposed. Something dropped straight into the
		/// folder still needs an address the resource system can be pointed at.
		constexpr std::string_view k_synthetic_root{ "models/velocity/" };

		/// A workshop addon downloads as one .vpk and nobody wants to be told to go and extract it, so
		/// the archive is mounted in place and its contents answer alongside the loose files.
		constexpr std::string_view k_archive_extension{ ".vpk" };
		constexpr std::uint32_t k_vpk_signature{ 0x55aa1234u };

		/// An entry pointing at this archive index has its bytes in the directory file itself rather than
		/// in a numbered sibling -- which is how every single-file .vpk stores everything.
		constexpr std::uint16_t k_vpk_inline_archive{ 0x7fffu };

		/// A sanity bound on the directory tree, not on the archive. A tree this size would be several
		/// hundred thousand files; anything larger is a corrupt or hostile header rather than a model pack.
		constexpr std::uint32_t k_max_vpk_tree{ 64u * 1024u * 1024u };

		[[nodiscard]] bool has_extension( std::string_view name, std::string_view ext )
		{
			if ( name.size( ) <= ext.size( ) )
			{
				return false;
			}

			return _strnicmp( name.data( ) + name.size( ) - ext.size( ), ext.data( ), ext.size( ) ) == 0;
		}

		/// Engine paths arrive in every shape there is: back-slashed, doubled separators, mixed case,
		/// sometimes carrying the mod folder in front. Both sides of the lookup go through this, so the
		/// table only ever needs exact matches and a miss costs one hash.
		[[nodiscard]] std::string normalize( std::string_view in )
		{
			std::string out{};
			out.reserve( in.size( ) );

			for ( const auto c : in )
			{
				const auto ch = ( c == '\\' ) ? '/' : static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );

				if ( ch == '/' && !out.empty( ) && out.back( ) == '/' )
				{
					continue;
				}

				out.push_back( ch );
			}

			std::size_t leading{};
			while ( leading < out.size( ) && out[ leading ] == '/' )
			{
				++leading;
			}

			if ( leading )
			{
				out.erase( 0, leading );
			}

			for ( const std::string_view prefix : { "game/csgo/", "csgo/" } )
			{
				if ( out.starts_with( prefix ) )
				{
					out.erase( 0, prefix.size( ) );
					break;
				}
			}

			return out;
		}

		/// The game-relative path a scanned file answers to. Anchored on a real content root when the
		/// file sits under one, so the addon's own internal references resolve; otherwise pushed under
		/// the synthetic root, which is all a single self-contained .vmdl_c needs.
		[[nodiscard]] std::string game_key( const std::string& normalized_relative )
		{
			// The earliest boundary wins, not the first root in the table above. The common workshop
			// player pack is laid out "characters/models/<author>/<name>/<name>.vmdl_c", and a
			// first-match-in-list-order search finds "models/" at the *inner* boundary and shears the
			// "characters/" off the front. The model would still answer at that truncated address, but
			// every mesh and material it names is baked as "characters/models/...", so it would load as
			// an untextured blob and look like the feature was broken.
			auto cut = std::string::npos;

			for ( const auto root : k_content_roots )
			{
				if ( normalized_relative.starts_with( root ) )
				{
					return normalized_relative;
				}

				// Only at a path boundary: "mymodels/x.vmdl_c" is not "models/x.vmdl_c".
				const auto boundary = std::string{ '/' } + std::string{ root };
				const auto at = normalized_relative.find( boundary );

				if ( at != std::string::npos && ( cut == std::string::npos || at < cut ) )
				{
					cut = at;
				}
			}

			if ( cut != std::string::npos )
			{
				return normalized_relative.substr( cut + 1 );
			}

			return std::string{ k_synthetic_root } + normalized_relative;
		}

		/// set_player_model takes the source path; the resource system appends the _c when it opens the
		/// file. Handing it the compiled name gives "models/x.vmdl_c_c" and a model that never loads.
		[[nodiscard]] std::string strip_compiled_suffix( std::string_view path )
		{
			if ( path.ends_with( "_c" ) )
			{
				path.remove_suffix( 2 );
			}

			return std::string{ path };
		}

		[[nodiscard]] std::string to_utf8( const std::wstring& in )
		{
			if ( in.empty( ) )
			{
				return {};
			}

			const auto needed = WideCharToMultiByte( CP_UTF8, 0, in.c_str( ), static_cast< int >( in.size( ) ), nullptr, 0, nullptr, nullptr );
			if ( needed <= 0 )
			{
				return {};
			}

			std::string out( static_cast< std::size_t >( needed ), '\0' );
			WideCharToMultiByte( CP_UTF8, 0, in.c_str( ), static_cast< int >( in.size( ) ), out.data( ), needed, nullptr, nullptr );

			return out;
		}

		/// The row label. Keeps one level of parent folder, because a great many addons ship a file
		/// called "model.vmdl_c" and a list of six rows all reading "model" is not a picker.
		[[nodiscard]] std::string display_name( const std::string& relative )
		{
			auto trimmed = relative;

			if ( has_extension( trimmed, k_model_extension ) )
			{
				trimmed.resize( trimmed.size( ) - k_model_extension.size( ) );
			}

			const auto last = trimmed.find_last_of( '/' );
			if ( last == std::string::npos || last == 0 )
			{
				return trimmed;
			}

			const auto parent = trimmed.find_last_of( '/', last - 1 );

			return parent == std::string::npos ? trimmed : trimmed.substr( parent + 1 );
		}

		template <typename type>
		[[nodiscard]] bool read_pod( std::ifstream& stream, type& out )
		{
			stream.read( reinterpret_cast< char* >( &out ), sizeof( type ) );

			return stream.gcount( ) == static_cast< std::streamsize >( sizeof( type ) );
		}

		/// The bytes for `<name>_dir.vpk` entry with archive index 3 live in `<name>_003.vpk` beside it.
		/// A single-file archive never reaches this -- all of its entries carry the inline index instead.
		[[nodiscard]] std::wstring sibling_archive( const std::filesystem::path& dir_archive, std::uint16_t index )
		{
			auto stem = dir_archive.stem( ).wstring( );

			if ( stem.size( ) > 4 && _wcsicmp( stem.c_str( ) + stem.size( ) - 4, L"_dir" ) == 0 )
			{
				stem.resize( stem.size( ) - 4 );
			}

			return ( dir_archive.parent_path( ) / std::format( L"{}_{:03}.vpk", stem, index ) ).wstring( );
		}

		/// The tree stores a file as three separate nul-terminated strings with the extension hoisted off
		/// the front, and uses a single space where a component is empty -- a file at the archive root has
		/// the folder " ", and an extensionless file has the extension " ".
		[[nodiscard]] std::string join_vpk_path( const std::string& folder, const std::string& name, const std::string& extension )
		{
			std::string out{};
			out.reserve( folder.size( ) + name.size( ) + extension.size( ) + 2 );

			if ( !folder.empty( ) && folder != " " )
			{
				out += folder;
				out.push_back( '/' );
			}

			out += name;

			if ( !extension.empty( ) && extension != " " )
			{
				out.push_back( '.' );
				out += extension;
			}

			return out;
		}

		/// A bounds-checked walk over the directory tree. Every read reports whether it stayed inside the
		/// buffer, so a truncated or lying header stops the parse instead of walking off the end of it --
		/// this is parsing a file the user downloaded from the internet on the strength of its own header.
		class tree_cursor
		{
		public:
			tree_cursor( const std::uint8_t* data, std::size_t size ) : m_data{ data }, m_size{ size } { }

			[[nodiscard]] bool read_string( std::string& out )
			{
				const auto start = this->m_at;

				while ( this->m_at < this->m_size && this->m_data[ this->m_at ] != '\0' )
				{
					++this->m_at;
				}

				if ( this->m_at >= this->m_size )
				{
					return false;
				}

				out.assign( reinterpret_cast< const char* >( this->m_data + start ), this->m_at - start );
				++this->m_at;

				return true;
			}

			template <typename type>
			[[nodiscard]] bool read( type& out )
			{
				if ( this->m_size - this->m_at < sizeof( type ) )
				{
					return false;
				}

				std::memcpy( &out, this->m_data + this->m_at, sizeof( type ) );
				this->m_at += sizeof( type );

				return true;
			}

			[[nodiscard]] bool read_bytes( std::size_t count, std::vector<std::uint8_t>& out )
			{
				if ( this->m_size - this->m_at < count )
				{
					return false;
				}

				out.assign( this->m_data + this->m_at, this->m_data + this->m_at + count );
				this->m_at += count;

				return true;
			}

		private:
			const std::uint8_t* m_data{};
			std::size_t m_size{};
			std::size_t m_at{};
		};

		/// Only the head of a model is read when scanning it. A compiled resource carries its block table
		/// and its RERL block at the very front -- 256 KB covers both many times over for any player model
		/// -- and pulling a dozen whole 2 MB models off disk to look at their first few kilobytes would
		/// stall the frame the scan runs on.
		constexpr std::uint32_t k_scan_window{ 256u * 1024u };

		/// A bound on RERL itself. A player model names dozens of files, never thousands.
		constexpr std::uint32_t k_max_references{ 4096u };

		/// The front of one scanned file, wherever it lives. A zero `length` means "the file at `path`",
		/// which is how a loose file is described; otherwise it is the inline head the archive's tree
		/// carried followed by the bytes at `offset`. A short read is not an error here -- every reader
		/// downstream is bounds-checked, so a truncated buffer simply yields fewer references.
		[[nodiscard]] std::vector<std::uint8_t> read_scan_head( const std::wstring& path, std::uint64_t offset,
			std::uint32_t length, const std::vector<std::uint8_t>& preload )
		{
			std::vector<std::uint8_t> out{ preload };

			// Stored entirely inline in the tree, or already past the window: nothing to open.
			if ( out.size( ) >= k_scan_window || ( length == 0u && !out.empty( ) ) )
			{
				return out;
			}

			std::ifstream stream{ path, std::ios::binary };
			if ( !stream.is_open( ) )
			{
				return {};
			}

			auto want = static_cast< std::uint64_t >( length );

			if ( length == 0u )
			{
				stream.seekg( 0, std::ios::end );

				const auto size = static_cast< std::streamoff >( stream.tellg( ) );
				if ( size <= 0 )
				{
					return {};
				}

				want = static_cast< std::uint64_t >( size );
			}

			want = std::min< std::uint64_t >( want, k_scan_window - out.size( ) );

			stream.seekg( static_cast< std::streamoff >( offset ), std::ios::beg );
			if ( !stream )
			{
				return {};
			}

			const auto head = out.size( );
			out.resize( head + static_cast< std::size_t >( want ) );
			stream.read( reinterpret_cast< char* >( out.data( ) + head ), static_cast< std::streamsize >( want ) );
			out.resize( head + static_cast< std::size_t >( stream.gcount( ) ) );

			return out;
		}

		/// Every external file a compiled resource names, out of its RERL block -- the list Source 2 keeps
		/// precisely so the engine knows what else it has to load. The names are the *source* names
		/// (`.vmat`, `.vtex`), so looking one up against the compiled files in the folder means appending
		/// `_c` to it.
		///
		/// The layout, verified against a real .vmdl_c rather than taken from a wiki: the header is
		/// { u32 file_size, u16 header_version, u16 resource_version, u32 block_offset, u32 block_count },
		/// the block table sits at 8 + block_offset holding 12-byte entries { char type[ 4 ], u32 offset,
		/// u32 size } whose body is at the offset field's own position + 4 + offset, and RERL's body is
		/// { i32 entries_offset, u32 entries_count } followed by 16-byte records { u64 id, i32 name_offset }
		/// with the string at the name field's own position + name_offset. Every offset in the format is
		/// relative to where it was read from, which is why none of them are added to a base.
		[[nodiscard]] std::vector<std::string> collect_references( const std::vector<std::uint8_t>& data )
		{
			std::vector<std::string> out{};

			if ( data.size( ) < 16u )
			{
				return out;
			}

			std::uint32_t block_offset{};
			std::uint32_t block_count{};
			std::memcpy( &block_offset, data.data( ) + 8, sizeof( block_offset ) );
			std::memcpy( &block_count, data.data( ) + 12, sizeof( block_count ) );

			if ( block_count == 0u || block_count > 64u )
			{
				return out;
			}

			const auto table = 8ull + block_offset;
			if ( table + 12ull * block_count > data.size( ) )
			{
				return out;
			}

			for ( std::uint32_t index = 0; index < block_count; ++index )
			{
				const auto at = table + 12ull * index;

				if ( std::memcmp( data.data( ) + at, "RERL", 4 ) != 0 )
				{
					continue;
				}

				std::uint32_t block_relative{};
				std::memcpy( &block_relative, data.data( ) + at + 4, sizeof( block_relative ) );

				const auto body = at + 4ull + block_relative;
				if ( body + 8ull > data.size( ) )
				{
					continue;
				}

				std::int32_t entries_offset{};
				std::uint32_t entries_count{};
				std::memcpy( &entries_offset, data.data( ) + body, sizeof( entries_offset ) );
				std::memcpy( &entries_count, data.data( ) + body + 4, sizeof( entries_count ) );

				if ( entries_offset < 0 || entries_count == 0u || entries_count > k_max_references )
				{
					continue;
				}

				const auto entries = body + static_cast< std::uint64_t >( entries_offset );
				if ( entries + 16ull * entries_count > data.size( ) )
				{
					continue;
				}

				for ( std::uint32_t record = 0; record < entries_count; ++record )
				{
					const auto name_field = entries + 16ull * record + 8ull;

					std::int32_t name_offset{};
					std::memcpy( &name_offset, data.data( ) + name_field, sizeof( name_offset ) );

					if ( name_offset < 0 )
					{
						continue;
					}

					const auto at_name = name_field + static_cast< std::uint64_t >( name_offset );
					if ( at_name >= data.size( ) )
					{
						continue;
					}

					const auto* chars = reinterpret_cast< const char* >( data.data( ) + at_name );
					const auto room = static_cast< std::size_t >( data.size( ) - at_name );

					std::size_t length = 0;
					while ( length < room && chars[ length ] != '\0' )
					{
						++length;
					}

					// Ran to the end of the buffer with no terminator: the read window cut the string in
					// half and there is no telling what the rest of it said.
					if ( length == 0 || length == room )
					{
						continue;
					}

					out.emplace_back( chars, length );
				}
			}

			return out;
		}

		/// Namespaces the game itself owns. An addon that ships into one of these is overriding base-game
		/// content rather than bringing its own, so every reference it makes looks like it belongs to the
		/// addon and the check below would condemn all of them.
		constexpr std::string_view k_vanilla_namespaces[ ]
		{
			"characters/models/shared/",
			"models/weapons/",
			"models/player/",
			"models/inventory_items/"
		};

		/// The addon's own corner of the content tree: the content root its path starts with plus the one
		/// author segment under it, e.g. "models/b_cansin/" or "characters/models/hlym/". A workshop item
		/// namespaces everything it ships under one of those, and its materials repeat it verbatim
		/// underneath `materials/` -- so a reference that lands inside it is content the addon was supposed
		/// to bring along, while one that lands outside it is vanilla CS2 the game already has.
		///
		/// The root has to be matched first and it has to be the longest one that matches. Taking a flat
		/// two segments read "characters/models/hlym/bcsz/bcsz_arms_prefab.vmdl" as the namespace
		/// "characters/models/", which swallowed the vanilla arms and gloves the model legitimately points
		/// at and refused a model that had everything it needed.
		///
		/// Empty when no namespace can be named -- a path too shallow, or one rooted in a namespace the
		/// game owns. That turns the check off for that model rather than guessing.
		[[nodiscard]] std::string content_namespace( const std::string& game_path )
		{
			std::string_view root{};

			for ( const auto candidate : k_content_roots )
			{
				if ( game_path.starts_with( candidate ) && candidate.size( ) > root.size( ) )
				{
					root = candidate;
				}
			}

			// "characters/" is a root in its own right and "characters/models/" is where player content
			// actually lives; the author segment is the one under the deeper of the two.
			constexpr std::string_view characters_models{ "characters/models/" };

			if ( game_path.starts_with( characters_models ) )
			{
				root = characters_models;
			}

			if ( root.empty( ) )
			{
				return {};
			}

			const auto author_end = game_path.find( '/', root.size( ) );
			if ( author_end == std::string::npos )
			{
				return {};
			}

			// A further separator is what proves the author segment is a folder holding a tree rather than
			// the filename itself.
			if ( game_path.find( '/', author_end + 1 ) == std::string::npos )
			{
				return {};
			}

			auto space = game_path.substr( 0, author_end + 1 );

			for ( const auto vanilla : k_vanilla_namespaces )
			{
				if ( space == vanilla )
				{
					return {};
				}
			}

			return space;
		}

		/// First line of the file, and the version check. Anyone who used the first cut of this feature
		/// already has a readme.txt saying models must be extracted by hand, which is now wrong.
		constexpr std::string_view k_readme_marker{ "custom player models (v2)" };

		/// Written next to the models, and rewritten whenever the text above changes. Nobody goes looking
		/// for documentation, and the layout rule below is the single thing that decides whether an addon
		/// appears correctly or as a blob.
		void write_readme( const std::wstring& directory )
		{
			const auto path = directory + L"\\readme.txt";

			{
				std::ifstream existing{ path.c_str( ), std::ios::binary };

				if ( existing )
				{
					std::string head( k_readme_marker.size( ), '\0' );
					existing.read( head.data( ), static_cast< std::streamsize >( head.size( ) ) );

					if ( static_cast< std::size_t >( existing.gcount( ) ) == head.size( ) && head == k_readme_marker )
					{
						return;
					}
				}
			}

			std::ofstream file{ path.c_str( ), std::ios::binary | std::ios::trunc };
			if ( !file )
			{
				return;
			}

			file <<
				"custom player models (v2)\r\n"
				"=========================\r\n"
				"\r\n"
				"Three things work in here. Pick whichever you have.\r\n"
				"\r\n"
				"1. A workshop or GameBanana .vpk, dropped in as-is.\r\n"
				"\r\n"
				"       models\\kaiju_no8.vpk\r\n"
				"\r\n"
				"   The archive is read in place -- no extracting, no Source 2 Viewer. If it came as\r\n"
				"   a set of files ending _dir.vpk, _000.vpk, _001.vpk, keep them all together in\r\n"
				"   the same folder; the _dir one is the index and the rest are its payload.\r\n"
				"\r\n"
				"2. An extracted addon, keeping its own folder structure.\r\n"
				"\r\n"
				"       models\\miku\\models\\characters\\miku.vmdl_c\r\n"
				"       models\\miku\\materials\\characters\\miku_body.vmat_c\r\n"
				"\r\n"
				"   That layout matters. A .vmdl_c refers to its meshes and materials by absolute\r\n"
				"   game path, baked in when it was compiled, so those files have to be reachable\r\n"
				"   under the same models\\ and materials\\ prefixes they were compiled with. Keep the\r\n"
				"   tree and they are. Flatten it and you get an untextured or invisible model.\r\n"
				"\r\n"
				"3. A single self-contained .vmdl_c dropped straight in here. It is served as\r\n"
				"   models/velocity/<filename>.\r\n"
				"\r\n"
				"Only COMPILED models can be selected -- that is a .vmdl_c. A plain .vmdl is source\r\n"
				"text and the game cannot read it.\r\n"
				"\r\n"
				"Everything here is local only. The model is served to your own game client from\r\n"
				"this folder; the server is never told about it and nobody else sees it.\r\n"
				"\r\n"
				"Pick one under skins -> models, then hit refresh after adding files.\r\n";
		}

	} // namespace custom_model_detail

	std::wstring custom_models::directory( )
	{
		wchar_t app_data[ MAX_PATH ]{};
		if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data ) ) )
		{
			return {};
		}

		const auto root = std::wstring{ app_data } + L"\\velocity";
		const auto models = root + L"\\models";

		CreateDirectoryW( root.c_str( ), nullptr );
		CreateDirectoryW( models.c_str( ), nullptr );

		return models;
	}

	std::string custom_models::directory_utf8( )
	{
		return custom_model_detail::to_utf8( directory( ) );
	}

	void custom_models::refresh( )
	{
		// Both threads ask for this -- the game thread on its first live frame, the render thread when the
		// page opens or the button is pressed. Serialised so two scans cannot publish half of each other.
		const std::scoped_lock scan_lock{ this->m_scan_mutex };

		this->m_scanned.store( true, std::memory_order_relaxed );

		// Built into locals and published in one swap at the end. Nothing a reader can see moves while the
		// walk is running, which is what stops the menu iterating a vector that is being rebuilt.
		std::vector<entry> entries{};
		auto uncompiled = 0;

		std::unordered_map<std::string, std::wstring> table{};
		std::unordered_map<std::string, packed_file> packed{};

		const auto root = directory( );
		if ( root.empty( ) )
		{
			{
				const std::scoped_lock lock{ this->m_vfs_mutex };
				this->m_vfs.clear( );
				this->m_packed.clear( );
			}

			this->m_has_files.store( false, std::memory_order_relaxed );

			{
				const std::scoped_lock lock{ this->m_list_mutex };
				this->m_entries = std::make_shared<const std::vector<entry>>( );
			}

			this->m_uncompiled.store( 0, std::memory_order_relaxed );
			this->m_status.store( status::no_directory, std::memory_order_relaxed );
			this->m_revision.fetch_add( 1, std::memory_order_release );
			return;
		}

		custom_model_detail::write_readme( root );

		struct scanned_file
		{
			std::string relative{};
			std::wstring absolute{};
			std::uintmax_t size{};
		};

		std::vector<scanned_file> files{};
		std::vector<scanned_file> archives{};

		// Kept in lockstep with the entry list, one source per model row, so the dependency scan below can
		// read each model back without caring whether it came off the disk or out of an archive. Thrown away
		// with the refresh -- nothing outside this function has any use for it.
		std::vector<scan_source> sources{};

		std::error_code ec{};
		const std::filesystem::path base{ root };

		// Manual increment with an error_code on every step: the throwing overloads are unusable here
		// (Ship builds with exceptions off) and a folder the user is mid-copy into will absolutely hand
		// back errors partway through a walk.
		auto it = std::filesystem::recursive_directory_iterator{ base, std::filesystem::directory_options::skip_permission_denied, ec };
		const std::filesystem::recursive_directory_iterator end{};

		for ( ; !ec && it != end; it.increment( ec ) )
		{
			std::error_code entry_ec{};
			if ( !it->is_regular_file( entry_ec ) || entry_ec )
			{
				continue;
			}

			const auto relative = std::filesystem::relative( it->path( ), base, entry_ec );
			if ( entry_ec || relative.empty( ) )
			{
				continue;
			}

			auto relative_utf8 = custom_model_detail::to_utf8( relative.wstring( ) );
			if ( relative_utf8.empty( ) )
			{
				continue;
			}

			std::ranges::replace( relative_utf8, '\\', '/' );

			const auto size = std::filesystem::file_size( it->path( ), entry_ec );

			auto& bucket = custom_model_detail::has_extension( relative_utf8, custom_model_detail::k_archive_extension ) ? archives : files;

			bucket.push_back( {
				.relative = std::move( relative_utf8 ),
				.absolute = it->path( ).wstring( ),
				.size = entry_ec ? 0ull : size
			} );
		}

		// Sorted before insertion so a duplicated game path resolves the same way on every launch. Two
		// files can legitimately claim one address -- the same model dropped twice, once loose and once
		// in a tree -- and "whichever the directory walk reached first" is not an answer.
		std::ranges::sort( files, []( const scanned_file& a, const scanned_file& b ) { return a.relative < b.relative; } );
		std::ranges::sort( archives, []( const scanned_file& a, const scanned_file& b ) { return a.relative < b.relative; } );

		for ( const auto& file : files )
		{
			const auto normalized = custom_model_detail::normalize( file.relative );
			const auto key = custom_model_detail::game_key( normalized );

			table.emplace( key, file.absolute );

			// Answerable at both addresses: the one its tree implies and the synthetic one. Costs a
			// string per file and saves the user having to know which prefix we picked.
			table.emplace( std::string{ custom_model_detail::k_synthetic_root } + normalized, file.absolute );

			if ( custom_model_detail::has_extension( normalized, custom_model_detail::k_model_extension ) )
			{
				entries.push_back( {
					.relative = file.relative,
					.display = custom_model_detail::display_name( file.relative ),
					.game_path = custom_model_detail::strip_compiled_suffix( key ),
					.size = file.size
				} );

				sources.push_back( { .path = file.absolute } );
			}
			else if ( custom_model_detail::has_extension( normalized, custom_model_detail::k_source_extension ) )
			{
				++uncompiled;
			}
		}

		// Archives after the loose files, so a file the user extracted himself shadows the same path
		// inside a .vpk he also left lying there -- an extracted file is the deliberate one.
		for ( const auto& archive : archives )
		{
			this->mount_vpk( std::filesystem::path{ archive.absolute }, archive.relative, packed, sources, entries, uncompiled );
		}

		// Scanned here, while `sources` is still index-for-index with the entry list and before the sort
		// below reorders it. A model whose own materials are not in the folder must never be applied: CS2
		// answers a player material it cannot load with "attempting to render with error material" as a
		// FATAL ERROR and closes itself, so the picker has to know which rows are unusable before the click.
		for ( std::size_t index = 0; index < entries.size( ) && index < sources.size( ); ++index )
		{
			this->scan_dependencies( entries[ index ], sources[ index ], table, packed );
		}

		// One order for the list regardless of how a model got in it. Without this the loose rows come out
		// sorted and the packed ones trail behind them in mount order, which reads like a bug.
		std::ranges::sort( entries, []( const entry& a, const entry& b ) { return a.relative < b.relative; } );

		const auto populated = !table.empty( ) || !packed.empty( );

		{
			const std::scoped_lock lock{ this->m_vfs_mutex };
			this->m_vfs = std::move( table );
			this->m_packed = std::move( packed );
		}

		this->m_has_files.store( populated, std::memory_order_relaxed );

		const auto found_models = !entries.empty( );

		{
			const std::scoped_lock lock{ this->m_list_mutex };
			this->m_entries = std::make_shared<const std::vector<entry>>( std::move( entries ) );
		}

		this->m_uncompiled.store( uncompiled, std::memory_order_relaxed );

		if ( !found_models )
		{
			this->m_status.store( status::no_models, std::memory_order_relaxed );
		}
		else if ( const auto current = this->m_status.load( std::memory_order_relaxed );
			current == status::no_directory || current == status::no_models )
		{
			this->m_status.store( status::idle, std::memory_order_relaxed );
		}

		// A rescan can move or delete the file the selection points at, so whatever we bound last time has
		// to be re-evaluated against the new list instead of trusted. The bind bookkeeping is game-thread
		// state and this function often runs on the render thread, so say only that the list moved and let
		// the apply path drop its own state on the next frame.
		this->m_revision.fetch_add( 1, std::memory_order_release );
	}

	/// Read the directory tree out of a .vpk and register everything in it. Nothing is copied out of the
	/// archive here -- each entry keeps the archive it lives in, its offset and its length, and the bytes
	/// are pulled only when the engine actually asks for that file.
	bool custom_models::mount_vpk( const std::filesystem::path& archive, const std::string& archive_relative,
		std::unordered_map<std::string, packed_file>& packed, std::vector<scan_source>& sources,
		std::vector<entry>& entries, int& uncompiled )
	{
		std::ifstream stream{ archive, std::ios::binary };
		if ( !stream.is_open( ) )
		{
			return false;
		}

		std::uint32_t signature{};
		std::uint32_t version{};
		std::uint32_t tree_size{};

		if ( !custom_model_detail::read_pod( stream, signature ) || signature != custom_model_detail::k_vpk_signature )
		{
			return false;
		}

		// v1 and v2 differ only in header length; both carry the same tree. v2 adds the checksum section
		// sizes, which we have no use for -- the game validates nothing about a file we hand it directly.
		if ( !custom_model_detail::read_pod( stream, version ) || ( version != 1u && version != 2u ) )
		{
			return false;
		}

		if ( !custom_model_detail::read_pod( stream, tree_size ) || tree_size == 0u || tree_size > custom_model_detail::k_max_vpk_tree )
		{
			return false;
		}

		auto header_size = 12u;

		if ( version == 2u )
		{
			std::uint32_t unused[ 4 ]{};

			if ( !custom_model_detail::read_pod( stream, unused ) )
			{
				return false;
			}

			header_size = 28u;
		}

		std::vector<std::uint8_t> tree( tree_size );
		stream.read( reinterpret_cast< char* >( tree.data( ) ), static_cast< std::streamsize >( tree_size ) );

		if ( static_cast< std::uint32_t >( stream.gcount( ) ) != tree_size )
		{
			return false;
		}

		// Where entry offsets are measured from when the bytes are in this file rather than a sibling.
		const auto data_base = static_cast< std::uint64_t >( header_size ) + tree_size;
		const auto self = archive.wstring( );

		custom_model_detail::tree_cursor cursor{ tree.data( ), tree.size( ) };

		std::string extension{};
		std::string folder{};
		std::string name{};
		auto mounted = 0;

		while ( cursor.read_string( extension ) && !extension.empty( ) )
		{
			while ( cursor.read_string( folder ) && !folder.empty( ) )
			{
				while ( cursor.read_string( name ) && !name.empty( ) )
				{
					std::uint32_t crc{};
					std::uint16_t preload_bytes{};
					std::uint16_t archive_index{};
					std::uint32_t entry_offset{};
					std::uint32_t entry_length{};
					std::uint16_t terminator{};

					if ( !cursor.read( crc ) || !cursor.read( preload_bytes ) || !cursor.read( archive_index )
						|| !cursor.read( entry_offset ) || !cursor.read( entry_length ) || !cursor.read( terminator ) )
					{
						return mounted != 0;
					}

					packed_file file{};

					// The head of the file sits inline in the tree, immediately after the entry. Small
					// files are stored entirely this way and carry a zero length.
					if ( preload_bytes != 0u && !cursor.read_bytes( preload_bytes, file.preload ) )
					{
						return mounted != 0;
					}

					file.length = entry_length;

					if ( entry_length != 0u )
					{
						if ( archive_index == custom_model_detail::k_vpk_inline_archive )
						{
							file.archive = self;
							file.offset = data_base + entry_offset;
						}
						else
						{
							file.archive = custom_model_detail::sibling_archive( archive, archive_index );
							file.offset = entry_offset;
						}
					}

					const auto internal = custom_model_detail::join_vpk_path( folder, name, extension );
					const auto normalized = custom_model_detail::normalize( internal );
					const auto key = custom_model_detail::game_key( normalized );
					const auto total = file.preload.size( ) + entry_length;

					if ( custom_model_detail::has_extension( normalized, custom_model_detail::k_model_extension ) )
					{
						entries.push_back( {
							.relative = archive_relative + "::" + internal,
							.display = custom_model_detail::display_name( internal ),
							.game_path = custom_model_detail::strip_compiled_suffix( key ),
							.size = total,
							.packed = true
						} );

						sources.push_back( {
							.path = file.archive,
							.offset = file.offset,
							.length = file.length,
							.preload = file.preload
						} );
					}
					else if ( custom_model_detail::has_extension( normalized, custom_model_detail::k_source_extension ) )
					{
						++uncompiled;
					}

					// Both addresses, same as a loose file: the one the archive's own tree implies and the
					// synthetic one, so a .vpk holding a single bare model is still selectable.
					packed.emplace( key, file );
					packed.emplace( std::string{ custom_model_detail::k_synthetic_root } + normalized, std::move( file ) );

					++mounted;
				}
			}
		}

		return mounted != 0;
	}

	/// Count what one model needs and cannot get. There is no engine call for "does this file exist" -- the
	/// filesystem interface we hook is a read, not a query -- so the answer comes out of the model's own
	/// reference list checked against the two tables the refresh just built.
	void custom_models::scan_dependencies( entry& target, const scan_source& source,
		const std::unordered_map<std::string, std::wstring>& loose,
		const std::unordered_map<std::string, packed_file>& packed ) const
	{
		const auto space = custom_model_detail::content_namespace( target.game_path );
		if ( space.empty( ) )
		{
			return;
		}

		const auto head = custom_model_detail::read_scan_head( source.path, source.offset, source.length, source.preload );
		if ( head.empty( ) )
		{
			return;
		}

		constexpr std::string_view materials{ "materials/" };

		for ( const auto& reference : custom_model_detail::collect_references( head ) )
		{
			const auto normalized = custom_model_detail::normalize( reference );

			// Materials are addressed from the game root and everything else from the content tree, so the
			// addon's namespace sits either at the front or one segment in.
			const std::string_view within = normalized.starts_with( materials )
				? std::string_view{ normalized }.substr( materials.size( ) )
				: std::string_view{ normalized };

			// Outside the addon's own namespace, which means it is asking for something the base game
			// ships -- weapon materials, the shared character skeletons, the animation graphs. Those are
			// not ours to provide and the engine will find them itself.
			if ( !within.starts_with( space ) )
			{
				continue;
			}

			// RERL names the uncompiled resource; what an addon actually ships is the compiled one. Both
			// spellings are checked because a hand-built folder can contain either.
			if ( loose.contains( normalized + "_c" ) || packed.contains( normalized + "_c" )
				|| loose.contains( normalized ) || packed.contains( normalized ) )
			{
				continue;
			}

			// The first one is the one the menu names. Fifty-eight missing files are always fifty-eight
			// files out of the same content pack, and naming one of them is what tells the user which.
			if ( target.missing_files == 0 )
			{
				target.missing_sample = reference;
			}

			++target.missing_files;
		}
	}

	std::wstring custom_models::resolve( std::string_view request ) const
	{
		if ( !this->m_has_files.load( std::memory_order_relaxed ) || request.empty( ) )
		{
			return {};
		}

		auto key = custom_model_detail::normalize( request );
		if ( key.empty( ) )
		{
			return {};
		}

		const std::scoped_lock lock{ this->m_vfs_mutex };

		if ( const auto found = this->m_vfs.find( key ); found != this->m_vfs.end( ) )
		{
			return found->second;
		}

		// The table is keyed the way the engine normally asks -- with the _c. Somebody asking for the
		// source name wants the compiled file next to it.
		if ( !key.ends_with( "_c" ) )
		{
			key += "_c";

			if ( const auto found = this->m_vfs.find( key ); found != this->m_vfs.end( ) )
			{
				return found->second;
			}
		}

		return {};
	}

	bool custom_models::read_packed( std::string_view request, std::vector<std::uint8_t>& out ) const
	{
		if ( !this->m_has_files.load( std::memory_order_relaxed ) || request.empty( ) )
		{
			return false;
		}

		auto key = custom_model_detail::normalize( request );
		if ( key.empty( ) )
		{
			return false;
		}

		packed_file file{};

		// The entry is copied out and the lock dropped before any I/O. This runs on the engine's
		// filesystem threads, and holding the mutex across a disk read would stall a refresh -- or, worse,
		// stall every other filesystem thread behind one slow seek.
		{
			const std::scoped_lock lock{ this->m_vfs_mutex };

			auto found = this->m_packed.find( key );

			if ( found == this->m_packed.end( ) && !key.ends_with( "_c" ) )
			{
				key += "_c";
				found = this->m_packed.find( key );
			}

			if ( found == this->m_packed.end( ) )
			{
				return false;
			}

			file = found->second;
		}

		out.clear( );
		out.reserve( file.preload.size( ) + file.length );
		out.insert( out.end( ), file.preload.begin( ), file.preload.end( ) );

		if ( file.length != 0u )
		{
			if ( file.archive.empty( ) )
			{
				return false;
			}

			std::ifstream stream{ std::filesystem::path{ file.archive }, std::ios::binary };
			if ( !stream.is_open( ) )
			{
				return false;
			}

			stream.seekg( static_cast< std::streamoff >( file.offset ) );
			if ( !stream )
			{
				return false;
			}

			const auto base = out.size( );
			out.resize( base + file.length );

			stream.read( reinterpret_cast< char* >( out.data( ) + base ), static_cast< std::streamsize >( file.length ) );

			// A short read means the sibling archive is missing or truncated -- somebody copied the
			// _dir.vpk on its own. Better to fall through to the game's own search paths than to hand the
			// resource system half a file.
			if ( static_cast< std::uint32_t >( stream.gcount( ) ) != file.length )
			{
				out.clear( );
				return false;
			}
		}

		return !out.empty( );
	}

	void custom_models::on_frame_stage_notify( )
	{
		// One scan on the first frame we are alive, so the folder is listed and the VFS table is armed
		// before the menu is ever opened. Everything after that is on the refresh button.
		if ( !this->m_scanned.load( std::memory_order_relaxed ) )
		{
			this->refresh( );
		}

		// A scan can land between two frames and from the other thread, and it can move or delete the file
		// the selection names. Dropping the bind bookkeeping here rather than inside `refresh` keeps all
		// writes to it on this thread, which is the only reason it needs no lock.
		if ( const auto revision = this->m_revision.load( std::memory_order_acquire ); revision != this->m_seen_revision )
		{
			this->m_seen_revision = revision;
			this->m_bound_path.clear( );
			this->m_bound = false;
			this->m_blocked_relative.clear( );
		}

		// A map change invalidates the same bookkeeping, for a different reason: the precache below is only
		// good for the world it ran against. Keep m_bound_path across a level change and the guard at the
		// bind site sees the same selection, skips bind_resource, and hands set_player_model a path whose
		// content the new map's resource system has never heard of -- which is not a missing texture but
		// the "attempting to render with error material" fatal box.
		//
		// Read here rather than cleared from the level hooks: this is the only thread that writes any of
		// it, which is what lets it go without a lock.
		if ( const auto generation = systems::g_entities.generation( ); generation != this->m_seen_generation )
		{
			this->m_seen_generation = generation;
			this->m_bound_path.clear( );
			this->m_bound = false;
			this->m_blocked_relative.clear( );

			// The old map's pawn is gone with it, so nothing we recorded about the model on it still
			// describes anything. Waiting for the pawn address to change is not enough on its own -- the
			// allocator is free to hand the new map's pawn the same address.
			this->reset_tracking( );
			this->m_tracked_pawn = 0;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || systems::g_local.is_in_cinematic( ) || !local.pawn )
		{
			return;
		}

		if ( this->m_tracked_pawn != local.pawn )
		{
			this->reset_tracking( );
			this->m_tracked_pawn = local.pawn;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return;
		}

		const auto model_state = game_scene_node + SCHEMA( "CSkeletonInstance", "m_modelState"_hash );

		// A copy of the row, not a pointer into the list. Everything below runs across `bind_resource`,
		// `set_player_model` and `cycle_weapon_owners`, and a refresh on the render thread inside that
		// window used to leave this pointing at strings a rebuilt vector had already freed.
		std::optional<entry> selected{};

		if ( settings::g_changer.custom_model.value )
		{
			selected = this->find_selected( );
		}

		// The one case where refusing beats applying. Everywhere else a model that half-loads is at least
		// diagnosable, but a player material CS2 cannot find is not a missing texture -- it is a
		// "attempting to render with error material" FATAL ERROR box and a closed game. So a model whose
		// own content is not in the folder is dropped here, and the restore path below puts the original
		// model back as if nothing had been selected.
		auto blocked = false;

		if ( selected && selected->missing_files > 0 )
		{
			if ( this->m_blocked_relative != selected->relative )
			{
				this->m_blocked_relative = selected->relative;

				logging::console::print( xs( "custom model not applied: {} file(s) it needs are not in the folder, starting with {}" ),
					selected->missing_files, selected->missing_sample );
			}

			this->m_status.store( status::missing_content, std::memory_order_relaxed );
			blocked = true;
			selected.reset( );
		}
		else
		{
			this->m_blocked_relative.clear( );
		}

		// Read by the agent changer, which stands down while this is true. Set before the restore path
		// below so that turning the feature off hands control back on the same frame.
		this->m_active = selected.has_value( );

		if ( !selected )
		{
			if ( !blocked && settings::g_changer.custom_model.value && !settings::g_changer.custom_model_file.empty( ) )
			{
				this->m_status.store( status::missing_selection, std::memory_order_relaxed );
			}

			if ( this->m_overridden && !this->m_original_model.empty( ) )
			{
				memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, this->m_original_model.c_str( ) );
				this->cycle_weapon_owners( local.pawn );

				this->m_applied_handle = 0;
				this->m_applied_relative.clear( );
				this->m_overridden = false;
			}

			return;
		}

		if ( !this->m_overridden && this->m_original_model.empty( ) )
		{
			const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
			if ( model_name_ptr )
			{
				this->m_original_model = memory::read_string( model_name_ptr );
			}
		}

		// Once per selection, not once per frame: precaching walks the resource dependencies and there
		// is no reason to redo that for a model already in the system.
		if ( this->m_bound_path != selected->game_path )
		{
			this->m_bound = this->bind_resource( selected->game_path );
			this->m_bound_path = selected->game_path;
			this->m_status.store( this->m_bound ? status::bound : status::bind_failed, std::memory_order_relaxed );

			if ( !this->m_bound )
			{
				logging::console::print( xs( "custom model failed to load: {}" ), selected->game_path );
			}
		}

		const auto current_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		const auto selection_matches = ( this->m_applied_relative == selected->relative );
		const auto handle_matches = ( this->m_applied_handle != 0 && current_handle == this->m_applied_handle );

		// The handle check is what makes this self-healing. Respawns, socache refreshes and the game's
		// own econ pass all rewrite the model out from under us, and the only visible symptom is a
		// handle that no longer matches the one we applied.
		if ( this->m_overridden && selection_matches && handle_matches )
		{
			return;
		}

		// Applied even when the bind check failed. A model that loads as an error mesh is at least
		// visibly diagnosable, whereas refusing to apply it looks identical to the feature being off --
		// and the bind check is inferred from the skybox path, so it is not the thing to gate on.
		memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, selected->game_path.c_str( ) );

		// Hulls come out of the model, and an arbitrary one has no reason to be player-shaped. A figure
		// with 20-unit-wide bounds is nearly unhittable and one with 200-unit bounds cannot fit through
		// a doorway, so the standing player hull gets stamped back over whatever the mesh asked for.
		const auto collision = local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ), math::vector3( -16.0f, -16.0f, 0.0f ) );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ), math::vector3( 16.0f, 16.0f, 72.0f ) );

		this->cycle_weapon_owners( local.pawn );

		this->m_applied_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		this->m_applied_relative = selected->relative;
		this->m_overridden = true;
	}

	std::shared_ptr<const std::vector<custom_models::entry>> custom_models::entries( ) const
	{
		const std::scoped_lock lock{ this->m_list_mutex };

		// Never null, so no caller has to check. Before the first scan there is a list -- it is empty.
		if ( !this->m_entries )
		{
			static const auto nothing = std::make_shared<const std::vector<entry>>( );
			return nothing;
		}

		return this->m_entries;
	}

	std::optional<custom_models::entry> custom_models::find_selected( ) const
	{
		const auto& wanted = settings::g_changer.custom_model_file.value;
		if ( wanted.empty( ) )
		{
			return std::nullopt;
		}

		const auto snapshot = this->entries( );

		const auto found = std::ranges::find_if( *snapshot, [ & ]( const entry& candidate )
		{
			return _stricmp( candidate.relative.c_str( ), wanted.c_str( ) ) == 0;
		} );

		if ( found == snapshot->end( ) )
		{
			return std::nullopt;
		}

		return *found;
	}

	void custom_models::reset_tracking( )
	{
		this->m_original_model.clear( );
		this->m_applied_relative.clear( );
		this->m_applied_handle = 0;
		this->m_overridden = false;
	}

	/// Detach and immediately re-attach every held weapon. The viewmodel and the world weapon are
	/// parented to bones on the old skeleton, and swapping the model leaves them hanging off nothing
	/// until the ownership write forces the game to rebuild those attachments.
	void custom_models::cycle_weapon_owners( std::uintptr_t pawn ) const
	{
		const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
		if ( !weapon_services )
		{
			return;
		}

		const auto weapons_base = weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hMyWeapons"_hash );
		const auto weapons_size = memory::read<int>( weapons_base );
		const auto weapons_data = memory::read<std::uintptr_t>( weapons_base + 0x8 );

		if ( !weapons_data || weapons_size <= 0 )
		{
			return;
		}

		for ( auto i = 0; i < weapons_size; ++i )
		{
			const auto handle = memory::read<std::uint32_t>( weapons_data + i * sizeof( std::uint32_t ) );
			const auto weapon = systems::g_entities.lookup( handle );

			if ( !weapon )
			{
				continue;
			}

			const auto owner_off = SCHEMA( "C_BaseEntity", "m_hOwnerEntity"_hash );
			const auto saved_owner = memory::read<std::uint32_t>( weapon + owner_off );

			memory::write<std::uint32_t>( weapon + owner_off, 0xffffffff );
			memory::write<std::uint32_t>( weapon + owner_off, saved_owner );
		}
	}

	/// Push the model through the resource system and read the binding back. Two reasons this is worth
	/// the round trip rather than just calling set_player_model and hoping: the resource has to be in
	/// the system before the pawn is pointed at it, and the binding read is the only honest answer to
	/// "did that file actually load", which is what the menu's status line reports.
	bool custom_models::bind_resource( const std::string& game_path ) const
	{
		struct buffer_string
		{
			std::uint32_t m_unknown1{};
			std::uint32_t m_unknown2{ 0xc00000c8 };

			union
			{
				std::uintptr_t m_str_ptr;
				std::uint8_t data[ 0xc8 ];
			};

			std::uintptr_t m_unknown3{};
			std::uintptr_t m_unknown4{};
		} buffer;

		const auto init_path_buffer = PATTERN (patterns::init_particle_path_buffer);
		const auto precache_resource = PATTERN (patterns::resource_system_precache);

		if ( !init_path_buffer || !precache_resource || !addresses::globals::resource_system )
		{
			return false;
		}

		// 'ldmv' is "vmdl" backwards: the type tag is a little-endian FourCC, the same way the skybox
		// path hands over 'xetv' for a vtex and the weather particles hand over 'fcpv'.
		memory::call<void>( init_path_buffer, &buffer, game_path.c_str( ) );
		buffer.m_unknown4 = 'ldmv';

		memory::call<void>( precache_resource, addresses::globals::resource_system, &buffer, "" );

		// The buffer is consumed by the call above, so it gets rebuilt for the lookup.
		memory::call<void>( init_path_buffer, &buffer, game_path.c_str( ) );
		buffer.m_unknown4 = 'ldmv';

		const auto binding = memory::call_vfunc<std::uintptr_t>( addresses::globals::resource_system, 79, &buffer, 0ll );
		const auto model = binding ? memory::safe_read<std::uintptr_t>( binding ) : std::nullopt;

		return model.has_value( ) && *model != 0;
	}

	const char* custom_models::status_text( ) const
	{
		switch ( this->m_status.load( std::memory_order_relaxed ) )
		{
		case status::no_directory:
			return "cannot open %appdata%\\velocity\\models";

		case status::no_models:
			return "drop a .vmdl_c or a workshop .vpk in the folder, then refresh";

		case status::missing_selection:
			return "selected model is no longer in the folder";

		case status::missing_content:
			return "this model's own materials are missing - it needs its content pack too";

		case status::bound:
			return "model loaded";

		case status::bind_failed:
			return "model failed to load - see readme.txt for the folder layout";

		default:
			return "";
		}
	}

} // namespace features::changer
