#pragma once

/// Runtime unload. The menu button calls request( ); the work happens on a thread of its own, because the
/// click lands inside the present hook and a hook cannot remove itself while it is still executing.
namespace unload {

	/// Starts the teardown and returns immediately. Safe to call more than once: only the first call acts.
	void request( );

	/// True from the moment request( ) is accepted. Detours that create hooks lazily check this so they do
	/// not re-arm one behind the teardown's back.
	[[nodiscard]] bool requested( );

} // namespace unload
