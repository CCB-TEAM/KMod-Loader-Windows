// GIF -> a folder of PNG frames (for animated card art): composes the frames (disposal, transparency), shrinks them, keeps at most maxFrames.
#pragma once
#include <string>
#include <vector>

struct GifFrames
{
    int width = 0, height = 0;        // size of the written PNG frames
    std::vector<int> delaysMs;        // per written frame (merged when frames were dropped)
};

// Writes <outDir>/f001.png, f002.png ... (outDir must exist). Returns false with err set when the file is not a usable GIF.
// crop: nullptr, or { x, y, side } in GIF pixels: only that square is kept (parts outside the picture stay transparent)
bool GifDecodeToPngs(const std::string& path, const std::string& outDir, int maxSide, int maxFrames, const int* crop, GifFrames& out, std::string& err);
