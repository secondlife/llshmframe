/**
 *
 * @file llsubscriber.cpp
 * @brief LLSubscriber implementation: attaches to a shared-memory segment, reads frames, and exchanges commands
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

#include "llinternal.h"
#include "llsegment.h"

#include <cstring>

using namespace detail;

struct LLSubscriber::Impl
{
    std::string               name;
    LLConfig                  remote;
    LLLayout                  L;
    std::unique_ptr<LLSegment> seg;
    LLHeader*                 hdr  = nullptr;
    void*                     base = nullptr;

    LLRing tx, rx;

    std::uint64_t session       = 0;
    std::uint64_t sessions      = 0;
    std::uint64_t last_frame    = 0;
    std::uint64_t received      = 0;
    std::uint64_t dropped       = 0;
    std::uint64_t next_cmd      = 1;
    std::uint64_t sent = 0, recvd = 0, cmd_dropped = 0;
    bool          peer_shutdown = false;
    bool          owns_commands = false;
    std::uint64_t owner_gen     = 0; // our claim token, valid while owns_commands

    std::uint64_t stale_ns     = 500ull * 1000 * 1000;
    std::uint64_t retry_ns     = 250ull * 1000 * 1000;
    std::uint64_t last_progress = 0;
    std::uint64_t last_attempt  = 0;

    std::uint64_t min_read_ns  = 0;  // 0 = unlimited
    std::uint64_t last_read_ns = 0;
    std::uint64_t throttled    = 0;

    void detach()
    {
        hdr = nullptr; base = nullptr;
        tx = LLRing{}; rx = LLRing{};
        owns_commands = false;
        owner_gen     = 0;
    }

    // True while we still hold the claim we last took out on
    // command_owner_gen; false if another subscriber has since stolen it
    // (see try_connect()). Updates owns_commands as a side effect so
    // callers can just check that afterwards.
    bool still_owns_commands()
    {
        if (!owns_commands) return false;
        if (hdr->command_owner_gen.load(std::memory_order_acquire) != owner_gen)
        {
            owns_commands = false; // stolen while we were stalled
            return false;
        }
        hdr->command_owner_heartbeat_ns.store(now_ns(), std::memory_order_release);
        return true;
    }

    // Validate a candidate mapping and adopt it if it is a new session.
    bool try_connect()
    {
        auto candidate = LLSegment::open(name);
        if (!candidate) return false;

        void* b = candidate->address();
        LLLayout cl;
        LLHeader* h = validate_header(b, candidate->size(), cl);
        if (!h) return false;

        // Same producer we are already reading: keep the current mapping
        // rather than churning it.
        if (hdr && h->session_id == session) return true;

        LLConfig c;
        c.name              = name;
        c.max_width         = h->max_width;
        c.max_height        = h->max_height;
        c.bytes_per_pixel   = h->bytes_per_pixel;
        c.slot_count        = h->slot_count;
        c.command_slots     = h->command_slots;
        c.max_command_bytes = h->max_command_bytes;

        seg     = std::move(candidate);
        base    = b;
        hdr     = h;
        remote  = c;
        L       = cl;
        session = h->session_id;
        ++sessions;
        last_frame    = 0;
        next_cmd      = 1;
        peer_shutdown = false;

        // The command channel is single-subscriber: the first subscriber to
        // reach a fresh session claims it, everyone else is refused rather
        // than silently corrupting shared head/tail bookkeeping -- unless
        // the current claimant's heartbeat has gone stale (crashed without
        // releasing it), in which case we steal it instead of being refused
        // forever. A lost CAS just means someone else claimed or stole it
        // between our load and our attempt; reassess rather than give up.
        owns_commands = false;
        std::uint64_t existing_gen = h->command_owner_gen.load(std::memory_order_acquire);
        for (;;)
        {
            if (existing_gen != 0)
            {
                const std::uint64_t age = now_ns() -
                    h->command_owner_heartbeat_ns.load(std::memory_order_acquire);
                if (age < kCommandOwnerStaleNs) break; // genuinely held; give up
            }

            std::uint64_t claim = now_ns();
            if (claim == 0) claim = 1; // 0 is reserved for "unclaimed"
            if (h->command_owner_gen.compare_exchange_weak(
                    existing_gen, claim, std::memory_order_acq_rel))
            {
                owns_commands = true;
                owner_gen     = claim;
                h->command_owner_heartbeat_ns.store(now_ns(), std::memory_order_release);
                break;
            }
        }

        tx = LLRing{base, L.upstream_offset,   &L, c.command_slots, c.max_command_bytes};
        rx = LLRing{base, L.downstream_offset, &L, c.command_slots, c.max_command_bytes};
        return true;
    }
};

LLSubscriber::LLSubscriber() : d_(new Impl) {}

LLSubscriber::~LLSubscriber()
{
    if (d_->hdr && d_->owns_commands)
    {
        // CAS rather than an unconditional store: if we were stolen from
        // while stalled, someone else's claim is now in there and this
        // clean-looking exit must not clobber it.
        std::uint64_t expected = d_->owner_gen;
        d_->hdr->command_owner_gen.compare_exchange_strong(
            expected, 0, std::memory_order_release);
    }
}

std::unique_ptr<LLSubscriber> LLSubscriber::open(std::string name)
{
    std::unique_ptr<LLSubscriber> s(new LLSubscriber);
    s->d_->name          = std::move(name);
    s->d_->last_progress = now_ns();
    s->d_->try_connect();
    return s;
}

bool LLSubscriber::connected() const { return d_->hdr != nullptr; }

bool LLSubscriber::producer_responsive() const
{
    const Impl& m = *d_;
    if (!m.hdr) return false;
    const std::uint64_t age =
        now_ns() - m.hdr->last_heartbeat_ns.load(std::memory_order_acquire);
    return age < m.stale_ns;
}

bool LLSubscriber::poll()
{
    Impl& m = *d_;
    const std::uint64_t now = now_ns();

    // Refreshes our command-channel heartbeat on every poll (not just when
    // we happen to send/receive), so a consumer that only ever reads frames
    // still keeps a stale-owner steal from firing while it is genuinely
    // alive. Also catches the case where we were stolen from while stalled.
    if (m.hdr) m.still_owns_commands();

    // Healthy and recently productive: nothing to do. This is the common
    // path and costs one subtraction.
    if (m.hdr && (now - m.last_progress) < m.stale_ns) return true;

    // Quiet. Being quiet is not proof of being orphaned -- the producer may
    // simply be idle -- so check its actual heartbeat before assuming
    // anything is wrong, rather than trusting that the mapping is still
    // valid: on Windows (and similarly elsewhere) a named shared-memory
    // mapping stays open as long as we ourselves still hold it, even long
    // after the producer that created it has died without a clean shutdown
    // -- so re-opening the same name below and finding the same session_id
    // (see try_connect()'s own "keep the current mapping" fast path) proves
    // nothing about whether that producer is actually still running. Only
    // the heartbeat does, which is why this function returns
    // producer_responsive() rather than "m.hdr != nullptr" everywhere below.
    if (producer_responsive()) return true;

    // Rate-limited so this cannot become a hot loop.
    if ((now - m.last_attempt) < m.retry_ns) return false;
    m.last_attempt = now;

    m.try_connect();
    m.last_progress = now; // reset the clock either way
    return producer_responsive();
}

// ------------------------------------------------------------- frames

LLReadResult LLSubscriber::read_latest(void* dst, std::size_t capacity, LLFrameInfo& info)
{
    Impl& m = *d_;

    // poll() runs BEFORE the rate gate: throttling reads must never
    // prevent reconnection to a restarted producer.
    if (!poll()) return LLReadResult::Disconnected;

    if (m.min_read_ns)
    {
        const std::uint64_t now = now_ns();
        if (m.last_read_ns && (now - m.last_read_ns) < m.min_read_ns)
        {
            ++m.throttled;
            return LLReadResult::Throttled;
        }
        m.last_read_ns = now;
    }

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const std::uint64_t packed = m.hdr->latest.load(std::memory_order_acquire);

        if (packed == 0 || (packed >> 2) == m.last_frame)
        {
            if (m.hdr->shutdown.load(std::memory_order_acquire))
            {
                m.peer_shutdown = true;
                return LLReadResult::Disconnected;
            }
            return LLReadResult::NoNewFrame;
        }

        const std::uint32_t slot = static_cast<std::uint32_t>(packed & 0x3u);
        if (slot >= m.remote.slot_count) return LLReadResult::Contended;

        LLSlotHeader* s = slot_at(m.base, m.L, slot);

        const std::uint64_t before = s->seq.load(std::memory_order_acquire);
        if (before & 1u) continue; // writer is inside it

        LLFrameInfo fi;
        fi.frame_id      = s->frame_id;
        fi.timestamp_ns  = s->timestamp_ns;
        fi.width         = s->width;
        fi.height        = s->height;
        fi.stride        = s->stride;
        fi.payload_bytes = s->payload_bytes;

        const std::uint64_t bytes = fi.payload_bytes;
        if (bytes == 0 || bytes > m.remote.max_payload())
        {
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s->seq.load(std::memory_order_relaxed) != before) continue;
            return LLReadResult::Contended;
        }
        if (bytes > capacity)
        {
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s->seq.load(std::memory_order_relaxed) != before) continue;
            info = fi;
            return LLReadResult::BufferTooSmall;
        }

        std::memcpy(dst, slot_payload(m.base, m.L, slot), bytes);

        // The seqlock check: identical, even counters before and after
        // mean nothing overwrote us mid-copy.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->seq.load(std::memory_order_relaxed) != before) continue;

        // Guarded: a restarted producer numbers from 1 again, and an
        // unguarded subtraction would wrap to a nonsense drop count.
        if (m.last_frame != 0 && fi.frame_id > m.last_frame)
            m.dropped += fi.frame_id - m.last_frame - 1;

        m.last_frame    = fi.frame_id;
        m.last_progress = now_ns();
        ++m.received;
        info = fi;
        return LLReadResult::Ok;
    }

    return LLReadResult::Contended;
}

LLReadResult LLSubscriber::read_latest(std::vector<std::uint8_t>& dst, LLFrameInfo& info)
{
    if (!poll()) return LLReadResult::Disconnected;

    // Size once to the producer's maximum so the raw path can never come
    // back BufferTooSmall; capacity is retained across calls.
    // Cheap gate first: avoid resizing the buffer on a throttled call.
    if (d_->min_read_ns && d_->last_read_ns &&
        (now_ns() - d_->last_read_ns) < d_->min_read_ns)
    {
        ++d_->throttled;
        return LLReadResult::Throttled;
    }

    const std::uint64_t need = d_->remote.max_payload();
    if (dst.size() < need) dst.resize(static_cast<std::size_t>(need));

    return read_latest(dst.data(), dst.size(), info);
}

// ----------------------------------------------------------- commands

bool LLSubscriber::send(std::uint32_t type, const void* data, std::uint32_t size,
                        std::uint64_t reply_to, std::uint64_t* out_id)
{
    Impl& m = *d_;
    if (!m.hdr || !m.still_owns_commands()) return false;

    const std::uint64_t id = m.next_cmd;
    if (!m.tx.push(type, data, size, id, reply_to, now_ns()))
    {
        ++m.cmd_dropped;
        return false;
    }
    ++m.next_cmd;
    ++m.sent;
    if (out_id) *out_id = id;
    return true;
}

bool LLSubscriber::send_text(std::uint32_t type, std::string_view text,
                             std::uint64_t reply_to, std::uint64_t* out_id)
{
    return send(type, text.data(), static_cast<std::uint32_t>(text.size()),
                reply_to, out_id);
}

bool LLSubscriber::receive(LLCommand& out)
{
    Impl& m = *d_;
    if (!m.hdr || !m.still_owns_commands()) return false;
    if (!m.rx.pop(out)) return false;
    ++m.recvd;
    m.last_progress = now_ns(); // command traffic counts as liveness
    return true;
}

bool LLSubscriber::owns_command_channel() const { return d_->owns_commands; }

// --------------------------------------------------------------- info

const LLConfig* LLSubscriber::remote_config() const
{
    return d_->hdr ? &d_->remote : nullptr;
}

std::uint64_t LLSubscriber::session_id() const        { return d_->session; }
std::uint64_t LLSubscriber::sessions() const          { return d_->sessions; }
std::uint64_t LLSubscriber::frames_received() const   { return d_->received; }
std::uint64_t LLSubscriber::frames_dropped() const    { return d_->dropped; }
std::uint64_t LLSubscriber::commands_sent() const     { return d_->sent; }
std::uint64_t LLSubscriber::commands_received() const { return d_->recvd; }
std::uint64_t LLSubscriber::commands_dropped() const  { return d_->cmd_dropped; }
bool          LLSubscriber::producer_shutdown() const { return d_->peer_shutdown; }

void LLSubscriber::set_max_read_hz(double hz)
{
    d_->min_read_ns = (hz > 0.0) ? static_cast<std::uint64_t>(1e9 / hz) : 0ull;
}

double LLSubscriber::max_read_hz() const
{
    return d_->min_read_ns ? 1e9 / double(d_->min_read_ns) : 0.0;
}

bool LLSubscriber::read_due() const
{
    const Impl& m = *d_;
    if (!m.min_read_ns || !m.last_read_ns) return true;
    return (now_ns() - m.last_read_ns) >= m.min_read_ns;
}

std::uint64_t LLSubscriber::reads_throttled() const { return d_->throttled; }

void LLSubscriber::set_stale_timeout_ms(std::uint32_t ms)
{
    d_->stale_ns = std::uint64_t(ms) * 1000ull * 1000ull;
    d_->retry_ns = d_->stale_ns / 2 ? d_->stale_ns / 2 : 1;
}
