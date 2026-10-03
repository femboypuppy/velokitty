#include <pch/pch.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <protection/game_addresses.hpp>
#include <core/settings.hpp>
#include <core/systems/systems.hpp>
#include <core/rendering/rendering.hpp>
#include <core/features/features.hpp>

namespace features::misc {

	namespace {

		constexpr auto k_application_id { "1555802790708781076" };

		/// The art asset key the activity asks for. It only shows once an image with this exact name is
		/// uploaded under Rich Presence > Art Assets in the Discord developer portal.
		constexpr auto k_large_image { "aimwhere" };

		enum class opcode : std::uint32_t { handshake = 0, frame = 1, close = 2, ping = 3, pong = 4 };

		[[nodiscard]] std::string json_escape (std::string_view in) {
			std::string out {};
			out.reserve (in.size () + 8);
			for (const auto c : in) {
				switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char> (c) >= 0x20)
						out += c;
				}
			}
			return out;
		}

		/// The map name normally comes from the level-init hook, which never fires if the cheat is injected
		/// mid-match. The engine's global vars keep the current map as a C string (0x180 is the path, 0x188
		/// the short name in current builds); both are tried and only a plausible map name is accepted, so a
		/// moved field reads as "no map" instead of garbage.
		[[nodiscard]] std::string map_from_global_vars () {
			const auto global_vars = memory::safe_read<std::uintptr_t> (addresses::globals::global_vars);
			if (!global_vars || !*global_vars)
				return {};

			for (const auto offset : { 0x188, 0x180 }) {
				const auto ptr = memory::safe_read<std::uintptr_t> (*global_vars + offset);
				if (!ptr || *ptr < 0x10000)
					continue;

				auto name = memory::read_string (*ptr, 128);
				if (const auto slash = name.find_last_of ('/'); slash != std::string::npos)
					name.erase (0, slash + 1);
				if (const auto dot = name.find ('.'); dot != std::string::npos)
					name.erase (dot);

				const auto valid = name.size () >= 3 && name != "<empty>" && std::ranges::all_of (name, [] (char c) {
					return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
				});
				if (valid)
					return name;
			}
			return {};
		}

		/// A middle dot between parts, spelled as UTF-8 bytes so it doesn't depend on the source encoding.
		constexpr auto k_separator { " \xC2\xB7 " };

		/// The controller's rank type is what the scoreboard keys its rank display on: 11 is a Premier rating,
		/// so it is the one thing that tells Premier apart from a plain competitive match (both are 0/1).
		constexpr std::int8_t k_rank_type_premier { 11 };

		struct mode_info {
			std::string name {};
			bool has_rounds {};
		};

		[[nodiscard]] mode_info current_mode (std::uintptr_t controller) {
			const auto game_type_var = CONVAR ("game_type");
			const auto game_mode_var = CONVAR ("game_mode");
			if (!game_type_var || !game_mode_var)
				return {};

			const auto game_type = game_type_var->get<int> ();
			const auto game_mode = game_mode_var->get<int> ();

			if (game_type == 0) {
				switch (game_mode) {
				case 0: return { "casual", true };
				case 1: {
					const auto rank_type = memory::safe_read<std::int8_t> (controller + SCHEMA ("CCSPlayerController", "m_iCompetitiveRankType"_hash)).value_or (0);
					return { rank_type == k_rank_type_premier ? "premier" : "competitive", true };
				}
				case 2: return { "wingman", true };
				}
			}
			else if (game_type == 1) {
				switch (game_mode) {
				case 0: return { "arms race", false };
				case 2: return { "deathmatch", false };
				}
			}
			return {};
		}

		/// Round wins per side, from the C_CSTeam entities. Anything outside a sane range is treated as unread,
		/// so a moved field shows no score rather than a nonsense one.
		[[nodiscard]] std::optional<std::pair<int, int>> team_scores () {
			std::optional<int> ct {}, t {};
			for (const auto& team : systems::g_entities.get_by_type (systems::entities::type::team)) {
				const auto number = memory::safe_read<int> (team.ptr + SCHEMA ("C_BaseEntity", "m_iTeamNum"_hash)).value_or (0);
				const auto score = memory::safe_read<int> (team.ptr + SCHEMA ("C_Team", "m_iScore"_hash)).value_or (-1);
				if (score < 0 || score > 999)
					continue;
				if (number == 3)
					ct = score;
				else if (number == 2)
					t = score;
			}
			if (!ct || !t)
				return std::nullopt;
			return std::pair { *ct, *t };
		}

		class pipe {
		public:
			pipe () = default;
			pipe (const pipe&) = delete;
			pipe& operator= (const pipe&) = delete;
			~pipe () { close (); }

			/// Discord listens on the first free of discord-ipc-0..9; take the first that opens.
			bool open () {
				for (auto i = 0; i < 10; ++i) {
					const auto name = std::format (L"\\\\?\\pipe\\discord-ipc-{}", i);
					m_handle = CreateFileW (name.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
					if (m_handle != INVALID_HANDLE_VALUE)
						return true;
				}
				m_handle = INVALID_HANDLE_VALUE;
				return false;
			}

			void close () {
				if (m_handle != INVALID_HANDLE_VALUE) {
					CloseHandle (m_handle);
					m_handle = INVALID_HANDLE_VALUE;
				}
			}

			[[nodiscard]] bool is_open () const { return m_handle != INVALID_HANDLE_VALUE; }

			/// One frame: little-endian opcode, little-endian length, then the JSON payload.
			bool write (opcode op, std::string_view json) {
				std::string frame (8 + json.size (), '\0');
				const auto op_value = static_cast<std::uint32_t> (op);
				const auto length = static_cast<std::uint32_t> (json.size ());
				std::memcpy (frame.data (), &op_value, 4);
				std::memcpy (frame.data () + 4, &length, 4);
				std::memcpy (frame.data () + 8, json.data (), json.size ());

				DWORD written {};
				return WriteFile (m_handle, frame.data (), static_cast<DWORD> (frame.size ()), &written, nullptr) && written == frame.size ();
			}

			/// Reads one frame if a whole header is waiting; never blocks on an empty pipe. False on a broken pipe.
			bool poll (std::optional<std::pair<opcode, std::string>>& out) {
				out.reset ();

				DWORD available {};
				if (!PeekNamedPipe (m_handle, nullptr, 0, nullptr, &available, nullptr))
					return false;
				if (available < 8)
					return true;

				std::uint32_t header [2] {};
				if (!read_exact (header, sizeof (header)))
					return false;

				std::string payload (header [1], '\0');
				if (header [1] && !read_exact (payload.data (), header [1]))
					return false;

				out.emplace (static_cast<opcode> (header [0]), std::move (payload));
				return true;
			}

			/// Waits up to `timeout` for a frame, for the handshake reply.
			bool wait (std::optional<std::pair<opcode, std::string>>& out, std::chrono::milliseconds timeout) {
				const auto deadline = std::chrono::steady_clock::now () + timeout;
				while (std::chrono::steady_clock::now () < deadline) {
					if (!poll (out))
						return false;
					if (out)
						return true;
					Sleep (25);
				}
				return true;
			}

		private:
			bool read_exact (void* dst, std::uint32_t size) {
				auto* p = static_cast<std::uint8_t*> (dst);
				while (size) {
					DWORD got {};
					if (!ReadFile (m_handle, p, size, &got, nullptr) || !got)
						return false;
					p += got;
					size -= got;
				}
				return true;
			}

			HANDLE m_handle { INVALID_HANDLE_VALUE };
		};

	} // namespace

	void discord_rpc::on_frame_stage_notify () {
		const auto now = clock::now ();
		if (now < m_next_publish)
			return;
		m_next_publish = now + std::chrono::seconds (1);

		if (m_shut_down)
			return;

		if (!m_thread.joinable ()) {
			m_start_unix = static_cast<std::int64_t> (std::time (nullptr));
			m_thread = std::jthread ([this] (std::stop_token stop) { run (stop); });
		}

		// All of this is game-thread state; the worker only ever sees the finished strings. The controller is
		// read directly rather than through g_local, which drops to nothing whenever there is no pawn -- a
		// spectator, or the moment between joining and spawning -- and that read as the main menu.
		const auto controller = memory::safe_read<std::uintptr_t> (addresses::globals::local_player_controller).value_or (0);

		std::string details {};
		std::string state {};
		if (controller) {
			if (rendering::g_widgets.s_map_name.empty ())
				rendering::g_widgets.s_map_name = map_from_global_vars ();
			const auto& map = rendering::g_widgets.s_map_name;

			// details: "wingman · de_vertigo"
			const auto mode = current_mode (controller);
			if (!mode.name.empty ())
				details = map.empty () ? mode.name : mode.name + k_separator + map;
			else
				details = map.empty () ? "in a match" : "playing " + map;

			// state: "ct 7 - 5 t · alive", own side first
			const auto team = memory::safe_read<int> (controller + SCHEMA ("C_BaseEntity", "m_iTeamNum"_hash)).value_or (0);
			const auto alive = memory::safe_read<bool> (controller + SCHEMA ("CCSPlayerController", "m_bPawnIsAlive"_hash)).value_or (false);

			const auto game_rules = memory::safe_read<std::uintptr_t> (addresses::globals::game_rules).value_or (0);
			const auto warmup = game_rules && memory::safe_read<bool> (game_rules + SCHEMA ("C_CSGameRules", "m_bWarmupPeriod"_hash)).value_or (false);
			const auto match_over = game_rules && memory::safe_read<int> (game_rules + SCHEMA ("C_CSGameRules", "m_gamePhase"_hash)).value_or (0) == 5;

			std::vector<std::string> parts {};
			if (warmup)
				parts.emplace_back ("warmup");
			else if (match_over)
				parts.emplace_back ("match over");

			const auto scores = mode.has_rounds && !warmup ? team_scores () : std::nullopt;
			if (team == 2 || team == 3) {
				if (scores) {
					parts.emplace_back (team == 3
						? std::format ("ct {} - {} t", scores->first, scores->second)
						: std::format ("t {} - {} ct", scores->second, scores->first));
				}
				else if (parts.empty ()) {
					parts.emplace_back (team == 3 ? "counter-terrorist" : "terrorist");
				}
				if (!match_over)
					parts.emplace_back (alive ? "alive" : "dead");
			}
			else {
				parts.emplace_back ("spectating");
				if (scores)
					parts.emplace_back (std::format ("ct {} - {} t", scores->first, scores->second));
			}

			for (const auto& part : parts) {
				if (!state.empty ())
					state += k_separator;
				state += part;
			}
		} else {
			details = "in the main menu";
		}

		std::scoped_lock lock (m_mutex);
		if (details != m_details || state != m_state) {
			m_details = std::move (details);
			m_state = std::move (state);
			++m_revision;
		}
	}

	void discord_rpc::shutdown () {
		m_shut_down = true;
		if (m_thread.joinable ()) {
			m_thread.request_stop ();
			m_wake.notify_all ();
			m_thread.join ();
		}
	}

	void discord_rpc::run (std::stop_token stop) {
		pipe ipc {};
		std::uint64_t sent_revision {};
		auto next_connect = clock::now ();
		auto last_send = clock::time_point {};
		auto nonce = 0u;
		bool logged_unavailable {};

		const auto pid = GetCurrentProcessId ();

		const auto send_activity = [&] (bool clear) -> bool {
			std::string details {}, state {};
			{
				std::scoped_lock lock (m_mutex);
				details = m_details;
				state = m_state;
			}

			std::string activity {};
			if (!clear) {
				activity = std::format (R"(,"activity":{{"details":"{}")", json_escape (details));
				if (!state.empty ())
					activity += std::format (R"(,"state":"{}")", json_escape (state));
				activity += std::format (R"(,"timestamps":{{"start":{}}},"assets":{{"large_image":"{}","large_text":"aimwhere"}},"instance":false}})",
					m_start_unix, k_large_image);
			}

			const auto json = std::format (R"({{"cmd":"SET_ACTIVITY","args":{{"pid":{}{}}},"nonce":"{}"}})", pid, activity, ++nonce);
			return ipc.write (opcode::frame, json);
		};

		const auto disconnect = [&] {
			ipc.close ();
			next_connect = clock::now () + std::chrono::seconds (15);
		};

		std::mutex wait_mutex {};
		while (!stop.stop_requested ()) {
			// A one-second beat; only an unload cuts it short. Text changes are picked up on the next beat --
			// waking on them early would only spin against the five-second send limit.
			{
				std::unique_lock lock (wait_mutex);
				m_wake.wait_for (lock, stop, std::chrono::seconds (1), [] { return false; });
			}
			if (stop.stop_requested ())
				break;

			if (!settings::g_misc.m_discord_rpc.enabled.value) {
				if (ipc.is_open ()) {
					(void)send_activity (true);
					ipc.close ();
					sent_revision = 0;
				}
				continue;
			}

			const auto now = clock::now ();

			if (!ipc.is_open ()) {
				if (now < next_connect)
					continue;

				if (!ipc.open ()) {
					if (!logged_unavailable) {
						logging::console::print (xs ("[discord] no discord-ipc pipe; retrying every 15s\n"));
						logged_unavailable = true;
					}
					next_connect = now + std::chrono::seconds (15);
					continue;
				}

				std::optional<std::pair<opcode, std::string>> reply {};
				if (!ipc.write (opcode::handshake, std::format (R"({{"v":1,"client_id":"{}"}})", k_application_id))
					|| !ipc.wait (reply, std::chrono::seconds (3)) || !reply || reply->first != opcode::frame) {
					logging::console::print (xs ("[discord] handshake failed{}{}\n"), reply ? ": " : "", reply ? reply->second : std::string {});
					disconnect ();
					continue;
				}

				logging::console::print (xs ("[discord] connected\n"));
				logged_unavailable = false;
				sent_revision = 0;
			}

			// Drain whatever Discord sent: answer pings, drop the pipe on a close.
			for (;;) {
				std::optional<std::pair<opcode, std::string>> frame {};
				if (!ipc.poll (frame)) {
					disconnect ();
					break;
				}
				if (!frame)
					break;
				if (frame->first == opcode::ping)
					(void)ipc.write (opcode::pong, frame->second);
				else if (frame->first == opcode::close) {
					logging::console::print (xs ("[discord] closed by discord: {}\n"), frame->second);
					disconnect ();
					break;
				}
				else if (frame->second.find (R"("evt":"ERROR")") != std::string::npos)
					logging::console::print (xs ("[discord] error: {}\n"), frame->second);
			}
			if (!ipc.is_open ())
				continue;

			std::uint64_t revision {};
			{
				std::scoped_lock lock (m_mutex);
				revision = m_revision;
			}

			if (revision == sent_revision || now - last_send < std::chrono::seconds (5))
				continue;

			if (!send_activity (false)) {
				disconnect ();
				continue;
			}

			sent_revision = revision;
			last_send = now;
		}

		// Leave nothing behind on the profile after an unload.
		if (ipc.is_open ())
			(void)send_activity (true);
	}

} // namespace features::misc
