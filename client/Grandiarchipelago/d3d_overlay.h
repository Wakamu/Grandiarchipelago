#pragma once

namespace grandia_ap {

// Hooks IDXGISwapChain::Present (D3D11). Draws toast text via GDI → R8G8B8A8
// texture → CopySubresourceRegion (D2D cannot target the game's RGBA backbuffer).
bool InstallD3dOverlay();
void ShutdownD3dOverlay();
bool IsD3dOverlayInstalled();

// UTF-8 message appended as a new overlay line (max 8). Each line expires on its own timer
// (duration_ms; 0 = until cleared). Oldest dropped if the queue is full.
// rgb is 0xRRGGBB (e.g. Archipelago Plum 0xDDA0DD).
void ShowD3dOverlayToast(const char* utf8_message, unsigned duration_ms = 5000,
                         unsigned rgb = 0xFFE528);
void ClearD3dOverlayToast();

}  // namespace grandia_ap
