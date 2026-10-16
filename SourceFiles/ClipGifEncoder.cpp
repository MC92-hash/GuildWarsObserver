#include "pch.h"
#include "ClipGifEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ClipGif
{
namespace
{
    // The palette is built and looked up on a 6-bit-per-channel grid: 262 144 cells, fine enough
    // that the grid itself never shows once the light dither is on top.
    constexpr int kGridBits  = 6;
    constexpr int kGridShift = 8 - kGridBits;
    constexpr int kGridSize  = 1 << (3 * kGridBits);

    constexpr int kPaletteColours  = 255;   // index 255 is the transparent one
    constexpr uint8_t kTransparent = 255;

    // Channel weights for colour distance: the eye is most sensitive to green, least to blue.
    constexpr int kWeightR = 2, kWeightG = 4, kWeightB = 3;

    // Frames sampled for the palette, and the pixel stride inside each.
    constexpr int kPaletteSampleFrames = 24;
    constexpr int kPaletteSampleStride = 2;

    // Ordered (Bayer 4x4) dither. It is a fixed pattern on the screen, so a pixel that does not
    // change keeps its index from frame to frame and the frame difference stays small; error
    // diffusion would shimmer and defeat it.
    constexpr int kBayer4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    constexpr float kDitherSpread = 32.f;   // at strength 1, in 8-bit units peak to peak

    inline int Cell(int r, int g, int b)
    {
        return ((r >> kGridShift) << (2 * kGridBits)) | ((g >> kGridShift) << kGridBits) | (b >> kGridShift);
    }

    inline int CellCentre(int level) { return (level << kGridShift) | (1 << (kGridShift - 1)); }

    struct Colour { uint8_t r, g, b; };

    struct HistEntry
    {
        uint8_t  c[3];      // cell centre, 8-bit
        uint32_t count;
    };

    struct Box
    {
        size_t   begin = 0, end = 0;
        uint64_t count = 0;
        int      lo[3] = {255, 255, 255};
        int      hi[3] = {0, 0, 0};

        void Fit(const std::vector<HistEntry>& e)
        {
            count = 0;
            for (int k = 0; k < 3; k++) { lo[k] = 255; hi[k] = 0; }
            for (size_t i = begin; i < end; i++)
            {
                count += e[i].count;
                for (int k = 0; k < 3; k++)
                {
                    lo[k] = std::min<int>(lo[k], e[i].c[k]);
                    hi[k] = std::max<int>(hi[k], e[i].c[k]);
                }
            }
        }
        int LongestAxis() const
        {
            const int w[3] = {kWeightR, kWeightG, kWeightB};
            int best = 0, bestLen = -1;
            for (int k = 0; k < 3; k++)
            {
                int len = (hi[k] - lo[k]) * w[k];
                if (len > bestLen) { bestLen = len; best = k; }
            }
            return best;
        }
        int WeightedRange() const
        {
            const int w[3] = {kWeightR, kWeightG, kWeightB};
            int m = 0;
            for (int k = 0; k < 3; k++) m = std::max(m, (hi[k] - lo[k]) * w[k]);
            return m;
        }
    };

    // Median cut: keep splitting the box with the most pixels times the widest colour range, at
    // the pixel-weighted median of its longest axis.
    std::vector<Colour> BuildPalette(const std::vector<FrameRGB>& frames, int w, int h)
    {
        std::vector<uint32_t> hist(kGridSize, 0);
        const size_t n = frames.size();
        const size_t frameStep = std::max<size_t>(1, n / kPaletteSampleFrames);
        for (size_t f = 0; f < n; f += frameStep)
        {
            const uint8_t* px = frames[f].data();
            for (int y = 0; y < h; y += kPaletteSampleStride)
                for (int x = 0; x < w; x += kPaletteSampleStride)
                {
                    const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 3;
                    hist[Cell(p[0], p[1], p[2])]++;
                }
        }

        std::vector<HistEntry> entries;
        for (int i = 0; i < kGridSize; i++)
        {
            if (!hist[i]) continue;
            HistEntry e;
            e.c[0] = static_cast<uint8_t>(CellCentre((i >> (2 * kGridBits)) & ((1 << kGridBits) - 1)));
            e.c[1] = static_cast<uint8_t>(CellCentre((i >> kGridBits) & ((1 << kGridBits) - 1)));
            e.c[2] = static_cast<uint8_t>(CellCentre(i & ((1 << kGridBits) - 1)));
            e.count = hist[i];
            entries.push_back(e);
        }
        if (entries.empty())
            return {Colour{0, 0, 0}};

        std::vector<Box> boxes;
        boxes.reserve(kPaletteColours);
        Box first;
        first.begin = 0;
        first.end = entries.size();
        first.Fit(entries);
        boxes.push_back(first);

        while (boxes.size() < static_cast<size_t>(kPaletteColours))
        {
            int pick = -1;
            double bestScore = 0.0;
            for (size_t i = 0; i < boxes.size(); i++)
            {
                if (boxes[i].end - boxes[i].begin < 2) continue;
                double score = static_cast<double>(boxes[i].count) * boxes[i].WeightedRange();
                if (score > bestScore) { bestScore = score; pick = static_cast<int>(i); }
            }
            if (pick < 0) break;

            Box b = boxes[pick];
            const int axis = b.LongestAxis();
            std::sort(entries.begin() + b.begin, entries.begin() + b.end,
                      [axis](const HistEntry& a, const HistEntry& c) { return a.c[axis] < c.c[axis]; });

            const uint64_t half = b.count / 2;
            uint64_t acc = 0;
            size_t split = b.end - 1;
            for (size_t i = b.begin; i < b.end - 1; i++)
            {
                acc += entries[i].count;
                if (acc >= half) { split = i + 1; break; }
            }
            split = std::clamp(split, b.begin + 1, b.end - 1);

            Box lo = b, hi = b;
            lo.end = split;
            hi.begin = split;
            lo.Fit(entries);
            hi.Fit(entries);
            boxes[pick] = lo;
            boxes.push_back(hi);
        }

        std::vector<Colour> palette;
        palette.reserve(boxes.size());
        for (const Box& b : boxes)
        {
            uint64_t s[3] = {0, 0, 0}, c = 0;
            for (size_t i = b.begin; i < b.end; i++)
            {
                for (int k = 0; k < 3; k++) s[k] += static_cast<uint64_t>(entries[i].c[k]) * entries[i].count;
                c += entries[i].count;
            }
            if (!c) continue;
            palette.push_back(Colour{static_cast<uint8_t>(s[0] / c), static_cast<uint8_t>(s[1] / c),
                                     static_cast<uint8_t>(s[2] / c)});
        }
        return palette;
    }

    // Grid cell -> nearest palette index, filled on first use.
    class PaletteLookup
    {
    public:
        explicit PaletteLookup(const std::vector<Colour>& palette)
            : m_palette(palette), m_lut(kGridSize, -1) {}

        uint8_t operator()(int r, int g, int b)
        {
            const int cell = Cell(r, g, b);
            int16_t& v = m_lut[cell];
            if (v < 0) v = static_cast<int16_t>(Nearest(cell));
            return static_cast<uint8_t>(v);
        }

    private:
        int Nearest(int cell) const
        {
            const int r = CellCentre((cell >> (2 * kGridBits)) & ((1 << kGridBits) - 1));
            const int g = CellCentre((cell >> kGridBits) & ((1 << kGridBits) - 1));
            const int b = CellCentre(cell & ((1 << kGridBits) - 1));
            int best = 0, bestD = INT32_MAX;
            for (size_t i = 0; i < m_palette.size(); i++)
            {
                const int dr = r - m_palette[i].r, dg = g - m_palette[i].g, db = b - m_palette[i].b;
                const int d = kWeightR * dr * dr + kWeightG * dg * dg + kWeightB * db * db;
                if (d < bestD) { bestD = d; best = static_cast<int>(i); }
            }
            return best;
        }

        const std::vector<Colour>& m_palette;
        std::vector<int16_t> m_lut;
    };

    // ---- byte output -------------------------------------------------------------------------

    void Put16(std::vector<uint8_t>& out, int v)
    {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }

    // LSB-first bit packer that writes the 255-byte data sub-blocks of a GIF image.
    class SubBlockBitWriter
    {
    public:
        explicit SubBlockBitWriter(std::vector<uint8_t>& out) : m_out(out) { m_block.reserve(255); }

        void Put(uint32_t code, int bits)
        {
            m_acc |= code << m_bits;
            m_bits += bits;
            while (m_bits >= 8)
            {
                PushByte(static_cast<uint8_t>(m_acc & 0xFF));
                m_acc >>= 8;
                m_bits -= 8;
            }
        }

        void Finish()
        {
            if (m_bits > 0)
                PushByte(static_cast<uint8_t>(m_acc & 0xFF));
            m_acc = 0;
            m_bits = 0;
            FlushBlock();
            m_out.push_back(0);   // block terminator
        }

    private:
        void PushByte(uint8_t b)
        {
            m_block.push_back(b);
            if (m_block.size() == 255) FlushBlock();
        }
        void FlushBlock()
        {
            if (m_block.empty()) return;
            m_out.push_back(static_cast<uint8_t>(m_block.size()));
            m_out.insert(m_out.end(), m_block.begin(), m_block.end());
            m_block.clear();
        }

        std::vector<uint8_t>& m_out;
        std::vector<uint8_t>  m_block;
        uint32_t m_acc = 0;
        int      m_bits = 0;
    };

    // GIF LZW with 8-bit pixels: clear = 256, end = 257, codes up to 12 bits, and a clear code
    // whenever the table fills. The code width grows exactly when the decoder's does: after code
    // number j since the last clear is written, the next one is wider once 258 + j reaches 2^width.
    class LzwEncoder
    {
    public:
        void Encode(const uint8_t* idx, size_t count, std::vector<uint8_t>& out)
        {
            constexpr int kMinCodeSize = 8;
            constexpr int kClear = 1 << kMinCodeSize;
            constexpr int kEnd = kClear + 1;
            constexpr int kMaxCode = 4096;

            out.push_back(kMinCodeSize);
            SubBlockBitWriter w(out);

            int codeSize = kMinCodeSize + 1;
            int next = kEnd + 1;
            ResetTable();
            w.Put(kClear, codeSize);

            auto emit = [&](int code) {
                w.Put(static_cast<uint32_t>(code), codeSize);
                if (next >= (1 << codeSize) && codeSize < 12) codeSize++;
            };

            if (count == 0)
            {
                w.Put(kEnd, codeSize);
                w.Finish();
                return;
            }

            int prefix = idx[0];
            for (size_t i = 1; i < count; i++)
            {
                const int c = idx[i];
                const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | static_cast<uint32_t>(c);
                const int found = Find(key);
                if (found >= 0)
                {
                    prefix = found;
                    continue;
                }
                emit(prefix);
                if (next < kMaxCode)
                {
                    Insert(key, next);
                    next++;
                }
                else
                {
                    w.Put(kClear, codeSize);
                    ResetTable();
                    codeSize = kMinCodeSize + 1;
                    next = kEnd + 1;
                }
                prefix = c;
            }
            emit(prefix);
            w.Put(kEnd, codeSize);
            w.Finish();
        }

    private:
        static constexpr uint32_t kTableBits = 13;
        static constexpr uint32_t kTableSize = 1u << kTableBits;   // > 2x the 3838 live entries

        struct Slot { int32_t key; int32_t code; };

        void ResetTable()
        {
            m_table.assign(kTableSize, Slot{-1, 0});
        }
        static uint32_t Hash(uint32_t key) { return (key * 2654435761u) >> (32 - kTableBits); }
        int Find(uint32_t key) const
        {
            for (uint32_t h = Hash(key);; h = (h + 1) & (kTableSize - 1))
            {
                const Slot& s = m_table[h];
                if (s.key < 0) return -1;
                if (static_cast<uint32_t>(s.key) == key) return s.code;
            }
        }
        void Insert(uint32_t key, int code)
        {
            uint32_t h = Hash(key);
            while (m_table[h].key >= 0) h = (h + 1) & (kTableSize - 1);
            m_table[h] = Slot{static_cast<int32_t>(key), code};
        }

        std::vector<Slot> m_table;
    };
}

int FrameDelayCs(int index, int fps)
{
    fps = std::max(1, fps);
    const auto at = [fps](int i) { return static_cast<int>(std::lround(i * 100.0 / fps)); };
    return std::max(2, at(index + 1) - at(index));
}

FrameRGB Downscale(const FrameRGB& src, int srcW, int srcH, int dstW, int dstH)
{
    if (dstW == srcW && dstH == srcH) return src;
    FrameRGB dst(static_cast<size_t>(dstW) * dstH * 3);
    const float sx = static_cast<float>(srcW) / dstW;
    const float sy = static_cast<float>(srcH) / dstH;
    for (int y = 0; y < dstH; y++)
    {
        const int y0 = static_cast<int>(y * sy);
        const int y1 = std::max(y0 + 1, std::min(srcH, static_cast<int>((y + 1) * sy)));
        for (int x = 0; x < dstW; x++)
        {
            const int x0 = static_cast<int>(x * sx);
            const int x1 = std::max(x0 + 1, std::min(srcW, static_cast<int>((x + 1) * sx)));
            uint32_t s[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                {
                    const uint8_t* p = &src[(static_cast<size_t>(yy) * srcW + xx) * 3];
                    s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
                    n++;
                }
            uint8_t* d = &dst[(static_cast<size_t>(y) * dstW + x) * 3];
            for (int k = 0; k < 3; k++) d[k] = static_cast<uint8_t>((s[k] + n / 2) / n);
        }
    }
    return dst;
}

bool Encode(const std::vector<FrameRGB>& frames, const EncodeSettings& s, std::vector<uint8_t>& out,
            std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    out.clear();
    const int w = s.width, h = s.height;
    if (frames.empty() || w <= 0 || h <= 0 || w > 65535 || h > 65535) return false;
    const size_t pixels = static_cast<size_t>(w) * h;
    for (const FrameRGB& f : frames)
        if (f.size() != pixels * 3) return false;

    auto cancelled = [cancel]() { return cancel && cancel->load(); };
    auto report = [progress](float p) { if (progress) progress->store(p); };

    report(0.f);
    const std::vector<Colour> palette = BuildPalette(frames, w, h);
    if (cancelled()) return false;
    report(0.08f);
    PaletteLookup lookup(palette);

    // ---- header, global colour table, loop forever ----
    const char sig[] = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    Put16(out, w);
    Put16(out, h);
    out.push_back(0xF7);   // global table, 8-bit colour resolution, 256 entries
    out.push_back(0);      // background index
    out.push_back(0);      // pixel aspect
    for (int i = 0; i < 256; i++)
    {
        const Colour c = i < static_cast<int>(palette.size()) ? palette[i] : Colour{0, 0, 0};
        out.push_back(c.r);
        out.push_back(c.g);
        out.push_back(c.b);
    }
    const uint8_t netscape[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                                0x03, 0x01, 0x00, 0x00, 0x00};
    out.insert(out.end(), netscape, netscape + sizeof(netscape));

    const float spread = std::clamp(s.dither, 0.f, 1.f) * kDitherSpread;
    float bayer[4][4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            bayer[y][x] = ((kBayer4[y][x] + 0.5f) / 16.f - 0.5f) * spread;

    std::vector<uint8_t> idx(pixels), prev(pixels), sub;
    LzwEncoder lzw;

    for (size_t f = 0; f < frames.size(); f++)
    {
        if (cancelled()) { out.clear(); return false; }

        const uint8_t* px = frames[f].data();
        for (int y = 0; y < h; y++)
        {
            const float* brow = bayer[y & 3];
            for (int x = 0; x < w; x++)
            {
                const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 3;
                const float d = brow[x & 3];
                const int r = std::clamp(static_cast<int>(p[0] + d), 0, 255);
                const int g = std::clamp(static_cast<int>(p[1] + d), 0, 255);
                const int b = std::clamp(static_cast<int>(p[2] + d), 0, 255);
                idx[static_cast<size_t>(y) * w + x] = lookup(r, g, b);
            }
        }

        // The rectangle that changed since the previous frame (the whole frame for the first).
        int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
        bool any = true;
        if (f > 0)
        {
            x0 = w; y0 = h; x1 = -1; y1 = -1;
            for (int y = 0; y < h; y++)
            {
                const uint8_t* a = &idx[static_cast<size_t>(y) * w];
                const uint8_t* b = &prev[static_cast<size_t>(y) * w];
                if (std::memcmp(a, b, w) == 0) continue;
                y0 = std::min(y0, y);
                y1 = y;
                for (int x = 0; x < w; x++)
                    if (a[x] != b[x]) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
            }
            any = x1 >= 0;
            if (!any) { x0 = y0 = x1 = y1 = 0; }
        }
        const int rw = x1 - x0 + 1, rh = y1 - y0 + 1;

        sub.resize(static_cast<size_t>(rw) * rh);
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++)
            {
                const size_t i = static_cast<size_t>(y0 + y) * w + (x0 + x);
                const bool same = f > 0 && (!any || idx[i] == prev[i]);
                sub[static_cast<size_t>(y) * rw + x] = same ? kTransparent : idx[i];
            }

        // Graphic control: leave the frame in place, transparent index 255 after the first frame.
        out.push_back(0x21);
        out.push_back(0xF9);
        out.push_back(0x04);
        out.push_back(static_cast<uint8_t>((1 << 2) | (f > 0 ? 1 : 0)));
        Put16(out, FrameDelayCs(static_cast<int>(f), s.fps));
        out.push_back(kTransparent);
        out.push_back(0x00);

        out.push_back(0x2C);
        Put16(out, x0);
        Put16(out, y0);
        Put16(out, rw);
        Put16(out, rh);
        out.push_back(0x00);   // no local table, not interlaced

        lzw.Encode(sub.data(), sub.size(), out);

        std::swap(prev, idx);
        report(0.08f + 0.92f * static_cast<float>(f + 1) / frames.size());
    }

    out.push_back(0x3B);
    report(1.f);
    return true;
}
}
