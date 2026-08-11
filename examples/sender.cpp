/**
 *
 * @file sender.cpp
 * @brief Example producer: publishes a generated pattern at a configurable size and rate
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

// Example producer.  llshmframe_sender [width] [height] [fps]
#include <shmframe/llshmframe.h>
#include "protocol.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t g_run = 1;
void on_signal(int) { g_run = 0; }

void render(std::uint8_t* dst, std::uint32_t w, std::uint32_t h, std::uint64_t frame)
{
    const auto sweep = std::uint8_t(frame & 0xFF);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = dst + std::size_t(y) * w * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = row + std::size_t(x) * 4;
            p[0] = std::uint8_t(x * 255u / w);
            p[1] = std::uint8_t(y * 255u / h);
            p[2] = sweep;
            p[3] = 0xFF;
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    auto w   = std::uint32_t(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 640);
    auto h   = std::uint32_t(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 360);
    int  fps = argc > 3 ? std::atoi(argv[3]) : 60;
    if (fps <= 0) fps = 60;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    LLConfig cfg;
    cfg.name       = "shmframe_demo";
    cfg.max_width  = 1280;
    cfg.max_height = 720;

    LLStatus st{};
    auto pub = LLPublisher::create(cfg, &st);
    if (!pub) {
        std::cerr << "publisher failed: " << to_string(st) << "\n";
        return 1;
    }

    std::cout << "sender: " << w << "x" << h << " @ " << fps << " fps, segment "
              << (segment_bytes(cfg) / (1024 * 1024)) << " MiB, session "
              << std::hex << pub->session_id() << std::dec << "\n";

    const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
    auto next = std::chrono::steady_clock::now();
    auto report = next;
    std::uint64_t since = 0;
    LLCommand cmd;

    while (g_run)
    {
        while (pub->receive(cmd))
        {
            switch (cmd.type) {
            case proto::kPing:
                pub->send(proto::kPong, nullptr, 0, cmd.id);
                break;
            case proto::kSetSize: {
                std::uint32_t nw = 0, nh = 0;
                char note[96];
                if (proto::unpack_size(cmd.data.data(), cmd.data.size(), nw, nh) &&
                    std::uint64_t(nw) * nh * 4 <= cfg.max_payload() && nw && nh) {
                    w = nw; h = nh;
                    int n = std::snprintf(note, sizeof(note), "resized to %ux%u", w, h);
                    pub->send(proto::kSizeAck, note, std::uint32_t(n), cmd.id);
                    std::cout << "sender: <- kSetSize " << w << "x" << h << "\n";
                } else {
                    int n = std::snprintf(note, sizeof(note), "rejected (max %ux%u)",
                                          cfg.max_width, cfg.max_height);
                    pub->send(proto::kLog, note, std::uint32_t(n), cmd.id);
                }
                break;
            }
            default:
                std::cout << "sender: <- opcode " << cmd.type << "\n";
                break;
            }
        }

        if (std::uint8_t* dst = pub->begin_write(w, h)) {
            render(dst, w, h, pub->frames_published());
            pub->commit();
            ++since;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - report >= std::chrono::seconds(1)) {
            std::cout << "sender: " << since << " fps (total " << pub->frames_published()
                      << ", cmds in " << pub->commands_received()
                      << "/out " << pub->commands_sent() << ")\n";
            since = 0; report = now;
        }

        next += period;
        if (next > now) std::this_thread::sleep_until(next); else next = now;
    }

    std::cout << "sender: stopped after " << pub->frames_published() << " frames\n";
    return 0;
}
