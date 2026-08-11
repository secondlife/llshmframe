/**
 *
 * @file llpublisher.cpp
 * @brief LLPublisher implementation: owns a shared-memory segment, publishes frames, and exchanges commands
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
#include <new>

using namespace detail;

struct LLPublisher::Impl
{
    LLConfig                  cfg;
    LLLayout                  L;
    std::unique_ptr<LLSegment> seg;
    LLHeader*                 hdr  = nullptr;
    void*                     base = nullptr;

    LLRing tx, rx;

    LLSlotHeader* pending    = nullptr;
    std::uint32_t write_slot = 0;
    std::uint64_t frame_id   = 0;
    std::uint64_t next_cmd   = 1;
    std::uint64_t sent = 0, recvd = 0, dropped = 0;

    std::uint64_t min_interval_ns = 0;  // 0 = unlimited
    std::uint64_t last_publish_ns = 0;
    std::uint64_t throttled       = 0;
};

LLPublisher::LLPublisher() : d_(new Impl) {}

LLPublisher::~LLPublisher()
{
    if (d_->hdr)
    {
        // Tell the peer we are going cleanly, then unlink the name.
        d_->hdr->shutdown.store(1, std::memory_order_release);
        LLSegment::unlink(d_->cfg.name);
    }
}

std::unique_ptr<LLPublisher> LLPublisher::create(const LLConfig& cfg, LLStatus* status)
{
    const auto fail = [&](LLStatus s) { if (status) *status = s; return nullptr; };

    if (!config_valid(cfg)) return fail(LLStatus::InvalidConfig);

    const LLLayout L = compute_layout(cfg);
    if (L.total_size == 0 || L.total_size > kMaxSegment)
        return fail(LLStatus::InvalidConfig);

    bool exists = false;
    auto seg = LLSegment::create(cfg.name, L.total_size, exists);

    if (!seg && exists)
    {
        // Something already answers to this name. Only steal it if it looks
        // abandoned -- an unparseable/foreign header, a clean-shutdown
        // marker, or a heartbeat older than stale_producer_ms -- otherwise a
        // genuinely live publisher would be silently orphaned mid-flight.
        bool reclaim = true;
        if (auto existing = LLSegment::open(cfg.name))
        {
            LLLayout existing_layout;
            if (LLHeader* eh = validate_header(existing->address(), existing->size(),
                                               existing_layout))
            {
                const bool shut = eh->shutdown.load(std::memory_order_acquire) != 0;
                const std::uint64_t age_ns =
                    now_ns() - eh->last_heartbeat_ns.load(std::memory_order_acquire);
                const std::uint64_t stale_ns =
                    std::uint64_t(cfg.stale_producer_ms) * 1'000'000ull;
                reclaim = shut || age_ns >= stale_ns;
            }
            // else: doesn't parse as one of ours -- foreign or corrupt,
            // safe to reclaim.
        }

        if (!reclaim) return fail(LLStatus::AlreadyExists);

        LLSegment::unlink(cfg.name);
        seg = LLSegment::create(cfg.name, L.total_size, exists);
    }

    if (!seg) return fail(exists ? LLStatus::AlreadyExists : LLStatus::MappingFailed);

    void* base = seg->address();

    // Value-initialise the control structures (payloads are left alone --
    // the OS already zero-fills, and touching 24 MB here would be waste).
    LLHeader* h = new (base) LLHeader();
    for (std::uint32_t i = 0; i < cfg.slot_count; ++i)
        new (slot_at(base, L, i)) LLSlotHeader();
    new (ring_at(base, L.downstream_offset)) LLRingHeader();
    new (ring_at(base, L.upstream_offset))   LLRingHeader();

    h->version           = kVersion;
    h->session_id        = make_session_id();
    h->total_size        = L.total_size;
    h->max_width         = cfg.max_width;
    h->max_height        = cfg.max_height;
    h->bytes_per_pixel   = cfg.bytes_per_pixel;
    h->slot_count        = cfg.slot_count;
    h->command_slots     = cfg.command_slots;
    h->max_command_bytes = cfg.max_command_bytes;
    h->slots_offset      = L.slots_offset;
    h->slot_stride       = L.slot_stride;
    h->downstream_offset = L.downstream_offset;
    h->upstream_offset   = L.upstream_offset;
    h->ring_stride       = L.ring_stride;
    h->cmd_stride        = L.cmd_stride;
    h->last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);

    // Release-store the magic last: a peer that sees it sees everything.
    h->magic.store(kMagic, std::memory_order_release);

    std::unique_ptr<LLPublisher> p(new LLPublisher);
    p->d_->cfg  = cfg;
    p->d_->L    = L;
    p->d_->seg  = std::move(seg);
    p->d_->hdr  = h;
    p->d_->base = base;
    p->d_->tx   = LLRing{base, L.downstream_offset, &p->d_->L,
                         cfg.command_slots, cfg.max_command_bytes};
    p->d_->rx   = LLRing{base, L.upstream_offset, &p->d_->L,
                         cfg.command_slots, cfg.max_command_bytes};

    if (status) *status = LLStatus::Ok;
    return p;
}

// ------------------------------------------------------------- frames

std::uint8_t* LLPublisher::begin_write(std::uint32_t width, std::uint32_t height)
{
    Impl& m = *d_;
    if (!m.hdr || m.pending) return nullptr;
    if (width == 0 || height == 0) return nullptr;

    // Rate gate. Checked before any shared-memory access so a throttled
    // call is as close to free as possible.
    if (m.min_interval_ns)
    {
        const std::uint64_t now = now_ns();
        if (m.last_publish_ns && (now - m.last_publish_ns) < m.min_interval_ns)
        {
            ++m.throttled;
            return nullptr;
        }
    }

    const std::uint64_t bytes =
        std::uint64_t(width) * height * m.cfg.bytes_per_pixel;
    if (bytes > m.cfg.max_payload()) return nullptr;

    m.write_slot = (m.write_slot + 1u) % m.cfg.slot_count;
    LLSlotHeader* s = slot_at(m.base, m.L, m.write_slot);

    // Mark odd before touching the slot; the release fence keeps the
    // marker ahead of the payload writes.
    const std::uint64_t s0 = s->seq.load(std::memory_order_relaxed);
    s->seq.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    s->width         = width;
    s->height        = height;
    s->stride        = width * m.cfg.bytes_per_pixel;
    s->payload_bytes = static_cast<std::uint32_t>(bytes);

    m.pending = s;
    return slot_payload(m.base, m.L, m.write_slot);
}

void LLPublisher::cancel_write() noexcept
{
    Impl& m = *d_;
    if (!m.pending) return;

    // Flip the marker back to even without publishing: no commit() ever
    // ran, so hdr->latest still points elsewhere and no subscriber will
    // ever visit this slot for this write attempt.
    LLSlotHeader* s = m.pending;
    const std::uint64_t s0 = s->seq.load(std::memory_order_relaxed);
    s->seq.store(s0 + 1, std::memory_order_release);
    m.pending = nullptr;
}

void LLPublisher::commit(std::uint64_t timestamp_ns)
{
    Impl& m = *d_;
    if (!m.pending) return;

    LLSlotHeader* s = m.pending;
    s->frame_id     = ++m.frame_id;
    s->timestamp_ns = timestamp_ns ? timestamp_ns : now_ns();

    const std::uint64_t s0 = s->seq.load(std::memory_order_relaxed);
    s->seq.store(s0 + 1, std::memory_order_release);

    m.hdr->latest.store((m.frame_id << 2) | m.write_slot,
                        std::memory_order_release);
    m.pending         = nullptr;
    m.last_publish_ns = now_ns();
    m.hdr->last_heartbeat_ns.store(m.last_publish_ns, std::memory_order_release);
}

bool LLPublisher::publish(const void* src, std::uint32_t width,
                          std::uint32_t height, std::uint32_t src_stride)
{
    if (!src) return false;
    std::uint8_t* dst = begin_write(width, height);
    if (!dst) return false;

    const std::uint32_t bpp = d_->cfg.bytes_per_pixel;
    const std::uint32_t dst_stride = width * bpp;
    if (src_stride == 0) src_stride = dst_stride;

    if (src_stride == dst_stride)
    {
        std::memcpy(dst, src, std::size_t(dst_stride) * height);
    }
    else
    {
        const auto* s = static_cast<const std::uint8_t*>(src);
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(dst + std::size_t(y) * dst_stride,
                        s + std::size_t(y) * src_stride, dst_stride);
    }

    commit();
    return true;
}

void LLPublisher::heartbeat()
{
    if (d_->hdr) d_->hdr->last_heartbeat_ns.store(now_ns(), std::memory_order_release);
}

// ----------------------------------------------------------- commands

bool LLPublisher::send(std::uint32_t type, const void* data, std::uint32_t size,
                       std::uint64_t reply_to, std::uint64_t* out_id)
{
    Impl& m = *d_;
    if (!m.hdr) return false;

    const std::uint64_t id = m.next_cmd;
    if (!m.tx.push(type, data, size, id, reply_to, now_ns()))
    {
        ++m.dropped;
        return false;
    }
    ++m.next_cmd;
    ++m.sent;
    if (out_id) *out_id = id;
    return true;
}

bool LLPublisher::send_text(std::uint32_t type, std::string_view text,
                            std::uint64_t reply_to, std::uint64_t* out_id)
{
    return send(type, text.data(), static_cast<std::uint32_t>(text.size()),
                reply_to, out_id);
}

bool LLPublisher::receive(LLCommand& out)
{
    if (!d_->hdr || !d_->rx.pop(out)) return false;
    ++d_->recvd;
    return true;
}

// --------------------------------------------------------------- info

void LLPublisher::set_max_publish_hz(double hz)
{
    d_->min_interval_ns = (hz > 0.0)
        ? static_cast<std::uint64_t>(1e9 / hz)
        : 0ull;
}

double LLPublisher::max_publish_hz() const
{
    return d_->min_interval_ns ? 1e9 / double(d_->min_interval_ns) : 0.0;
}

bool LLPublisher::publish_due() const
{
    const Impl& m = *d_;
    if (!m.min_interval_ns || !m.last_publish_ns) return true;
    return (now_ns() - m.last_publish_ns) >= m.min_interval_ns;
}

std::uint64_t LLPublisher::frames_throttled() const  { return d_->throttled; }
std::uint64_t LLPublisher::frames_published() const  { return d_->frame_id; }
std::uint64_t LLPublisher::session_id() const        { return d_->hdr ? d_->hdr->session_id : 0; }
std::uint64_t LLPublisher::commands_sent() const     { return d_->sent; }
std::uint64_t LLPublisher::commands_received() const { return d_->recvd; }
std::uint64_t LLPublisher::commands_dropped() const  { return d_->dropped; }
std::uint32_t LLPublisher::outbound_pending() const  { return d_->tx.pending(); }
const LLConfig& LLPublisher::config() const          { return d_->cfg; }

bool LLPublisher::has_subscriber() const
{
    return d_->hdr && d_->hdr->command_owner_gen.load(std::memory_order_acquire) != 0;
}

bool LLPublisher::command_owner_stale() const
{
    if (!d_->hdr) return false;
    if (d_->hdr->command_owner_gen.load(std::memory_order_acquire) == 0) return false;
    const std::uint64_t age = now_ns() -
        d_->hdr->command_owner_heartbeat_ns.load(std::memory_order_acquire);
    return age >= kCommandOwnerStaleNs;
}
