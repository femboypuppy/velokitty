#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/steam/steam.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::misc {

	namespace {

		/// 'AWHR'. Any channel number works as long as nothing else in the process reads it; the game's
		/// own peer-to-peer traffic goes through its private copy of the networking library anyway.
		constexpr int k_channel { 0x41574852 };

		constexpr std::array<char, 8> k_magic { 'A', 'I', 'M', 'W', 'H', 'E', 'R', 'E' };
		constexpr std::uint8_t k_version { 1 };
		constexpr std::uint8_t k_hello { 0 };
		constexpr std::uint8_t k_ack { 1 };

		struct packet {
			std::array<char, 8> magic { k_magic };
			std::uint8_t version { k_version };
			std::uint8_t kind {};
		};
		static_assert(sizeof (packet) == 10);

		constexpr std::uint64_t k_steam_id_base { 76561197960265728ull };

		/// A user who goes this long without a word stops counting -- they unloaded, or left.
		constexpr auto k_forget_after = std::chrono::seconds (35);

		/// Hellos to known users keep them from being forgotten. Players who have never answered are asked
		/// less and less often: a stock client drops every one, and anyone who injects later says hello
		/// themselves, which is answered straight away.
		[[nodiscard]] std::chrono::seconds hello_interval (bool known, int hellos) {
			if (known)
				return std::chrono::seconds (10);
			if (hellos < 3)
				return std::chrono::seconds (10);
			if (hellos < 10)
				return std::chrono::seconds (30);
			return std::chrono::seconds (60);
		}

		/// Steam IDs of the humans currently in the server; bots carry no Steam ID.
		[[nodiscard]] std::vector<std::uint64_t> match_players () {
			std::vector<std::uint64_t> out {};
			for (const auto& player : systems::g_entities.get_by_type (systems::entities::type::player)) {
				if (!player.ptr)
					continue;

				const auto id = memory::safe_read<std::uint64_t> (
					player.ptr + SCHEMA ("CBasePlayerController", "m_steamID"_hash)).value_or (0);
				if (id >= k_steam_id_base)
					out.push_back (id);
			}
			return out;
		}

		static constexpr const char* k_badge_script = R"PANORAMA(
(function () {

	SAimwhere = (function () {

		function isValid(panel) {
			return panel && panel.IsValid();
		}

		function getScoreboard() {
			var root = $.GetContextPanel();
			if (!isValid(root)) return null;
			var scoreboard = root.id === "Scoreboard" ? root : root.FindChildTraverse("Scoreboard");
			return isValid(scoreboard) ? scoreboard : null;
		}

		function getRow(sb, xuid, account_id) {
			var keys = [xuid, account_id];
			var prefixes = ["player-", "id-", "id-player-", "player_"];
			for (var i = 0; i < keys.length; ++i) {
				for (var j = 0; j < prefixes.length; ++j) {
					var row = sb.FindChildTraverse(prefixes[j] + keys[i]);
					if (isValid(row)) return row;
				}
			}
			return null;
		}

		function part(parent, id, type) {
			var p = $.CreatePanel(type || "Panel", parent, id);
			p.hittest = false;
			return p;
		}

		// The mark rebuilt out of panels, since the scoreboard can only load compiled images: a dark tile,
		// an accent ring with four ticks crossing it, and a white question mark in the middle.
		function makeBadge(parent, id, accent) {
			var tile = part(parent, id);
			tile.AddClass("aimwhere-badge");
			tile.style.width = "16px";
			tile.style.height = "16px";
			tile.style.verticalAlign = "center";
			tile.style.marginRight = "4px";
			tile.style.backgroundColor = "#111111";
			tile.style.borderRadius = "3px";
			tile.style.border = "1px solid rgba(255,255,255,0.18)";

			var ring = part(tile, id + "-ring");
			ring.style.width = "10px";
			ring.style.height = "10px";
			ring.style.horizontalAlign = "center";
			ring.style.verticalAlign = "center";
			ring.style.borderRadius = "50%";
			ring.style.border = "1px solid " + accent;

			var ticks = [["center", "top", "1px", "4px"], ["center", "bottom", "1px", "4px"],
			             ["left", "center", "4px", "1px"], ["right", "center", "4px", "1px"]];
			for (var i = 0; i < ticks.length; ++i) {
				var t = part(tile, id + "-tick" + i);
				t.style.horizontalAlign = ticks[i][0];
				t.style.verticalAlign = ticks[i][1];
				t.style.width = ticks[i][2];
				t.style.height = ticks[i][3];
				t.style.backgroundColor = accent;
			}

			var q = part(tile, id + "-q", "Label");
			q.text = "?";
			q.style.horizontalAlign = "center";
			q.style.verticalAlign = "center";
			q.style.textAlign = "center";
			q.style.fontSize = "9px";
			q.style.fontWeight = "bold";
			q.style.color = "#FFFFFF";
			q.style.margin = "0px";
			q.style.padding = "0px";
			return tile;
		}

		function recolor(tile, id, accent) {
			var ring = tile.FindChild(id + "-ring");
			if (isValid(ring)) ring.style.border = "1px solid " + accent;
			for (var i = 0; i < 4; ++i) {
				var t = tile.FindChild(id + "-tick" + i);
				if (isValid(t)) t.style.backgroundColor = accent;
			}
		}

		return {
			// users: [{x: steamid64 string, a: account id string}]. Strings, because a steamid64 does not
			// survive a JavaScript number.
			update: function (users, accent) {
				try {
					var sb = getScoreboard();
					if (!sb) return;

					var wanted = {};
					for (var i = 0; i < users.length; ++i) {
						var u = users[i];
						var id = "aimwhere-badge-" + u.x;
						wanted[id] = true;

						var row = getRow(sb, u.x, u.a);
						if (!row) continue;
						var icons = row.FindChildTraverse("id-sb-name__nameicons");
						if (!isValid(icons)) continue;

						var tile = icons.FindChild(id);
						if (!isValid(tile)) {
							tile = makeBadge(icons, id, accent);
							var first = icons.GetChild(0);
							if (isValid(first) && first !== tile) icons.MoveChildBefore(tile, first);
						} else {
							recolor(tile, id, accent);
						}
						tile.style.visibility = "visible";
					}

					// Panels stay put once made (the rows cache their paint), so a user who left is hidden,
					// not deleted.
					var badges = sb.FindChildrenWithClassTraverse("aimwhere-badge");
					for (var j = 0; j < badges.length; ++j) {
						if (isValid(badges[j]) && !wanted[badges[j].id]) badges[j].style.visibility = "collapse";
					}
				} catch (e) {}
			}
		};

	})();

})();
)PANORAMA";

	} // namespace

	void aimwhere_users::on_frame_stage_notify () {
		if (!settings::g_misc.m_aimwhere_users.enabled.value) {
			if (m_networking_ready) {
				// Turning the setting off makes us unfindable too: stop answering and drop everyone.
				shutdown ();
				m_networking_ready = false;
			}
			return;
		}

		if (!m_networking_ready) {
			if (m_networking_failed)
				return;

			m_local_id = steam::user::get_steam_id ();
			if (!steam::networking::initialize () || m_local_id < k_steam_id_base) {
				m_networking_failed = true;
				logging::console::print (xs ("[aimwhere] steam networking unavailable; badges disabled\n"));
				return;
			}

			m_networking_ready = true;
			logging::console::print (xs ("[aimwhere] networking ready, local={}\n"), m_local_id.load ());
		}

		const auto now = clock::now ();

		if (now >= m_next_pump) {
			m_next_pump = now + std::chrono::milliseconds (250);
			pump_messages (now);
		}

		if (now >= m_next_greet) {
			m_next_greet = now + std::chrono::seconds (1);
			greet_players (now);
		}

		update_scoreboard ();
	}

	void aimwhere_users::on_level_change () {
		m_script_injected = false;
		m_scoreboard_open = false;
		m_scoreboard_frames = 0;
		m_sent_badges.clear ();
		m_ui_engine = nullptr;
		m_script_panel = nullptr;

		// Known users carry over (a map change keeps the same people), but everyone gets greeted again soon
		// so the new server's players are found without waiting out the backoff.
		std::scoped_lock lock (m_mutex);
		m_peers.clear ();
	}

	void aimwhere_users::shutdown () {
		std::vector<std::uint64_t> ids {};
		{
			std::scoped_lock lock (m_mutex);
			for (const auto& [id, _] : m_users)
				ids.push_back (id);
			for (const auto& [id, _] : m_peers)
				ids.push_back (id);
			m_users.clear ();
			m_peers.clear ();
		}

		for (const auto id : ids)
			steam::networking::close_channel (id, k_channel);

		m_sent_badges.clear ();
	}

	bool aimwhere_users::is_user (std::uint64_t steam_id) const {
		if (!steam_id || !m_networking_ready || !settings::g_misc.m_aimwhere_users.enabled.value)
			return false;

		if (steam_id == m_local_id)
			return true;

		std::scoped_lock lock (m_mutex);
		return m_users.contains (steam_id);
	}

	bool aimwhere_users::say (std::uint64_t steam_id, std::uint8_t kind) const {
		packet p {};
		p.kind = kind;
		return steam::networking::send (steam_id, k_channel, &p, sizeof (p));
	}

	void aimwhere_users::pump_messages (clock::time_point now) {
		std::vector<steam::networking::message> inbox {};
		steam::networking::receive (k_channel, inbox);

		for (const auto& msg : inbox) {
			if (msg.sender < k_steam_id_base || msg.sender == m_local_id || msg.data.size () < sizeof (packet))
				continue;

			packet p {};
			std::memcpy (&p, msg.data.data (), sizeof (p));
			if (p.magic != k_magic || p.version != k_version)
				continue;

			bool is_new {};
			{
				std::scoped_lock lock (m_mutex);
				is_new = !m_users.contains (msg.sender);
				m_users [msg.sender] = now;
			}

			if (is_new)
				logging::console::print (xs ("[aimwhere] user detected: {}\n"), msg.sender);

			// Answer every hello, so whoever injected last learns about us on their first try.
			if (p.kind == k_hello)
				(void)say (msg.sender, k_ack);
		}

		std::scoped_lock lock (m_mutex);
		std::erase_if (m_users, [&] (const auto& entry) {
			const bool stale = now - entry.second > k_forget_after;
			if (stale)
				logging::console::print (xs ("[aimwhere] user gone quiet: {}\n"), entry.first);
			return stale;
		});
	}

	void aimwhere_users::greet_players (clock::time_point now) {
		const auto players = match_players ();

		for (const auto id : players) {
			if (id == m_local_id)
				continue;

			// A hello that arrives before we have sent anything waits as a session request; accepting it here
			// lets the message through on the next pump instead of whenever our own hello goes out.
			(void)steam::networking::accept (id);

			bool known {};
			peer state {};
			{
				std::scoped_lock lock (m_mutex);
				known = m_users.contains (id);
				state = m_peers [id];
			}

			if (state.hellos > 0 && now - state.last_hello < hello_interval (known, state.hellos))
				continue;

			// A refused send counts as a try too, or a peer Steam will not route to would be retried every second.
			(void)say (id, k_hello);

			std::scoped_lock lock (m_mutex);
			auto& p = m_peers [id];
			p.last_hello = now;
			++p.hellos;
		}
	}

	void aimwhere_users::update_scoreboard () {
		const auto scoreboard_open = (GetAsyncKeyState (VK_TAB) & 0x8000) != 0;
		if (!scoreboard_open) {
			m_scoreboard_open = false;
			return;
		}

		if (!m_scoreboard_open) {
			// The scoreboard rebuilds its rows when it opens, so every open starts from a clean send.
			m_scoreboard_open = true;
			m_scoreboard_frames = 0;
			m_sent_badges.clear ();
		}

		++m_scoreboard_frames;

		if (!m_script_injected) {
			if (m_scoreboard_frames % 30 != 1)
				return;

			if (!addresses::globals::panorama)
				return;

			auto* panorama = reinterpret_cast<c_panorama_ui_engine*> (addresses::globals::panorama);
			m_ui_engine = panorama->get_ui_engine ();
			m_script_panel = find_hud_panel ();
			if (!m_ui_engine || !m_script_panel)
				return;

			if (!run_script (k_badge_script))
				return;

			m_script_injected = true;
			logging::console::print (xs ("[aimwhere] scoreboard badge script injected\n"));
		}

		if (m_scoreboard_frames % 8 != 0)
			return;

		std::string users_json = "[";
		const auto add = [&] (std::uint64_t id) {
			if (users_json.size () > 1)
				users_json += ",";
			users_json += std::format (R"({{x:"{}",a:"{}"}})", id, id - k_steam_id_base);
		};

		for (const auto id : match_players ()) {
			if (is_user (id))
				add (id);
		}
		users_json += "]";

		const auto& accent = settings::g_gui.accent.value;
		const auto accent_hex = std::format ("#{:02X}{:02X}{:02X}", static_cast<int> (accent.r), static_cast<int> (accent.g), static_cast<int> (accent.b));
		const auto payload = users_json + accent_hex;

		// Re-sent every second or so even when nothing changed: a row that was rebuilt while open has lost
		// its badge, and the script only creates what is missing.
		if (payload == m_sent_badges && m_scoreboard_frames % 64 != 0)
			return;

		const auto script = std::format (
			R"(if(typeof(SAimwhere)!=='undefined'){{SAimwhere.update({},"{}");}})", users_json, accent_hex);
		if (run_script (script))
			m_sent_badges = payload;
	}

	c_ui_panel* aimwhere_users::find_hud_panel () const {
		if (!addresses::globals::hud)
			return nullptr;

		const auto hud = memory::safe_read<std::uintptr_t> (addresses::globals::hud).value_or (0);
		if (!hud)
			return nullptr;

		const auto panel = memory::safe_read<c_ui_panel*> (hud + 0x8).value_or (nullptr);
		if (!panel)
			return nullptr;

		const auto vtable = memory::safe_read<std::uintptr_t> (reinterpret_cast<std::uintptr_t> (panel));
		return vtable && *vtable ? panel : nullptr;
	}

	bool aimwhere_users::run_script (const std::string& script) {
		if (!m_ui_engine || !m_script_panel)
			return false;

		// The HUD panel and the engine are re-resolved every call: after a disconnect both can be gone, and
		// a script run into a freed panel takes the game down.
		auto* panorama = reinterpret_cast<c_panorama_ui_engine*> (addresses::globals::panorama);
		if (!panorama || panorama->get_ui_engine () != m_ui_engine || find_hud_panel () != m_script_panel) {
			m_script_injected = false;
			m_ui_engine = nullptr;
			m_script_panel = nullptr;
			m_sent_badges.clear ();
			return false;
		}

		// Slot 77 is CUIEngine::RunScript in the current panorama.dll; refuse anything outside the module.
		const auto engine = reinterpret_cast<std::uintptr_t> (m_ui_engine);
		const auto vtable = memory::safe_read<std::uintptr_t> (engine);
		const auto function = vtable ? memory::safe_read<std::uintptr_t> (*vtable + 77 * sizeof (std::uintptr_t)) : std::nullopt;
		const auto panorama_begin = addresses::modules::panorama;
		const auto panorama_end = panorama_begin + memory::get_module_size (panorama_begin);
		if (!function || *function < panorama_begin || *function >= panorama_end)
			return false;

		m_ui_engine->run_script (m_script_panel, script.c_str ());
		return true;
	}

} // namespace features::misc
