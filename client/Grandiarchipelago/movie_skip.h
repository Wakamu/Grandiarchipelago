#pragma once

namespace grandia_ap {

// Makes MP4 cinematics skippable via Select (gamepad Back) or Backspace.
// Uses the game's own Movie_Play force-skip path (input mask 0x90F).
bool InstallMovieSkipHook();
void RemoveMovieSkipHook();
bool IsMovieSkipHookInstalled();
bool IsMoviePlaying();

// Call from the watcher loop.
void PollMovieSkipHotkey();

}  // namespace grandia_ap
