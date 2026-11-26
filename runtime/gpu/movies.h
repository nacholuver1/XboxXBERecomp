#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Upscaled movies: movies\ai\<name>.mp4 beside the exe, made from the game's own <name>.bik with the same frames,
// is drawn in place of each Bink frame. The Bink file still plays (its timing and audio); only the picture changes.
// No file, a different frame count or a decode error: the original frames are drawn.
namespace gpu::movies {

// after BinkOpen: the movie's file and its Bink struct (Width +0, Height +4, Frames +8, FrameNum +0xC)
void Opened(uint32_t bink, const char* path);
// before BinkClose
void Closed(uint32_t bink);

struct Frame {
    uint32_t width = 0, height = 0;
    uint32_t number = 0;  // 1-based, as Bink counts
    std::vector<uint8_t> bgrx;  // rows packed, top down
};

// render thread: is a guest texture of this size the playing movie's frame? Then out gets the upscaled frame for
// the Bink frame now shown when it is decoded and newer than have (0: none yet); true when out was filled
bool IsMovie(uint32_t width, uint32_t height);
bool Take(uint32_t have, Frame& out);
// the Bink frame number now shown
uint32_t Shown();

}  // namespace gpu::movies
