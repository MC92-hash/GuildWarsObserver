#pragma once

// GIF writer for the replay's Clip export: one looping animated GIF from a run of RGB frames.
//
// One global palette for the whole clip (median cut over a sample of every frame), a light ordered
// dither that stays put from frame to frame, and frame differencing: a pixel whose palette index did
// not change since the previous frame is written as the transparent index and every frame is cut to
// the rectangle that changed. A still camera then costs almost nothing per frame, which is what
// keeps a clip under Discord's 10 MB upload limit.

#include <atomic>
#include <cstdint>
#include <vector>

namespace ClipGif
{
    // width * height RGB triplets, top row first.
    using FrameRGB = std::vector<uint8_t>;

    struct EncodeSettings
    {
        int   width  = 0;
        int   height = 0;
        int   fps    = 20;
        float dither = 0.35f;   // 0 = off, 1 = strong
    };

    // Encodes every frame into `out`. `progress` (optional) goes 0..1. A set `cancel` stops the
    // encoder, which then returns false with `out` empty.
    bool Encode(const std::vector<FrameRGB>& frames, const EncodeSettings& settings,
                std::vector<uint8_t>& out,
                std::atomic<float>* progress = nullptr,
                const std::atomic<bool>* cancel = nullptr);

    // Area-average downscale of an RGB image.
    FrameRGB Downscale(const FrameRGB& src, int srcW, int srcH, int dstW, int dstH);

    // Delay of frame `index` in 1/100 s. GIF delays are whole centiseconds, so rates that do not
    // divide 100 alternate (30 fps = 3, 4, 3, 3, 4, 3 ...) and keep the exact rate on average.
    int FrameDelayCs(int index, int fps);
}
