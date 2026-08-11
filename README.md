# llshmframe

Real-time frame and command transport over shared memory, for exchanging
pixel buffers between processes.

- **Frames** — lock-free triple buffer with per-slot seqlocks. Latest-wins: a
  slow consumer skips frames instead of stalling the producer. Zero-copy on
  the producer side.
- **Commands** — two lock-free SPSC rings, one per direction. Ordered,
  lossless, with back-pressure reported rather than silently dropped.
- **No mutex on the data path.** A peer that crashes, hangs, or sits in a
  debugger cannot wedge the survivor.
- **Boost is an implementation detail.** It is linked `PRIVATE`; consumers
  need neither Boost headers nor Boost libraries.
- **Runtime sizing.** Geometry is recorded in the segment header, so the two
  processes do not need matching compile-time constants.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Options: `LLSHMFRAME_SHARED` (default OFF), `LLSHMFRAME_BUILD_EXAMPLES`,
`LLSHMFRAME_BUILD_TESTS`.

## Use

```cmake
find_package(llshmframe REQUIRED)
target_link_libraries(my_app PRIVATE llshmframe::llshmframe)
```

Producer:

```cpp
LLConfig cfg;
cfg.name = "my_channel";
cfg.max_width = 1280; cfg.max_height = 720;

LLStatus st;
auto pub = LLPublisher::create(cfg, &st);
if (!pub) return log(to_string(st));

// zero-copy: render straight into the slot
if (std::uint8_t* dst = pub->begin_write(w, h)) {
    render_into(dst, w, h);
    pub->commit();
}
// or copy from a buffer you do not own (CEF OnPaint, etc.)
pub->publish(cef_buffer, w, h, cef_stride);
```

Consumer:

```cpp
auto sub = LLSubscriber::open("my_channel");   // segment need not exist yet

std::vector<std::uint8_t> buf;
LLFrameInfo info;

if (sub->read_latest(buf, info) == LLReadResult::Ok)
    upload_texture(buf.data(), info.width, info.height, info.stride);
```

Commands, either direction:

```cpp
std::uint64_t id;
sub->send(kSetSize, payload, 8, /*reply_to*/ 0, &id);

LLCommand cmd;
while (pub->receive(cmd))
    if (cmd.type == kSetSize) pub->send(kSizeAck, nullptr, 0, cmd.id);
```

## Behaviour worth knowing

**Producer restarts are handled.** `LLSubscriber` re-attaches automatically.
This matters more than it sounds: removing a shared memory segment only
unlinks the *name*, so an existing mapping keeps the old orphaned segment
alive while the new producer publishes into a different one. Without session
tracking the consumer goes silent forever with no error. Each segment carries
a session id; the subscriber notices, re-opens by name, and resyncs.
`sessions()` counts re-attachments.

*(Windows only: this specific recovery — restarting the producer while a
subscriber stays attached throughout — does not apply. See "Windows" below.)*

**A second live publisher under the same name is refused, not allowed to
steal it.** `LLPublisher::create()` only reclaims an existing same-named
segment when it looks abandoned: an unparseable/foreign header, a
clean-shutdown marker, or a heartbeat older than `LLConfig::stale_producer_ms`
(default 2000 ms). Otherwise it returns `LLStatus::AlreadyExists` and leaves
the live segment alone. `commit()` refreshes the heartbeat automatically;
call `heartbeat()` directly if your loop can go quiet for longer than a
peer's `stale_producer_ms` without publishing. `LLSubscriber::producer_responsive()`
exposes the same signal to a consumer, independent of frame cadence, so an
idle-but-alive producer can be told apart from one that has actually gone
away.

**One publisher, one subscriber for commands.** Frames tolerate several
independent readers, but the command channel is SPSC in each direction: two
subscribers pushing/popping the same ring concurrently would corrupt its
head/tail bookkeeping. This is enforced, not just documented — the first
`LLSubscriber` to attach to a given publisher session claims the command
channel; `send()`/`receive()` on any other subscriber attached to that same
session return `false` until the owner detaches, at which point the next
newly-opened subscriber can claim it. Check `owns_command_channel()` if you
need to know which one you are. Give each consumer its own channel if you
need several to use commands at once.

**`send()` can fail.** A full ring returns `false` rather than blocking or
dropping the oldest entry, because only the caller knows whether a command
should be retried, coalesced, or abandoned. Resize is latest-wins; mouse-down
is not. Watch `commands_dropped()`.

**`begin_write()` can be cancelled.** If you bail out of a render after
calling `begin_write()` — an exception, an early return — call
`cancel_write()` before doing anything else with the publisher. Leaving a
write pending blocks every future `begin_write()` until it is either
committed or cancelled.

**Rate limiting is available, and is non-blocking.**

```cpp
pub->set_max_publish_hz(30.0);   // 0 = unlimited (default)
sub->set_max_read_hz(30.0);
```

Both are gates, not sleeps: `begin_write()` returns `nullptr` and
`read_latest()` returns `LLReadResult::Throttled` when the interval has not
elapsed. **They cap work, not CPU** — a loop with no sleep of its own will
spin at 100% collecting `Throttled` results. Keep your `sleep_until`; use the
gates to enforce the ceiling regardless of how often the loop happens to run.

Two things the read gate deliberately does not affect: reconnection still runs
(so a throttled consumer still recovers from a producer restart), and the
command channel stays at full rate, so input never waits on the frame cap.

**Throttling the consumer is safe for frames, but not for commands.** Frames
are latest-wins, so a slow reader just skips them — visible in
`frames_dropped()`, with no effect on the producer. Commands are lossless and
finite, so if the peer sends faster than you drain, the ring fills and its
`send()` starts returning `false`. Drain commands every loop iteration even
when you skip the frame read, or size `command_slots` for the worst-case
burst.

**Latency is set by your poll rate.** The ring itself is sub-microsecond; the
round trip you measure will be roughly one poll interval. Drain more often
than you render if input latency matters, or add an OS wakeup primitive.

**Geometry is bounded.** `max_width`/`max_height` are capped (32768 each) so
that stride and total-payload arithmetic can never wrap a 32- or 64-bit
integer, regardless of `bytes_per_pixel`. `LLPublisher::create()` (and
`segment_bytes()`) return `LLStatus::InvalidConfig` / `0` for anything past
that rather than silently truncating.

**ThreadSanitizer will flag the payload copy.** Every seqlock races by the
letter of the C++ memory model — the reader copies bytes the writer may be
touching and validates afterwards. It is sound on real hardware and is how
kernel seqlocks work, but TSan cannot model it. Suppress `llsubscriber.cpp`
if you run under TSan.

**Windows.** `src/llsegment.cpp` uses `boost::interprocess::windows_shared_memory`
there — a native kernel section object with no filesystem/registry name to
leak and no `unlink()` to call: it is destroyed automatically once the last
handle and mapped region referencing it are gone. `llsegment.cpp` is the only
file that touches Boost.

This has a real consequence for producer-restart recovery (above): the name
IS the kernel object's identity, so `LLPublisher::create()` under a name a
subscriber still has mapped does not get a fresh object — it gets
`LLStatus::AlreadyExists`, because the old section is still alive. Recovering
from a producer restart on Windows therefore requires *every* subscriber
still attached to that name to also drop its old mapping (destroy and
recreate its `LLSubscriber`) before or after the producer restarts, not just
the producer. It only takes one straggler: if any single subscriber out of
however many are attached is still holding the mapping, `create()` keeps
returning `AlreadyExists` no matter how many times the producer retries. On
POSIX, `shared_memory_object::remove()` detaches the name from the old
object regardless of who still has it mapped, so the producer alone
restarting is sufficient and every existing subscriber recovers on its own.
The same asymmetry applies to heartbeat-based reclaiming of an abandoned
segment: on Windows it only ever triggers once every process holding the old
section has actually exited, since a real crash — unlike an in-process leak
— closes the handle for you.

**If your application needs the producer to be freely restartable (crash
recovery, rolling deploys) while consumers stay up and simply wait, this is
not something to work around by polling or retrying `create()` — on Windows
it will not resolve until the last consumer lets go.** The `examples/multiview_*`
demo takes the position that this is acceptable: if the producer dies,
consumers are expected to notice (`connected()` goes false, or
`producer_responsive()` does) and exit or restart rather than wait
indefinitely. A design that avoids this altogether (an always-recreatable
"which segment is current" indirection in front of the actual data segment)
is possible but is a real architectural change, not a quick patch — out of
scope unless a future application requirement actually needs it.
