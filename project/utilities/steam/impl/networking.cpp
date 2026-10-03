#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <protection/game_addresses.hpp>
#include "../steam.hpp"

namespace steam {

	namespace detail {

		inline std::uintptr_t networking_interface{};

		/// SteamNetworkingIdentity: type, payload size, then a 128-byte union whose widest scalar is a
		/// uint64. Only ever filled and read through the steam_api helpers, never by hand.
		struct alignas( 8 ) networking_identity
		{
			int type{};
			int size{};
			std::uint8_t data[ 128 ]{};
		};
		static_assert( sizeof( networking_identity ) == 136 );

		/// The leading fields of SteamNetworkingMessage_t, which is all this file reads.
		struct networking_message
		{
			void* data;
			int size;
			std::uint32_t connection;
			networking_identity peer;
		};
		static_assert( offsetof( networking_message, peer ) == 16 );

		constexpr int k_send_reliable{ 8 };
		constexpr int k_send_auto_restart_broken_session{ 32 };

		[[nodiscard]] static networking_identity make_identity( std::uint64_t steam_id )
		{
			networking_identity identity{};
			memory::call<void>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_SteamNetworkingIdentity_SetSteamID64" ), &identity, steam_id );
			return identity;
		}

	} // namespace detail

	bool networking::initialize( )
	{
		if ( !detail::networking_interface )
		{
			detail::networking_interface = memory::call<std::uintptr_t>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_SteamNetworkingMessages_SteamAPI_v002" ) );
		}

		return detail::networking_interface != 0;
	}

	bool networking::send( std::uint64_t steam_id, int channel, const void* data, std::uint32_t size )
	{
		if ( !detail::networking_interface )
		{
			return false;
		}

		const auto identity = detail::make_identity( steam_id );
		constexpr auto flags = detail::k_send_reliable | detail::k_send_auto_restart_broken_session;

		// EResult; 1 is k_EResultOK.
		return memory::call<int>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_ISteamNetworkingMessages_SendMessageToUser" ),
			detail::networking_interface, &identity, data, size, flags, channel ) == 1;
	}

	bool networking::accept( std::uint64_t steam_id )
	{
		if ( !detail::networking_interface )
		{
			return false;
		}

		const auto identity = detail::make_identity( steam_id );
		return memory::call<bool>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser" ), detail::networking_interface, &identity );
	}

	void networking::receive( int channel, std::vector<message>& out, int max )
	{
		if ( !detail::networking_interface || max <= 0 )
		{
			return;
		}

		std::array<detail::networking_message*, 32> batch{};
		const auto count = memory::call<int>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel" ),
			detail::networking_interface, channel, batch.data( ), std::min( max, static_cast< int >( batch.size( ) ) ) );

		for ( auto i = 0; i < count; ++i )
		{
			auto* msg = batch[ i ];
			if ( !msg )
			{
				continue;
			}

			message entry{};
			entry.sender = memory::call<std::uint64_t>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_SteamNetworkingIdentity_GetSteamID64" ), &msg->peer );
			if ( msg->data && msg->size > 0 )
			{
				const auto* bytes = static_cast< const std::uint8_t* >( msg->data );
				entry.data.assign( bytes, bytes + msg->size );
			}
			out.push_back( std::move( entry ) );

			memory::call<void>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_SteamNetworkingMessage_t_Release" ), msg );
		}
	}

	void networking::close_channel( std::uint64_t steam_id, int channel )
	{
		if ( !detail::networking_interface )
		{
			return;
		}

		const auto identity = detail::make_identity( steam_id );
		memory::call<bool>( MODULE_EXPORT( "steam_api64.dll:SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser" ), detail::networking_interface, &identity, channel );
	}

} // namespace steam
