// shmframe/llshmframe.h
//
// Real-time frame + command transport over shared memory.
//
//   LLPublisher  -- owns the segment, publishes frames, exchanges commands.
//   LLSubscriber -- attaches, reads the newest frame, exchanges commands.
//
// Frames use a lock-free triple buffer with per-slot seqlocks: latest-wins,
// so a slow consumer skips frames rather than stalling the producer. Commands
// use two lock-free SPSC rings (one per direction): ordered and lossless, with
// back-pressure surfaced to the caller instead of silent drops.
//
// Neither side can block the other. There is no mutex anywhere on the data
// path, so a peer that crashes, hangs, or is stopped in a debugger cannot
// wedge the survivor.
//
// This header deliberately exposes no Boost and no layout details, so it is
// safe to include from application code that knows nothing about either.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && defined(LLSHMFRAME_SHARED)
#  if defined(LLSHMFRAME_BUILDING)
#    define LLSHMFRAME_API __declspec(dllexport)
#  else
#    define LLSHMFRAME_API __declspec(dllimport)
#  endif
#else
#  define LLSHMFRAME_API
#endif

// ---------------------------------------------------------------- status

enum class LLStatus
{
    Ok,
    InvalidConfig,    // geometry or ring sizing out of range
    AlreadyExists,    // another publisher holds this name and looks alive
    VersionMismatch,  // built against an incompatible layout
    MappingFailed,    // OS refused the mapping (size? permissions?)
    Internal
};

LLSHMFRAME_API const char* to_string(LLStatus s);

// ---------------------------------------------------------------- config

// Sizing is resolved at runtime and recorded in the segment, so the two
// processes do NOT need to be compiled with matching constants -- the
// subscriber reads the geometry from the header it attaches to.
struct LLConfig
{
    std::string   name              = "shmframe";
    std::uint32_t max_width         = 1920;
    std::uint32_t max_height        = 1080;
    std::uint32_t bytes_per_pixel   = 4;  // BGRA
    std::uint32_t slot_count        = 3;  // 3 is the useful minimum
    std::uint32_t command_slots     = 64; // power of two
    std::uint32_t max_command_bytes = 512;

    // How long a same-named segment may go without a heartbeat before
    // LLPublisher::create() will treat it as abandoned (crashed without a
    // clean shutdown) and reclaim it instead of returning AlreadyExists.
    std::uint32_t stale_producer_ms = 2000;

    std::uint64_t max_payload() const
    {
        return static_cast<std::uint64_t>(max_width) * max_height * bytes_per_pixel;
    }
};

// Total bytes the segment will occupy. Useful for sanity-checking a
// config before committing to it.
LLSHMFRAME_API std::uint64_t segment_bytes(const LLConfig& cfg);

// ----------------------------------------------------------------- data

struct LLFrameInfo
{
    std::uint64_t frame_id      = 0;
    std::uint64_t timestamp_ns  = 0;
    std::uint32_t width         = 0;
    std::uint32_t height        = 0;
    std::uint32_t stride        = 0;
    std::uint32_t payload_bytes = 0;
};

struct LLCommand
{
    std::uint64_t             id           = 0; // monotonic per sender
    std::uint64_t             reply_to     = 0; // id this answers, or 0
    std::uint64_t             timestamp_ns = 0;
    std::uint32_t             type         = 0; // your opcode
    std::vector<std::uint8_t> data;

    std::string_view text() const
    {
        return std::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size());
    }
};

enum class LLReadResult
{
    Ok,           // a new frame was copied out
    NoNewFrame,   // nothing published since the last successful read
    Contended,      // producer lapped us repeatedly; retry shortly
    Throttled,      // rate limit not yet elapsed; no segment access made
    BufferTooSmall, // your destination cannot hold the frame
    Disconnected    // detached, or the producer said goodbye
};

// ------------------------------------------------------------- publisher

class LLSHMFRAME_API LLPublisher
{
public:
    // Creates a fresh segment. If a same-named segment already exists,
    // it is reclaimed only when it looks abandoned (invalid header, a
    // clean-shutdown marker, or a heartbeat older than
    // cfg.stale_producer_ms) -- otherwise this fails with AlreadyExists
    // rather than silently stealing the name out from under a live
    // publisher. Returns nullptr on failure with *status set.
    static std::unique_ptr<LLPublisher> create(const LLConfig& cfg,
                                                LLStatus* status = nullptr);

    ~LLPublisher();
    LLPublisher(const LLPublisher&)            = delete;
    LLPublisher& operator=(const LLPublisher&) = delete;

    // --- frames -------------------------------------------------------
    // Zero-copy: render straight into the returned pointer, then commit().
    // Returns nullptr if the geometry exceeds the configured maximum or a
    // previous begin_write() was not committed or cancelled.
    std::uint8_t* begin_write(std::uint32_t width, std::uint32_t height);
    void          commit(std::uint64_t timestamp_ns = 0); // 0 = now

    // Abandons a begin_write() without publishing it, e.g. after a render
    // error. Safe to call when there is nothing pending.
    void cancel_write() noexcept;

    // Optional publish rate cap. 0 (default) means unlimited. This is a
    // NON-BLOCKING gate: begin_write()/publish() return early instead of
    // sleeping, so your loop still needs its own sleep to yield the CPU.
    void   set_max_publish_hz(double hz);
    double max_publish_hz() const;
    bool   publish_due() const;   // would the gate let you through now?
    std::uint64_t frames_throttled() const;

    // Copying convenience. src_stride of 0 means tightly packed.
    bool publish(const void*   src,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t src_stride = 0);

    // Signals liveness without publishing a frame, e.g. while paused.
    // commit() already does this implicitly; call directly only if your
    // loop can go quiet for longer than a peer's stale_producer_ms.
    void heartbeat();

    // --- commands -----------------------------------------------------
    // Returns false if oversized or the outbound ring is full. Never
    // blocks and never overwrites unread commands.
    bool send(std::uint32_t  type,
              const void*    data     = nullptr,
              std::uint32_t  size     = 0,
              std::uint64_t  reply_to = 0,
              std::uint64_t* out_id   = nullptr);

    bool send_text(std::uint32_t  type,
                   std::string_view text,
                   std::uint64_t  reply_to = 0,
                   std::uint64_t* out_id   = nullptr);

    bool receive(LLCommand& out); // false when the queue is empty

    // --- info ---------------------------------------------------------
    std::uint64_t frames_published() const;
    std::uint64_t session_id() const;
    std::uint64_t commands_sent() const;
    std::uint64_t commands_received() const;
    std::uint64_t commands_dropped() const; // outbound ring was full
    std::uint32_t outbound_pending() const;
    const LLConfig& config() const;

    // True once some LLSubscriber has claimed the command channel for the
    // current session, false again once it cleanly detaches (see
    // LLSubscriber::owns_command_channel()). Since the command channel is
    // single-subscriber, this doubles as "is anyone attached" -- useful for
    // reacting to a consumer joining without threading your own bookkeeping
    // through the command stream.
    //
    // One case this does NOT surface as a false-then-true edge: a crashed
    // subscriber's claim is handed directly to whichever new one steals it
    // (see owns_command_channel()), so has_subscriber() can stay
    // continuously true across that handoff. Detecting "a new consumer
    // joined" this way will miss that particular transition; it never
    // misses a clean detach followed by a fresh attach.
    bool has_subscriber() const;

private:
    LLPublisher();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// ------------------------------------------------------------ subscriber

class LLSHMFRAME_API LLSubscriber
{
public:
    // Never fails: the segment does not have to exist yet. Poll
    // connected(), or just call read_latest() and watch for Disconnected.
    static std::unique_ptr<LLSubscriber> open(std::string name);

    ~LLSubscriber();
    LLSubscriber(const LLSubscriber&)            = delete;
    LLSubscriber& operator=(const LLSubscriber&) = delete;

    // Attaches, and transparently re-attaches when the producer is
    // restarted. Called for you by read_latest(); call it directly if you
    // only use the command channel. Returns connected().
    bool poll();
    bool connected() const;

    // True if the producer's heartbeat is fresh, independent of whether a
    // new frame has been published recently. Complements connected(),
    // which only means "attached at some point" -- this means "still
    // there right now" by the same stale_producer_ms-style timeout used
    // for reconnection (see set_stale_timeout_ms()).
    bool producer_responsive() const;

    // --- frames -------------------------------------------------------
    LLReadResult read_latest(std::vector<std::uint8_t>& dst, LLFrameInfo& info);
    LLReadResult read_latest(void* dst, std::size_t capacity, LLFrameInfo& info);

    // --- commands -----------------------------------------------------
    // The command channel is single-subscriber: the first LLSubscriber to
    // attach to a given publisher session claims it, and send()/receive()
    // on any other subscriber attached to that same session return false
    // until the owner detaches -- or, if the owner crashed without
    // detaching, until its heartbeat goes quiet for a couple of seconds and
    // a new attacher steals the channel instead of being refused forever.
    // A stolen-from subscriber finds out on its next send()/receive()/poll()
    // (including the one inside read_latest()), at which point
    // owns_command_channel() also flips to false. See owns_command_channel().
    bool send(std::uint32_t  type,
              const void*    data     = nullptr,
              std::uint32_t  size     = 0,
              std::uint64_t  reply_to = 0,
              std::uint64_t* out_id   = nullptr);

    bool send_text(std::uint32_t  type,
                   std::string_view text,
                   std::uint64_t  reply_to = 0,
                   std::uint64_t* out_id   = nullptr);

    bool receive(LLCommand& out);
    bool owns_command_channel() const;

    // --- info ---------------------------------------------------------
    // Geometry of the producer we are attached to (null when detached).
    const LLConfig* remote_config() const;

    std::uint64_t session_id() const;
    std::uint64_t sessions() const;   // increments on every re-attach
    std::uint64_t frames_received() const;
    std::uint64_t frames_dropped() const;
    std::uint64_t commands_sent() const;
    std::uint64_t commands_received() const;
    std::uint64_t commands_dropped() const;
    bool          producer_shutdown() const;

    // Quiet period before we suspect we were orphaned. Default 500 ms.
    // Also used as the freshness window for producer_responsive().
    void set_stale_timeout_ms(std::uint32_t ms);

    // Optional frame read rate cap. 0 (default) means unlimited. Returns
    // ReadResult::Throttled without touching the segment when the gate is
    // closed. Reconnection still runs, and the command channel is NOT
    // affected -- receive() stays available at full rate.
    //
    // NON-BLOCKING: this caps work done, not CPU used. Your loop must
    // still sleep, or it will spin returning Throttled.
    void   set_max_read_hz(double hz);
    double max_read_hz() const;
    bool   read_due() const;
    std::uint64_t reads_throttled() const;

private:
    LLSubscriber();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// -------------------------------------------------------------- utility

// Debug helper: write a BGRA buffer out as a 24-bit BMP. Alpha discarded.
// Pass flip_vertically = false if the source is already bottom-up.
LLSHMFRAME_API bool write_bmp_bgra(const std::string&  path,
                                    const std::uint8_t* bgra,
                                    std::uint32_t       width,
                                    std::uint32_t       height,
                                    std::uint32_t       stride,
                                    bool                flip_vertically = true);

LLSHMFRAME_API std::uint64_t now_ns();
