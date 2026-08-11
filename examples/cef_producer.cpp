// examples/cef_producer.cpp
//
// Stands in for a CEF-style render server: one process hosts a fixed pool of
// independent "browser view" channels. Each channel is its own llshmframe
// segment -- own frames, own commands -- because each consumer's clicks and
// navigation only ever affect that one consumer's own content; there is
// nothing to share between them.
//
// Channel names: llshmframe_cef_0 .. llshmframe_cef_<slot_count - 1>.
//
//   llshmframe_cef_producer [slot_count]
//
// Restarting this process on Windows: if you kill and relaunch it while any
// llshmframe_cef_consumer is still attached to one of its channels, that
// channel will fail to (re)create with LLStatus::AlreadyExists until the
// attached consumer(s) also exit. This is not a bug -- see "Windows" in the
// top-level README -- windows_shared_memory ties a channel's name to a
// kernel object that only dies once every handle referencing it, including
// a still-running consumer's, is gone. The expectation here is that a
// producer restart takes its consumers down with it; they are not meant to
// sit and wait for it to come back.

#include <shmframe/llshmframe.h>
#include "cef_protocol.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace cef_demo;

namespace {

volatile std::sig_atomic_t g_run = 1;
void on_signal(int) { g_run = 0; }

constexpr auto kRegenPeriod   = std::chrono::milliseconds(2000); // "a few seconds"
constexpr auto kPublishPeriod = std::chrono::milliseconds(33);   // ~30 fps

enum class Color { Red, Green, Blue };

Color color_from_text(std::string_view s)
{
    if (s == "red")  return Color::Red;
    if (s == "blue") return Color::Blue;
    return Color::Green;
}

Color random_color()
{
    constexpr Color cols[] = {Color::Red, Color::Green, Color::Blue};
    return cols[std::rand() % 3];
}

// Same checkerboard shape as the original GLFW skeleton this demo grew out
// of, just generated here instead of client-side, and now parameterised by
// whichever "URL" (color) the consumer last asked for. Regenerating always
// overwrites the whole canvas -- this is what eventually erases old mouse
// marks a few seconds after they are drawn.
void regenerate(std::vector<std::uint8_t>& canvas, std::uint32_t w, std::uint32_t h, Color color)
{
    const unsigned checker_size = 16 + (std::rand() % 64);
    std::uint8_t a[4] = {0, 0, 0, 255};
    std::uint8_t b[4] = {0, 0, 0, 255};
    const int ch = color == Color::Red ? 2 : color == Color::Green ? 1 : 0; // BGRA
    a[ch] = std::uint8_t(32 + std::rand() % 128);
    b[ch] = std::uint8_t(96 + std::rand() % 64);

    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t* px = ((x / checker_size) + (y / checker_size)) % 2 == 0 ? a : b;
            std::uint8_t* dst = &canvas[(std::size_t(y) * w + x) * 4];
            dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2]; dst[3] = px[3];
        }
    }
}

// A small persistent mark, BGRA, clipped to the canvas.
void draw_mark(std::vector<std::uint8_t>& canvas, std::uint32_t w, std::uint32_t h,
               std::int32_t x, std::int32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    if (x < 0 || y < 0 || std::uint32_t(x) >= w || std::uint32_t(y) >= h) return;
    constexpr int kRadius = 4;
    for (int dy = -kRadius; dy <= kRadius; ++dy) {
        const std::int64_t py = std::int64_t(y) + dy;
        if (py < 0 || py >= h) continue;
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            const std::int64_t px = std::int64_t(x) + dx;
            if (px < 0 || px >= w) continue;
            std::uint8_t* p = &canvas[(std::size_t(py) * w + std::size_t(px)) * 4];
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
    }
}

struct Slot
{
    std::unique_ptr<LLPublisher> pub;
    std::vector<std::uint8_t>    canvas;
    std::uint32_t                width  = kDefaultWidth;
    std::uint32_t                height = kDefaultHeight;
    Color                        color  = Color::Green;
    std::chrono::steady_clock::time_point next_regen;
    bool                          dirty  = true; // force one regen before the first publish
    bool                          had_subscriber = false; // edge-detects a new consumer claiming this slot
};

} // namespace

int main(int argc, char** argv)
{
    int slot_count = kSlotCount;
    if (argc > 1) slot_count = std::atoi(argv[1]);
    if (slot_count <= 0) slot_count = 1;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    std::vector<Slot> slots(static_cast<std::size_t>(slot_count));
    std::uint64_t worst_case_bytes = 0;

    for (int i = 0; i < slot_count; ++i)
    {
        LLConfig cfg;
        cfg.name       = kChannelPrefix + std::to_string(i);
        cfg.max_width  = kMaxWidth;
        cfg.max_height = kMaxHeight;
        worst_case_bytes += segment_bytes(cfg);

        LLStatus st{};
        slots[i].pub = LLPublisher::create(cfg, &st);
        if (!slots[i].pub) {
            std::cerr << "slot " << i << " (" << cfg.name << "): " << to_string(st) << "\n";
            return 1;
        }
        slots[i].canvas.assign(std::size_t(kDefaultWidth) * kDefaultHeight * 4, 0);
        slots[i].next_regen = std::chrono::steady_clock::now();
    }

    std::cout << "cef producer: " << slot_count << " channel(s) (" << kChannelPrefix << "0.."
              << (slot_count - 1) << "), " << (worst_case_bytes / (1024 * 1024))
              << " MiB worst case at " << kMaxWidth << "x" << kMaxHeight << " each, "
              << (kDefaultWidth) << "x" << kDefaultHeight << " to start\n";

    LLCommand cmd;
    auto next_publish = std::chrono::steady_clock::now();

    while (g_run)
    {
        const auto now = std::chrono::steady_clock::now();

        for (auto& s : slots)
        {
            const bool has_sub = s.pub->has_subscriber();
            if (has_sub && !s.had_subscriber) {
                s.color = random_color();
                s.dirty = true;
            }
            s.had_subscriber = has_sub;

            while (s.pub->receive(cmd))
            {
                switch (cmd.type)
                {
                case kSetUrl:
                    s.color = color_from_text(cmd.text());
                    s.dirty = true;
                    break;

                case kMouseMove: {
                    std::int32_t x, y;
                    if (unpack_i32x2(cmd.data.data(), cmd.data.size(), x, y))
                        draw_mark(s.canvas, s.width, s.height, x, y, 255, 255, 0); // yellow
                    break;
                }
                case kMouseButton: {
                    std::int32_t x, y; std::uint8_t button, action;
                    if (unpack_mouse_button(cmd.data.data(), cmd.data.size(), x, y, button, action))
                        draw_mark(s.canvas, s.width, s.height, x, y, 255, 255, 255); // white
                    break;
                }
                case kResize: {
                    std::uint32_t w, h;
                    if (unpack_size(cmd.data.data(), cmd.data.size(), w, h) && w && h) {
                        w = std::min(w, kMaxWidth);
                        h = std::min(h, kMaxHeight);
                        if (w != s.width || h != s.height) {
                            s.width  = w;
                            s.height = h;
                            s.canvas.assign(std::size_t(w) * h * 4, 0);
                            s.dirty = true;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            if (s.dirty || now >= s.next_regen) {
                regenerate(s.canvas, s.width, s.height, s.color);
                s.next_regen = now + kRegenPeriod;
                s.dirty = false;
            }
        }

        if (now >= next_publish) {
            next_publish = now + kPublishPeriod;
            for (auto& s : slots) s.pub->publish(s.canvas.data(), s.width, s.height);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "cef producer: shutting down\n";
    return 0;
}
