// In-game ImGui overlay (Windows). Hooks IDXGISwapChain::Present of the game's own swap chain (Direct3D 11 or 12, found at run time),
// draws the kmod menu on top of the frame and routes input to it while it is open. Insert shows / hides the menu.
#pragma once
#include <string>

// An experiment (used while testing native mods), not a shipped feature: compiled only with -DKMOD_OVERLAY=ON.

#ifdef KMOD_OVERLAY
void Overlay_Start();                               // call once the engine is up; runs its own thread until the swap chain's Present is hooked
bool Overlay_Visible();                             // the menu is open
void Overlay_SetSnapshot(const std::string& text);  // mods list (see kmod.overlay_snapshot in commands.lua), from the game thread
std::string Overlay_Status();                       // one line for the log / console
// Panels registered by native mods (KModApi::overlay_add_panel): draw(user) runs on the render thread inside the ImGui frame while the menu is open.
int Overlay_AddPanel(void (*draw)(void*), void* user);   // panel id (> 0) or -1
void* Overlay_Context();                            // the loader's ImGuiContext*, null before the first frame
void Overlay_Allocators(void** alloc, void** free_fn, void** user);
const char* Overlay_ImGuiVersion();
#else
inline void Overlay_Start() {}
inline bool Overlay_Visible() { return false; }
inline void Overlay_SetSnapshot(const std::string&) {}
inline std::string Overlay_Status() { return "not compiled in"; }
inline int Overlay_AddPanel(void (*)(void*), void*) { return -1; }
inline void* Overlay_Context() { return nullptr; }
inline void Overlay_Allocators(void**, void**, void**) {}
inline const char* Overlay_ImGuiVersion() { return ""; }
#endif
