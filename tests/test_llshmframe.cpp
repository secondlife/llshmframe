// Self-contained tests. No framework dependency.
#include <shmframe/llshmframe.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void test_config_and_sizing()
{
    std::printf("config/sizing\n");
    LLConfig c; c.name = "sf_test_a"; c.max_width = 64; c.max_height = 64;
    CHECK(segment_bytes(c) > 0);

    LLConfig bad = c; bad.slot_count = 2;              // below the minimum
    CHECK(segment_bytes(bad) == 0);
    bad = c; bad.command_slots = 63;                   // not a power of two
    CHECK(segment_bytes(bad) == 0);
    bad = c; bad.max_width = 0;
    CHECK(segment_bytes(bad) == 0);

    // Sizing must scale with geometry, proving it is runtime-derived.
    LLConfig big = c; big.max_width = 640; big.max_height = 480;
    CHECK(segment_bytes(big) > segment_bytes(c));
}

static void test_frame_roundtrip()
{
    std::printf("frame round trip\n");
    LLConfig c; c.name = "sf_test_b"; c.max_width = 128; c.max_height = 128;

    LLStatus st{};
    auto pub = LLPublisher::create(c, &st);
    CHECK(pub != nullptr); CHECK(st == LLStatus::Ok);
    if (!pub) return;

    auto sub = LLSubscriber::open(c.name);
    CHECK(sub->connected());
    CHECK(sub->remote_config() != nullptr);
    CHECK(sub->remote_config()->max_width == 128);

    std::vector<std::uint8_t> buf;
    LLFrameInfo info;

    CHECK(sub->read_latest(buf, info) == LLReadResult::NoNewFrame);

    std::uint8_t* dst = pub->begin_write(32, 16);
    CHECK(dst != nullptr);
    std::memset(dst, 0xAB, 32 * 16 * 4);
    pub->commit();

    CHECK(sub->read_latest(buf, info) == LLReadResult::Ok);
    CHECK(info.width == 32 && info.height == 16);
    CHECK(info.stride == 32 * 4);
    CHECK(info.payload_bytes == 32 * 16 * 4);
    CHECK(info.frame_id == 1);
    bool all = true;
    for (std::uint32_t i = 0; i < info.payload_bytes; ++i) if (buf[i] != 0xAB) all = false;
    CHECK(all);

    CHECK(sub->read_latest(buf, info) == LLReadResult::NoNewFrame);

    // Oversized geometry must be refused, not truncated.
    CHECK(pub->begin_write(4096, 4096) == nullptr);

    // Raw overload reports an inadequate destination.
    pub->publish(buf.data(), 32, 16);
    std::uint8_t tiny[4];
    CHECK(sub->read_latest(tiny, sizeof(tiny), info) == LLReadResult::BufferTooSmall);
}

static void test_cancel_write()
{
    std::printf("cancel_write\n");
    LLConfig c; c.name = "sf_test_i"; c.max_width = 32; c.max_height = 32;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);

    // Start a write, then abandon it -- must not wedge future writes and
    // must not be visible to the subscriber.
    std::uint8_t* dst = pub->begin_write(16, 16);
    CHECK(dst != nullptr);
    CHECK(pub->begin_write(16, 16) == nullptr); // still pending
    pub->cancel_write();

    std::vector<std::uint8_t> b; LLFrameInfo fi;
    CHECK(sub->read_latest(b, fi) == LLReadResult::NoNewFrame);

    dst = pub->begin_write(16, 16);
    CHECK(dst != nullptr); // no longer wedged
    std::memset(dst, 0x7A, 16 * 16 * 4);
    pub->commit();

    CHECK(sub->read_latest(b, fi) == LLReadResult::Ok);
    CHECK(fi.frame_id == 1); // the cancelled attempt never bumped frame_id
    CHECK(b[0] == 0x7A);

    pub->cancel_write(); // no-op with nothing pending; must not crash
}

static void test_overflow_rejected()
{
    std::printf("oversized geometry rejected as config, not wrapped\n");
    LLConfig c; c.name = "sf_test_j";
    c.max_width  = 1'000'000; // comfortably past the dimension cap
    c.max_height = 1080;
    CHECK(segment_bytes(c) == 0);

    LLStatus st{};
    auto pub = LLPublisher::create(c, &st);
    CHECK(pub == nullptr);
    CHECK(st == LLStatus::InvalidConfig);
}

static void test_double_publisher_guard()
{
    std::printf("double publisher guard\n");
    LLConfig c; c.name = "sf_test_k"; c.max_width = 32; c.max_height = 32;

    auto pub_a = LLPublisher::create(c);
    CHECK(pub_a != nullptr); if (!pub_a) return;

    // A second live publisher under the same name must be refused, not
    // silently allowed to steal the segment out from under the first.
    LLStatus st{};
    auto pub_b = LLPublisher::create(c, &st);
    CHECK(pub_b == nullptr);
    CHECK(st == LLStatus::AlreadyExists);

    // The first publisher is unaffected.
    CHECK(pub_a->publish(std::vector<std::uint8_t>(32 * 32 * 4, 9).data(), 32, 32));

    pub_a.reset();
    auto pub_c = LLPublisher::create(c, &st);
    CHECK(pub_c != nullptr);
    CHECK(st == LLStatus::Ok);
}

static void test_stale_producer_reclaimed()
{
    std::printf("stale (heartbeat-timed-out) producer reclaimed\n");
#if defined(_WIN32)
    std::printf("  skipped: a windows_shared_memory section stays alive as long as any "
                "handle is open, so an in-process leak cannot simulate a crash the way "
                "a real process exit (which closes handles automatically) does\n");
#else
    LLConfig c; c.name = "sf_test_l"; c.max_width = 32; c.max_height = 32;
    c.stale_producer_ms = 20;

    auto pub_a = LLPublisher::create(c);
    CHECK(pub_a != nullptr); if (!pub_a) return;
    const std::uint64_t first_session = pub_a->session_id();

    // Simulate an unclean exit: no destructor runs, so shutdown is never
    // set and the heartbeat simply stops advancing. Leaked intentionally,
    // standing in for a crashed process.
    (void)pub_a.release();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LLStatus st{};
    auto pub_b = LLPublisher::create(c, &st);
    CHECK(pub_b != nullptr);
    CHECK(st == LLStatus::Ok);
    if (pub_b) CHECK(pub_b->session_id() != first_session);
#endif
}

static void test_command_ownership()
{
    std::printf("command channel single-subscriber ownership\n");
    LLConfig c; c.name = "sf_test_m"; c.max_width = 32; c.max_height = 32;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;

    auto sub1 = LLSubscriber::open(c.name);
    CHECK(sub1->connected());
    CHECK(sub1->owns_command_channel());

    auto sub2 = LLSubscriber::open(c.name);
    CHECK(sub2->connected());
    CHECK(!sub2->owns_command_channel());

    CHECK(sub1->send_text(1, "from sub1"));
    CHECK(!sub2->send_text(1, "from sub2")); // refused, not corrupting the ring

    LLCommand in;
    CHECK(pub->receive(in));
    CHECK(in.text() == "from sub1");
    CHECK(!pub->receive(in)); // sub2's send never went anywhere

    // Once the owner detaches, a freshly-opened subscriber can claim it.
    sub1.reset();
    auto sub3 = LLSubscriber::open(c.name);
    CHECK(sub3->connected());
    CHECK(sub3->owns_command_channel());
}

static void test_command_ownership_steal_after_crash()
{
    std::printf("command channel stolen from a crashed owner\n");
    LLConfig c; c.name = "sf_test_n"; c.max_width = 32; c.max_height = 32;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;

    auto sub1 = LLSubscriber::open(c.name);
    CHECK(sub1->connected());
    CHECK(sub1->owns_command_channel());
    CHECK(sub1->send_text(1, "from sub1"));

    // Simulate a crash: no destructor runs, so the claim is never released
    // and its heartbeat simply stops advancing.
    (void)sub1.release();

    // Arriving immediately, the claim still looks fresh: refused, same as
    // the ordinary single-owner case above.
    auto sub2 = LLSubscriber::open(c.name);
    CHECK(sub2->connected());
    CHECK(!sub2->owns_command_channel());

    std::this_thread::sleep_for(std::chrono::milliseconds(2200));

    // Its heartbeat is now stale: the next attacher steals the channel
    // instead of being refused forever.
    auto sub3 = LLSubscriber::open(c.name);
    CHECK(sub3->connected());
    CHECK(sub3->owns_command_channel());

    LLCommand in;
    CHECK(pub->receive(in)); // sub1's earlier send was still queued
    CHECK(in.text() == "from sub1");
    CHECK(sub3->send_text(2, "from sub3"));
    CHECK(pub->receive(in));
    CHECK(in.text() == "from sub3");
}

static void test_clean_disconnect_slot_reuse()
{
    std::printf("clean disconnect: command channel and frames are reused\n");
    LLConfig c; c.name = "sf_test_o"; c.max_width = 32; c.max_height = 32;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    CHECK(!pub->has_subscriber());

    // Consumer A joins, reads a published frame, and exchanges a command.
    auto subA = LLSubscriber::open(c.name);
    CHECK(subA->connected());
    CHECK(subA->owns_command_channel());
    CHECK(pub->has_subscriber());
    const std::uint64_t session_before = subA->session_id();

    std::vector<std::uint8_t> canvas_a(32u * 32u * 4u, 0x11);
    CHECK(pub->publish(canvas_a.data(), 32, 32));

    std::vector<std::uint8_t> buf;
    LLFrameInfo info;
    CHECK(subA->read_latest(buf, info) == LLReadResult::Ok);
    CHECK(subA->send_text(1, "hello from A"));

    LLCommand in;
    CHECK(pub->receive(in));
    CHECK(in.text() == "hello from A");

    // A disconnects cleanly: its destructor runs and releases the claim.
    subA.reset();
    CHECK(!pub->has_subscriber());

    // The producer does not pause or reset anything just because the slot
    // is momentarily empty -- it keeps publishing regardless of who, if
    // anyone, is attached.
    std::vector<std::uint8_t> canvas_b(32u * 32u * 4u, 0x22);
    CHECK(pub->publish(canvas_b.data(), 32, 32));

    // Consumer B joins afterwards: same producer session (not a restart),
    // same slot, a fresh claim rather than a refusal.
    auto subB = LLSubscriber::open(c.name);
    CHECK(subB->connected());
    CHECK(subB->session_id() == session_before);
    CHECK(subB->owns_command_channel());
    CHECK(pub->has_subscriber()); // clean detach then fresh attach: a real false -> true edge

    // B sees the frame published while the slot was empty immediately --
    // no per-consumer setup or replay needed, since frames were never
    // gated on anyone being attached.
    CHECK(subB->read_latest(buf, info) == LLReadResult::Ok);
    CHECK(buf[0] == 0x22);

    CHECK(subB->send_text(2, "hello from B"));
    CHECK(pub->receive(in));
    CHECK(in.text() == "hello from B");
}

static void test_commands_bidirectional()
{
    std::printf("bidirectional commands\n");
    LLConfig c; c.name = "sf_test_c"; c.max_width = 32; c.max_height = 32;
    c.command_slots = 8; c.max_command_bytes = 32;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);
    CHECK(sub->connected());

    LLCommand in;
    std::uint64_t id = 0;
    CHECK(sub->send_text(42, "hello", 0, &id));
    CHECK(id == 1);
    CHECK(pub->receive(in));
    CHECK(in.type == 42);
    CHECK(in.text() == "hello");
    CHECK(in.id == 1);
    CHECK(!pub->receive(in));

    CHECK(pub->send_text(43, "world", in.id));
    CHECK(sub->receive(in));
    CHECK(in.type == 43);
    CHECK(in.text() == "world");
    CHECK(in.reply_to == 1);

    // Oversized payload rejected.
    std::vector<std::uint8_t> big(64, 7);
    CHECK(!sub->send(1, big.data(), std::uint32_t(big.size())));

    // Ring fills, reports back-pressure, and never overwrites.
    std::uint32_t accepted = 0;
    while (sub->send(9, "x", 1)) ++accepted;
    CHECK(accepted == c.command_slots);
    CHECK(sub->commands_dropped() >= 1);

    std::uint32_t drained = 0;
    while (pub->receive(in)) { CHECK(in.type == 9); ++drained; }
    CHECK(drained == accepted);
}

static void test_concurrent_no_tearing()
{
    std::printf("concurrent frames: tearing + loss\n");
    LLConfig c; c.name = "sf_test_d"; c.max_width = 320; c.max_height = 180;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);

    std::atomic<bool> stop{false};
    std::atomic<long> torn{0}, ok{0};

    std::thread prod([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::uint8_t* d = pub->begin_write(320, 180);
            if (!d) continue;
            std::memset(d, std::uint8_t((pub->frames_published() + 1) & 0xFF), 320 * 180 * 4);
            pub->commit();
        }
    });

    std::thread cons([&] {
        std::vector<std::uint8_t> b; LLFrameInfo fi;
        while (!stop.load(std::memory_order_relaxed)) {
            if (sub->read_latest(b, fi) != LLReadResult::Ok) continue;
            const std::uint8_t want = std::uint8_t(fi.frame_id & 0xFF);
            bool good = true;
            for (std::uint32_t i = 0; i < fi.payload_bytes; ++i)
                if (b[i] != want) { good = false; break; }
            (good ? ok : torn).fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true; prod.join(); cons.join();

    std::printf("  clean=%ld torn=%ld dropped=%llu\n", ok.load(), torn.load(),
                (unsigned long long)sub->frames_dropped());
    CHECK(torn.load() == 0);
    CHECK(ok.load() > 0);
}

static void test_producer_restart()
{
    std::printf("producer restart / orphan recovery\n");
    LLConfig c; c.name = "sf_test_e"; c.max_width = 64; c.max_height = 64;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);
    sub->set_stale_timeout_ms(50);

    std::vector<std::uint8_t> b; LLFrameInfo fi;
    pub->publish(std::vector<std::uint8_t>(64 * 64 * 4, 1).data(), 64, 64);
    CHECK(sub->read_latest(b, fi) == LLReadResult::Ok);
    const std::uint64_t first_session = sub->session_id();
    CHECK(sub->sessions() == 1);

#if defined(_WIN32)
    // windows_shared_memory has no name-unlink: the name IS the kernel
    // section object's identity, so as long as `sub` still holds it mapped,
    // a second Publisher::create() under the same name cannot succeed -- it
    // finds the still-alive object and reports AlreadyExists instead of
    // handing back a fresh one. See README, "Windows".
    pub.reset();
    LLStatus st{};
    auto pub2 = LLPublisher::create(c, &st);
    CHECK(pub2 == nullptr);
    CHECK(st == LLStatus::AlreadyExists);

    // Recovery requires the old subscriber to let go of the mapping first.
    sub.reset();
    pub2 = LLPublisher::create(c, &st);
    CHECK(pub2 != nullptr); if (!pub2) return;
    CHECK(pub2->session_id() != first_session);
    pub2->publish(std::vector<std::uint8_t>(64 * 64 * 4, 2).data(), 64, 64);

    sub = LLSubscriber::open(c.name);
    CHECK(sub->read_latest(b, fi) == LLReadResult::Ok && b[0] == 2);
    CHECK(sub->session_id() != first_session);
    CHECK(sub->sessions() == 1); // fresh subscriber, first attach
#else
    // Destroy and recreate: this is the case that silently wedges a naive
    // consumer, because unlink leaves the old mapping alive.
    pub.reset();
    auto pub2 = LLPublisher::create(c);
    CHECK(pub2 != nullptr); if (!pub2) return;
    CHECK(pub2->session_id() != first_session);
    pub2->publish(std::vector<std::uint8_t>(64 * 64 * 4, 2).data(), 64, 64);

    bool recovered = false;
    for (int i = 0; i < 200 && !recovered; ++i) {
        if (sub->read_latest(b, fi) == LLReadResult::Ok && b[0] == 2) recovered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(recovered);
    CHECK(sub->session_id() != first_session);
    CHECK(sub->sessions() == 2);
    // The frame id restarts at 1; the drop counter must not wrap.
    CHECK(sub->frames_dropped() < 1000);
#endif
}

static void test_subscriber_before_publisher()
{
    std::printf("subscriber started first\n");
    LLConfig c; c.name = "sf_test_f"; c.max_width = 32; c.max_height = 32;

    auto sub = LLSubscriber::open(c.name);
    CHECK(!sub->connected());
    std::vector<std::uint8_t> b; LLFrameInfo fi;
    CHECK(sub->read_latest(b, fi) == LLReadResult::Disconnected);

    sub->set_stale_timeout_ms(20);
    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    pub->publish(std::vector<std::uint8_t>(32 * 32 * 4, 5).data(), 32, 32);

    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        if (sub->read_latest(b, fi) == LLReadResult::Ok) got = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got);
}


static void test_rate_limiting()
{
    std::printf("rate limiting\n");
    LLConfig c; c.name = "sf_test_g"; c.max_width = 64; c.max_height = 64;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);

    CHECK(pub->max_publish_hz() == 0.0);   // unlimited by default
    CHECK(sub->max_read_hz() == 0.0);

    pub->set_max_publish_hz(50.0);
    sub->set_max_read_hz(25.0);
    CHECK(pub->max_publish_hz() > 49.0 && pub->max_publish_hz() < 51.0);

    std::vector<std::uint8_t> b; LLFrameInfo fi;
    long published = 0, read_ok = 0, throttled_reads = 0;

    // Hammer both loops for 1 second with no sleeping at all.
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)) {
        if (std::uint8_t* d = pub->begin_write(64, 64)) { d[0] = 1; pub->commit(); ++published; }
        const auto r = sub->read_latest(b, fi);
        if (r == LLReadResult::Ok) ++read_ok;
        else if (r == LLReadResult::Throttled) ++throttled_reads;
    }

    std::printf("  published=%ld (cap 50)  reads=%ld (cap 25)  throttled_calls=%ld\n",
                published, read_ok, throttled_reads);

    // Generous bounds: timer granularity, not a tight SLA.
    CHECK(published >= 40 && published <= 60);
    CHECK(read_ok   >= 18 && read_ok   <= 32);
    CHECK(pub->frames_throttled() > 0);
    CHECK(sub->reads_throttled() > 0);

    // The gate caps work, NOT cpu: a loop without sleep still spins hard.
    CHECK(throttled_reads > 1000);

    // Commands must stay at full rate even while frame reads are throttled.
    LLCommand in;
    CHECK(sub->send_text(77, "urgent"));
    CHECK(pub->receive(in));
    CHECK(in.text() == "urgent");

    pub->set_max_publish_hz(0.0);          // back to unlimited
    CHECK(pub->publish_due());
}

static void test_slow_consumer_command_backpressure()
{
    std::printf("slow consumer: frames vs commands\n");
    LLConfig c; c.name = "sf_test_h"; c.max_width = 64; c.max_height = 64;
    c.command_slots = 8;

    auto pub = LLPublisher::create(c);
    CHECK(pub != nullptr); if (!pub) return;
    auto sub = LLSubscriber::open(c.name);

    // Producer runs fast; consumer never drains. Frames must be fine
    // (latest-wins), commands must NOT be silently lost.
    std::vector<std::uint8_t> px(64 * 64 * 4, 3);
    long cmd_ok = 0, cmd_refused = 0;
    for (int i = 0; i < 100; ++i) {
        pub->publish(px.data(), 64, 64);
        if (pub->send(1, "c", 1)) ++cmd_ok; else ++cmd_refused;
    }

    std::printf("  frames published=100  commands accepted=%ld refused=%ld\n",
                cmd_ok, cmd_refused);

    CHECK(cmd_ok == 8);            // exactly the ring depth
    CHECK(cmd_refused == 92);      // refused, never overwritten
    CHECK(pub->commands_dropped() == 92);

    // Frames are unaffected: the newest is still readable.
    std::vector<std::uint8_t> b; LLFrameInfo fi;
    CHECK(sub->read_latest(b, fi) == LLReadResult::Ok);
    CHECK(fi.frame_id == 100);

    // Every accepted command survived intact and in order.
    LLCommand in; long drained = 0;
    while (sub->receive(in)) { CHECK(in.id == std::uint64_t(drained + 1)); ++drained; }
    CHECK(drained == 8);
}

int main()
{
    test_config_and_sizing();
    test_frame_roundtrip();
    test_cancel_write();
    test_overflow_rejected();
    test_double_publisher_guard();
    test_stale_producer_reclaimed();
    test_command_ownership();
    test_command_ownership_steal_after_crash();
    test_clean_disconnect_slot_reuse();
    test_commands_bidirectional();
    test_concurrent_no_tearing();
    test_producer_restart();
    test_subscriber_before_publisher();
    test_rate_limiting();
    test_slow_consumer_command_backpressure();

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
