#include <string.h>
#include "Net/TarGzExtractor.h"

#include <cstdint>
#include <vector>

int main()
{
    // FHCRC advances the deflate offset into the trailer. The input length
    // calculation must reject this before passing an underflowed size to miniz.
    std::vector<uint8_t> malformed(18, 0);
    malformed[0] = 0x1F;
    malformed[1] = 0x8B;
    malformed[2] = 8;
    malformed[3] = 0x02;
    malformed[12] = 2;
    malformed[13] = 8;

    std::vector<uint8_t> decompressed;
    return TarGz::GzipDecompress(malformed, decompressed) ? 1 : 0;
}
