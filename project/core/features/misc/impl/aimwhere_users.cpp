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

		constexpr std::uint64_t k_steam_id_base { 76561197960265728ull };

		/// Steam ID + name for each human in the server; bots carry no Steam ID.
		struct player_row {
			std::uint64_t steam_id {};
			std::string name {};
		};

		[[nodiscard]] std::vector<player_row> match_players () {
			std::vector<player_row> out {};
			for (const auto& player : systems::g_entities.get_by_type (systems::entities::type::player)) {
				if (!player.ptr)
					continue;

				const auto id = memory::safe_read<std::uint64_t> (
					player.ptr + SCHEMA ("CBasePlayerController", "m_steamID"_hash)).value_or (0);
				if (id < k_steam_id_base)
					continue;

				const auto name_ptr = memory::safe_read<std::uintptr_t> (
					player.ptr + SCHEMA ("CCSPlayerController", "m_sSanitizedPlayerName"_hash)).value_or (0);
				out.push_back ({ id, name_ptr ? memory::read_string (name_ptr, 127) : std::string {} });
			}
			return out;
		}

		// Sent ahead of every update and a no-op when the manager already exists. The HUD's script context can
		// be reset while the HUD panel itself stays, which wiped a once-injected manager and left every later
		// update guarded into doing nothing -- the badge showed, then stopped.
		static constexpr const char* k_badge_script = R"PANORAMA(
(function () {

	if (typeof SAimwhere !== "undefined") return;

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
		if (!m_ready) {
			m_local_id = steam::user::get_steam_id ();
			if (m_local_id < k_steam_id_base)
				return;

			m_ready = true;
		}

		const auto now = clock::now ();
		if (now >= m_next_scan) {
			m_next_scan = now + std::chrono::milliseconds (500);
			scan_names (now);
		}

		update_scoreboard ();
	}

	void aimwhere_users::scan_names (clock::time_point now) {
		// Passive: read the names the game already hands us and note which ones wear a marker. No send.
		std::scoped_lock lock (m_mutex);
		for (const auto& row : match_players ()) {
			if (row.steam_id == m_local_id) {
				// Self-test: has the marker we set survived the server's sanitiser on our own name? Only flip
				// hidden -> visible, and only after a few scans, so a name that has not propagated yet or a
				// transient empty read never forces the visible marker on when the hidden one would do.
				if (g_aimwhere_marker_visible || row.name.empty ())
					continue;

				if (row.name.find (k_aimwhere_marker_hidden) != std::string::npos) {
					m_self_unmarked = 0;
				}
				else if (++m_self_unmarked >= 3) {
					g_aimwhere_marker_visible = true;
					logging::console::print (xs ("[aimwhere] hidden marker stripped by the game; using the visible one\n"));
				}
				continue;
			}

			if (aimwhere_name_marked (row.name)) {
				if (!m_users.contains (row.steam_id))
					logging::console::print (xs ("[aimwhere] user detected: {}\n"), row.steam_id);
				m_users [row.steam_id] = now;
			}
		}

		// Someone whose name lost the marker (reset, reconnect, left) stops counting after a few seconds.
		std::erase_if (m_users, [&] (const auto& e) { return now - e.second > std::chrono::seconds (6); });
	}

	void aimwhere_users::on_level_change () {
		m_script_injected = false;
		m_scoreboard_open = false;
		m_scoreboard_frames = 0;
		m_sent_badges.clear ();
		m_ui_engine = nullptr;
		m_script_panel = nullptr;

		std::scoped_lock lock (m_mutex);
		m_users.clear ();
	}

	void aimwhere_users::shutdown () {
		m_sent_badges.clear ();
		std::scoped_lock lock (m_mutex);
		m_users.clear ();
	}

	bool aimwhere_users::is_user (std::uint64_t steam_id) const {
		if (!m_ready || !steam_id)
			return false;

		if (steam_id == m_local_id)
			return true;

		std::scoped_lock lock (m_mutex);
		return m_users.contains (steam_id);
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

		for (const auto& row : match_players ()) {
			if (is_user (row.steam_id))
				add (row.steam_id);
		}
		users_json += "]";

		const auto& accent = settings::g_gui.accent.value;
		const auto accent_hex = std::format ("#{:02X}{:02X}{:02X}", static_cast<int> (accent.r), static_cast<int> (accent.g), static_cast<int> (accent.b));
		const auto payload = users_json + accent_hex;

		// Re-sent every second or so even when nothing changed: a row that was rebuilt while open has lost
		// its badge, and the script only creates what is missing.
		if (payload == m_sent_badges && m_scoreboard_frames % 32 != 0)
			return;

		// The definition rides along every time (it returns at once when the manager exists), so a reset script
		// context is rebuilt on the next send instead of silently swallowing every update after it.
		const auto script = std::string (k_badge_script) + std::format (
			R"(if(typeof(SAimwhere)!=='undefined'){{SAimwhere.update({},"{}");}})", users_json, accent_hex);
		if (!run_script (script))
			return;

		if (payload != m_sent_badges)
			logging::console::print (xs ("[aimwhere] scoreboard badges sent: {}\n"), users_json);
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
