#include <pch/pch.hpp>
#include <utilities/logging/logging.hpp>
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

		// Map name and team are game-thread state; the worker only ever sees these copies.
		const auto local = systems::g_local.get ();
		const auto& map = rendering::g_widgets.s_map_name;

		std::string details {};
		std::string state {};
		if (local.controller && !map.empty ()) {
			details = "playing " + map;
			state = local.team == 3 ? "counter-terrorist" : local.team == 2 ? "terrorist" : "spectating";
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
