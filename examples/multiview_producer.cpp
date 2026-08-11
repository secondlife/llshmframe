/**
 *
 * @file multiview_producer.cpp
 * @brief Multiview demo producer: hosts view channels created on demand via a control channel, torn down again when idle
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// examples/multiview_producer.cpp
//
// One process hosts a pool of up to slot_count independent "view" channels
// -- a CEF embedder juggling several browser views is the motivating case,
// but nothing here is CEF-specific. Each channel is its own llshmframe
// segment -- own frames, own commands -- because each consumer's input only
// ever affects that one consumer's own content; there is nothing to share
// between them.
//
// Channels are NOT created up front: a real per-view "instance" (a live
// CEF renderer, in the motivating case; a checkerboard generator here) is
// too heavy to run slot_count of them regardless of whether anyone is
// watching. Instead, a small always-on control channel (kControlChannelName)
// lets a consumer request a channel; this process creates one on demand,
// hands back its index, and destroys it again once nobody has been attached
// for a while (see kIdleGracePeriod) -- so the steady-state cost is
// proportional to how many viewers are actually connected, not to
// slot_count.
//
// Channel names: llshmframe_multiview_0 .. llshmframe_multiview_<slot_count - 1>.
//
//   llshmframe_multiview_producer [slot_count]
//
// Restarting this process on Windows: if you kill and relaunch it while any
// llshmframe_multiview_consumer is still attached to one of its channels,
// that channel will fail to (re)create with LLStatus::AlreadyExists until
// the attached consumer(s) also exit. This is not a bug -- see "Windows" in
// the top-level README -- windows_shared_memory ties a channel's name to a
// kernel object that only dies once every handle referencing it, including
// a still-running consumer's, is gone. The expectation here is that a
// producer restart takes its consumers down with it; they are not meant to
// sit and wait for it to come back.

#include <shmframe/llshmframe.h>
#include "multiview_protocol.h"

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

using namespace multiview_demo;

namespace {

volatile std::sig_atomic_t g_run = 1;
void on_signal(int) { g_run = 0; }

constexpr auto kRegenPeriod   = std::chrono::milliseconds(2000); // "a few seconds"
constexpr auto kPublishPeriod = std::chrono::milliseconds(33);   // ~30 fps

// How long a slot may sit with nobody attached before its instance is torn
// down and the index freed for reuse. Deliberately longer, and a separate
// concern, from LLPublisher::command_owner_stale()'s ~2s window: that one
// is "the previous owner almost certainly crashed," this one is "nobody
// wants this right now" -- a slot whose owner crashed is reclaimed
// immediately (see the main loop) rather than waiting out this grace period.
constexpr auto kIdleGracePeriod = std::chrono::seconds(5);

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
    std::unique_ptr<LLPublisher> pub; // null <=> this index is free
    std::vector<std::uint8_t>    canvas;
    std::uint32_t                width  = kDefaultWidth;
    std::uint32_t                height = kDefaultHeight;
    Color                        color  = Color::Green;
    std::chrono::steady_clock::time_point next_regen;
    bool                          dirty  = true; // force one regen before the first publish
    bool                          had_subscriber = false; // edge-detects a new consumer claiming this slot

    // Seeded when the slot is allocated and refreshed every tick a
    // subscriber is attached; drives kIdleGracePeriod teardown. Deliberately
    // NOT edge-based (unlike had_subscriber above) -- a slot that is
    // allocated but never actually attached to (the requesting consumer
    // crashed, or gave up after a reply timeout) has no true->false edge to
    // time from, but does have an allocation time to time from.
    std::chrono::steady_clock::time_point last_active;
};

// Stands in for spinning up whatever this slot's real instance is (a live
// CEF renderer, in the motivating case); here it's just creating the
// segment and resetting the canvas. Leaves s untouched on failure.
bool allocate_slot(Slot& s, int index, LLConfig cfg, std::chrono::steady_clock::time_point now)
{
    cfg.name = kChannelPrefix + std::to_string(index);

    LLStatus st{};
    auto pub = LLPublisher::create(cfg, &st);
    if (!pub) {
        std::cerr << "slot " << index << " (" << cfg.name << "): " << to_string(st) << "\n";
        return false;
    }

    s.pub            = std::move(pub);
    s.canvas.assign(std::size_t(kDefaultWidth) * kDefaultHeight * 4, 0);
    s.width           = kDefaultWidth;
    s.height          = kDefaultHeight;
    s.color           = Color::Green;
    s.next_regen      = now;
    s.dirty           = true;
    s.had_subscriber  = false;
    s.last_active     = now;
    return true;
}

// Stands in for tearing down the real instance. Releases the canvas memory
// too (not just clears it) -- an idle slot should not be holding onto
// anything, which is the entire point of allocating on demand.
void free_slot(Slot& s)
{
    s = Slot{};
}

} // namespace

int main(int argc, char** argv)
{
    int slot_count = kSlotCount;
    if (argc > 1) slot_count = std::atoi(argv[1]);
    if (slot_count <= 0) slot_count = 1;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    LLConfig view_cfg; // template for whichever index gets allocated on demand
    view_cfg.max_width  = kMaxWidth;
    view_cfg.max_height = kMaxHeight;
    const std::uint64_t worst_case_bytes = segment_bytes(view_cfg) * std::uint64_t(slot_count);

    std::vector<Slot> slots(static_cast<std::size_t>(slot_count)); // all start unallocated (pub == nullptr)

    LLConfig control_cfg;
    control_cfg.name       = kControlChannelName;
    control_cfg.max_width  = 1; // never publishes a frame, only exchanges commands
    control_cfg.max_height = 1;

    LLStatus st{};
    auto control = LLPublisher::create(control_cfg, &st);
    if (!control) {
        std::cerr << "control channel (" << control_cfg.name << "): " << to_string(st) << "\n";
        return 1;
    }

    std::cout << "multiview producer: control channel ready, up to " << slot_count
              << " concurrent view(s) (" << kChannelPrefix << "0.." << (slot_count - 1) << "), "
              << (worst_case_bytes / (1024 * 1024)) << " MiB ceiling if all " << slot_count
              << " were active at once at " << kMaxWidth << "x" << kMaxHeight << " each -- "
              << "0 committed until requested\n";

    LLCommand cmd;
    auto next_publish = std::chrono::steady_clock::now();

    while (g_run)
    {
        const auto now = std::chrono::steady_clock::now();

        // Service slot requests first so a freshly-allocated slot gets a
        // chance to regen/publish within this same tick.
        while (control->receive(cmd))
        {
            if (cmd.type != kRequestSlot) continue;

            int free_index = -1;
            for (int i = 0; i < slot_count; ++i)
                if (!slots[std::size_t(i)].pub) { free_index = i; break; }

            if (free_index < 0 || !allocate_slot(slots[std::size_t(free_index)], free_index, view_cfg, now))
            {
                control->send(kSlotUnavailable, nullptr, 0, cmd.id);
                continue;
            }

            // Reply only now that the segment demonstrably exists: the
            // producer is single-threaded, so this command's own release
            // store (below, inside send()) is ordered after every write
            // allocate_slot() just made, including the new segment's own
            // "release the magic last" store -- the requesting consumer's
            // acquire-load of this reply therefore guarantees it will see
            // a fully-initialised header once it opens that segment by name.
            std::uint8_t payload[4];
            pack_u32(payload, std::uint32_t(free_index));
            control->send(kSlotAssigned, payload, 4, cmd.id);
        }

        for (auto& s : slots)
        {
            if (!s.pub) continue;

            const bool has_sub = s.pub->has_subscriber();

            if (has_sub && s.pub->command_owner_stale())
            {
                // Almost certainly a crashed consumer, not a merely-idle
                // one: reclaim now rather than waiting out the softer idle
                // grace period below.
                free_slot(s);
                continue;
            }

            if (has_sub)
            {
                if (!s.had_subscriber) {
                    s.color = random_color();
                    s.dirty = true;
                }
                s.last_active = now;
            }
            else if (now - s.last_active >= kIdleGracePeriod)
            {
                free_slot(s);
                continue;
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
            for (auto& s : slots) if (s.pub) s.pub->publish(s.canvas.data(), s.width, s.height);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "multiview producer: shutting down\n";
    return 0;
}
