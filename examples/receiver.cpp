// Example consumer.  llshmframe_receiver [poll_hz] [dump_after_frames]
#include <shmframe/llshmframe.h>
#include "protocol.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t g_run = 1;
void on_signal(int) { g_run = 0; }
} // namespace

int main(int argc, char** argv)
{
    const int  hz         = argc > 1 ? std::atoi(argv[1]) : 60;
    const long dump_after = argc > 2 ? std::atol(argv[2]) : 0;
    if (hz <= 0) return 1;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto sub = LLSubscriber::open("shmframe_demo");

    const std::filesystem::path dump_path = std::filesystem::absolute("frame_dump.bmp");
    if (dump_after > 0)
        std::cout << "receiver: will dump frame #" << dump_after << " to " << dump_path << "\n";
    else
        std::cout << "receiver: dumping DISABLED (pass a frame count as the 2nd argument)\n";

    std::vector<std::uint8_t> buf;
    LLFrameInfo info;
    LLCommand   cmd;

    const auto period = std::chrono::nanoseconds(1'000'000'000LL / hz);
    auto next = std::chrono::steady_clock::now();
    auto report = next;
    auto next_cmd = next + std::chrono::seconds(2);
    const std::uint32_t sizes[][2] = {{320,180},{640,360},{800,450},{256,144}};
    std::size_t si = 0;
    std::uint64_t ping_id = 0, ping_at = 0, since = 0;
    bool dumped = false, was_connected = false;

    while (g_run)
    {
        const auto r = sub->read_latest(buf, info);
        const auto now = std::chrono::steady_clock::now();

        if (sub->connected() != was_connected) {
            was_connected = sub->connected();
            std::cout << "receiver: " << (was_connected ? "connected" : "disconnected")
                      << " (session " << std::hex << sub->session_id() << std::dec
                      << ", #" << sub->sessions() << ")\n";
        }

        if (r == LLReadResult::Ok) {
            ++since;
            if (!dumped && dump_after > 0 &&
                sub->frames_received() >= std::uint64_t(dump_after)) {
                dumped = true;
                const bool ok = write_bmp_bgra(
                    dump_path.string(), buf.data(), info.width, info.height, info.stride);
                std::cout << "receiver: " << (ok ? "wrote " : "FAILED to write ")
                          << dump_path << " (frame " << info.frame_id << ", "
                          << info.width << "x" << info.height << ")\n";
            }
        }

        while (sub->receive(cmd)) {
            if (cmd.type == proto::kPong && cmd.reply_to == ping_id && ping_at) {
                std::cout << "receiver: <- kPong, round trip "
                          << (now_ns() - ping_at) / 1000.0 << " us\n";
                ping_at = 0;
            } else if (cmd.type == proto::kSizeAck) {
                std::cout << "receiver: <- kSizeAck (reply_to " << cmd.reply_to
                          << "): " << cmd.text() << "\n";
            } else if (cmd.type == proto::kLog) {
                std::cout << "receiver: <- log: " << cmd.text() << "\n";
            }
        }

        if (now >= next_cmd && sub->connected()) {
            next_cmd = now + std::chrono::seconds(2);
            std::uint8_t p[8];
            const auto& want = sizes[si++ % 4];
            std::uint64_t id = 0;
            if (sub->send(proto::kSetSize, p, proto::pack_size(p, want[0], want[1]), 0, &id))
                std::cout << "receiver: -> kSetSize " << want[0] << "x" << want[1]
                          << " (id " << id << ")\n";
            ping_at = now_ns();
            sub->send(proto::kPing, nullptr, 0, 0, &ping_id);
        }

        if (now - report >= std::chrono::seconds(1)) {
            std::cout << "receiver: " << since << " fps (total " << sub->frames_received()
                      << ", dropped " << sub->frames_dropped()
                      << ", sessions " << sub->sessions() << ")\n";
            since = 0; report = now;
        }

        next += period;
        if (next > now) std::this_thread::sleep_until(next); else next = now;
    }

    std::cout << "receiver: done. " << sub->frames_received() << " frames, "
              << sub->frames_dropped() << " dropped\n";
    return 0;
}
