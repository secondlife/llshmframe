// Application-level protocol for the multiview demo: one console producer
// hosting N independent "view" channels, each fed by its own GLFW/OpenGL
// consumer window. The library moves opaque bytes and knows nothing about
// any of this -- it is ordinary application code. A CEF-style embedder,
// where each channel is one browser view, is the motivating case, but
// nothing here is CEF-specific.
#pragma once
#include <cstddef>
#include <cstdint>

namespace multiview_demo
{
    // Shared by the producer and every consumer so neither can drift out of
    // sync with the other about channel naming or default/maximum geometry.
    inline constexpr int           kSlotCount     = 32;
    inline constexpr char          kChannelPrefix[] = "llshmframe_multiview_";
    inline constexpr std::uint32_t kDefaultWidth  = 960;
    inline constexpr std::uint32_t kDefaultHeight = 540;
    inline constexpr std::uint32_t kMaxWidth      = 1920;
    inline constexpr std::uint32_t kMaxHeight     = 1080;

    // Always-on, cheap (1x1 frame geometry -- it never publishes a frame,
    // only exchanges commands) channel a consumer uses to ask the producer
    // for one of the real per-view channels above, which the producer only
    // creates once actually requested. See multiview_producer.cpp.
    inline constexpr char kControlChannelName[] = "llshmframe_multiview_control";

    enum Opcode : std::uint32_t
    {
        // consumer -> producer, per-view channel
        kSetUrl      = 1, // text payload: "red" | "green" | "blue"
        kMouseMove   = 2, // data = {int32 x, int32 y}, canvas-space, little-endian
        kMouseButton = 3, // data = {int32 x, int32 y, uint8 button, uint8 action}
        kResize      = 4, // data = {uint32 width, uint32 height}

        // consumer -> producer, control channel only
        kRequestSlot     = 5, // empty payload

        // producer -> consumer, control channel only; reply_to = request id
        kSlotAssigned    = 6, // data = {uint32 slot index}
        kSlotUnavailable = 7, // empty payload -- no free slot right now
    };

    inline std::uint32_t pack_i32x2(std::uint8_t* d, std::int32_t x, std::int32_t y)
    {
        auto put = [&](int off, std::int32_t v) {
            d[off + 0] = std::uint8_t(v);       d[off + 1] = std::uint8_t(v >> 8);
            d[off + 2] = std::uint8_t(v >> 16);  d[off + 3] = std::uint8_t(v >> 24);
        };
        put(0, x); put(4, y);
        return 8;
    }

    inline bool unpack_i32x2(const std::uint8_t* d, std::size_t n,
                              std::int32_t& x, std::int32_t& y)
    {
        if (n < 8) return false;
        auto get = [&](int off) {
            return std::int32_t(std::uint32_t(d[off]) | (std::uint32_t(d[off + 1]) << 8) |
                                (std::uint32_t(d[off + 2]) << 16) | (std::uint32_t(d[off + 3]) << 24));
        };
        x = get(0); y = get(4);
        return true;
    }

    inline std::uint32_t pack_mouse_button(std::uint8_t* d, std::int32_t x, std::int32_t y,
                                           std::uint8_t button, std::uint8_t action)
    {
        const std::uint32_t n = pack_i32x2(d, x, y);
        d[n + 0] = button;
        d[n + 1] = action;
        return n + 2;
    }

    inline bool unpack_mouse_button(const std::uint8_t* d, std::size_t n,
                                    std::int32_t& x, std::int32_t& y,
                                    std::uint8_t& button, std::uint8_t& action)
    {
        if (n < 10 || !unpack_i32x2(d, n, x, y)) return false;
        button = d[8]; action = d[9];
        return true;
    }

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

    inline std::uint32_t pack_u32(std::uint8_t* d, std::uint32_t v)
    {
        d[0]=std::uint8_t(v); d[1]=std::uint8_t(v>>8); d[2]=std::uint8_t(v>>16); d[3]=std::uint8_t(v>>24);
        return 4;
    }

    inline bool unpack_u32(const std::uint8_t* d, std::size_t n, std::uint32_t& v)
    {
        if (n < 4) return false;
        v = std::uint32_t(d[0]) | (std::uint32_t(d[1])<<8) | (std::uint32_t(d[2])<<16) | (std::uint32_t(d[3])<<24);
        return true;
    }
}
