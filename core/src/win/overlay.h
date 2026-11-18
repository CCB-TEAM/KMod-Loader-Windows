// In-game ImGui overlay (Windows). Hooks IDXGISwapChain::Present of the game's own swap chain (Direct3D 11 or 12, found at run time),
// draws the kmod menu on top of the frame and routes input to it while it is open. Insert shows / hides the menu.
#pragma once
#include <string>

void Overlay_Start();                               // call once the engine is up; runs its own thread until the swap chain's Present is hooked
bool Overlay_Visible();                             // the menu is open
void Overlay_SetSnapshot(const std::string& text);  // mods list (see kmod.overlay_snapshot in commands.lua), from the game thread
std::string Overlay_Status();                       // one line for the log / console
