/**
 *
 * @file minimal.cpp
 * @brief Smallest possible llshmframe producer+consumer example, both endpoints in a single process
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

// examples/minimal.cpp
//
// The smallest useful program: one process, no threads, no signal handling,
// no loop. Creates a channel, publishes a frame, reads it back, and exchanges
// one command each way.
//
// Both endpoints normally live in different processes -- putting them side by
// side here just makes the whole API readable in one pass. Nothing about the
// library requires them to share a process.
//
//   cmake --build build --target llshmframe_minimal
//   ./build/llshmframe_minimal

#include <shmframe/llshmframe.h>

#include <cstdio>
#include <vector>

// Your own opcodes. The library moves opaque bytes and never inspects these.
enum : std::uint32_t { kHello = 1, kHelloAck = 2 };

int main()
{
    // ---- 1. producer creates the channel --------------------------------
    LLConfig cfg;
    cfg.name       = "minimal_demo";
    cfg.max_width  = 256;   // segment is sized from these, at runtime
    cfg.max_height = 256;

    LLStatus st{};
    auto pub = LLPublisher::create(cfg, &st);
    if (!pub)
    {
        std::printf("publisher failed: %s\n", to_string(st));
        return 1;
    }
    std::printf("channel '%s' is %llu bytes\n", cfg.name.c_str(),
                (unsigned long long)segment_bytes(cfg));

    // ---- 2. consumer attaches -------------------------------------------
    // open() never fails; the segment does not have to exist yet.
    auto sub = LLSubscriber::open(cfg.name);
    std::printf("connected: %s\n", sub->connected() ? "yes" : "no");

    // ---- 3. publish a frame ---------------------------------------------
    // Zero-copy: write straight into the slot, then commit.
    const std::uint32_t w = 128, h = 64;
    if (std::uint8_t* dst = pub->begin_write(w, h))
    {
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
            {
                std::uint8_t* px = dst + (std::size_t(y) * w + x) * 4;
                px[0] = std::uint8_t(x * 2); // B
                px[1] = std::uint8_t(y * 4); // G
                px[2] = 0x40;                // R
                px[3] = 0xFF;                // A
            }
        pub->commit();
    }

    // If the pixels already live somewhere else (a CEF OnPaint buffer, say),
    // use publish() instead and pass the source stride:
    //     pub->publish(cef_buffer, w, h, cef_stride);

    // ---- 4. read it back -------------------------------------------------
    std::vector<std::uint8_t> pixels;   // reused across calls; grows once
    LLFrameInfo                info;

    if (sub->read_latest(pixels, info) == LLReadResult::Ok)
        std::printf("got frame %llu: %ux%u, stride %u, %u bytes\n",
                    (unsigned long long)info.frame_id,
                    info.width, info.height, info.stride, info.payload_bytes);

    // Nothing new yet -- latest-wins means no queue to drain.
    if (sub->read_latest(pixels, info) == LLReadResult::NoNewFrame)
        std::printf("second read: no new frame (expected)\n");

    // ---- 5. commands, both directions ------------------------------------
    std::uint64_t id = 0;
    sub->send_text(kHello, "hello from the consumer", 0, &id);

    LLCommand cmd;
    while (pub->receive(cmd))
    {
        std::printf("producer received #%llu: '%.*s'\n",
                    (unsigned long long)cmd.id,
                    int(cmd.text().size()), cmd.text().data());

        // reply_to correlates the answer with the request.
        pub->send_text(kHelloAck, "ack from the producer", cmd.id);
    }

    while (sub->receive(cmd))
        std::printf("consumer received reply to #%llu: '%.*s'\n",
                    (unsigned long long)cmd.reply_to,
                    int(cmd.text().size()), cmd.text().data());

    // ---- 6. optional: save a frame to look at ----------------------------
    if (write_bmp_bgra("minimal.bmp", pixels.data(),
                       info.width, info.height, info.stride))
        std::printf("wrote minimal.bmp\n");

    // Destroying the LLPublisher signals shutdown and unlinks the segment.
    return 0;
}
