#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

namespace features::changer {

	void music_kits::on_frame_stage_notify( )
	{
		const auto local = systems::g_local.get( );

		// The controller, not the pawn: the anthem plays at round end, when the local player is as likely
		// to be dead as alive.
		if ( !local.controller )
		{
			return;
		}

		const auto services_offset = SCHEMA( "CCSPlayerController", "m_pInventoryServices"_hash );
		const auto music_offset = SCHEMA( "CCSPlayerController_InventoryServices", "m_unMusicID"_hash );

		// A schema miss resolves to 0, and writing through 0 would overwrite the object's vtable.
		if ( !services_offset || !music_offset )
		{
			return;
		}

		const auto services = memory::read<std::uintptr_t>( local.controller + services_offset );
		if ( !services )
		{
			return;
		}

		if ( this->m_tracked_services != services )
		{
			// A new controller (map change, reconnect) carries its own original; the old one is gone.
			this->m_tracked_services = services;
			this->m_overridden = false;
		}

		const auto& settings = settings::g_changer;
		const auto wanted = static_cast< std::uint16_t >( std::clamp( settings.music_kit_id.value, 0, 0xFFFF ) );
		const auto enabled = settings.music_kit.value && wanted != 0 && g_econ_item_system.find_music_kit( wanted );
		const auto current = memory::read<std::uint16_t>( services + music_offset );

		if ( enabled )
		{
			if ( !this->m_overridden )
			{
				this->m_original = current;
				this->m_overridden = true;
			}

			// Rewritten every frame it differs: the server re-sends the real value on every full update.
			if ( current != wanted )
			{
				memory::write<std::uint16_t>( services + music_offset, wanted );
			}

			return;
		}

		if ( this->m_overridden )
		{
			memory::write<std::uint16_t>( services + music_offset, this->m_original );
			this->m_overridden = false;
		}
	}

} // namespace features::changer
