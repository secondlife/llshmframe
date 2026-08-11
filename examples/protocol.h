// Application-level opcodes. The library moves opaque bytes and knows nothing
// about these -- define whatever your integration needs.
#pragma once
#include <cstddef>
#include <cstdint>

namespace proto
{
    enum Opcode : std::uint32_t
    {
        kPing    = 1,
        kPong    = 2,
        kSetSize = 3,   // data = {uint32 w, uint32 h}, little-endian
        kSizeAck = 4,
        kLog     = 5,
    };

    inline std::uint32_t pack_size(std::uint8_t* d, std::uint32_t w, std::uint32_t h)
    {
        d[0]=std::uint8_t(w); d[1]=std::uint8_t(w>>8); d[2]=std::uint8_t(w>>16); d[3]=std::uint8_t(w>>24);
        d[4]=std::uint8_t(h); d[5]=std::uint8_t(h>>8); d[6]=std::uint8_t(h>>16); d[7]=std::uint8_t(h>>24);
        return 8;
    }
    inline bool unpack_size(const std::uint8_t* d, std::size_t n,
                            std::uint32_t& w, std::uint32_t& h)
    {
        if (n < 8) return false;
        w = std::uint32_t(d[0]) | (std::uint32_t(d[1])<<8) | (std::uint32_t(d[2])<<16) | (std::uint32_t(d[3])<<24);
        h = std::uint32_t(d[4]) | (std::uint32_t(d[5])<<8) | (std::uint32_t(d[6])<<16) | (std::uint32_t(d[7])<<24);
        return true;
    }
}
