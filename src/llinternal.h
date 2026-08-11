// src/llinternal.h  --  private. Not installed.
//
// The on-wire layout of the segment. Everything here is computed at runtime
// from the LLConfig and recorded in the header, so the two processes need not
// share compile-time constants. The subscriber recomputes the layout from the
// header's own config fields and checks the stored offsets agree, which turns
// an ABI mismatch into a clean VersionMismatch instead of silent corruption.

#pragma once

#include "shmframe/llshmframe.h"

#include <atomic>
#include <cstring>

namespace detail
{
    inline constexpr std::uint32_t kMagic   = 0x534D4634u; // 'SMF4'
    inline constexpr std::uint32_t kVersion = 4u;
    inline constexpr std::uint64_t kLine    = 64u;

    inline constexpr std::uint32_t kMaxSlots      = 4u; // index packed in 2 bits
    inline constexpr std::uint32_t kMinSlots      = 3u;
    inline constexpr std::uint64_t kMaxSegment    = 2ull * 1024 * 1024 * 1024;

    // Keeps width * bytes_per_pixel (32-bit) and
    // max_width * max_height * bytes_per_pixel (64-bit) comfortably clear of
    // overflow for any bytes_per_pixel up to the 16 allowed below.
    inline constexpr std::uint32_t kMaxDimension = 32768u;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "shared-memory atomics must be lock-free; a fallback lock "
                  "table is per-process and would provide no exclusion");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "");

    inline std::uint64_t align_up(std::uint64_t v, std::uint64_t a)
    {
        return (v + a - 1u) / a * a;
    }

    // ---------------------------------------------------------- structs

    struct LLSlotHeader
    {
        std::atomic<std::uint64_t> seq; // even = stable, odd = being written
        std::uint64_t frame_id;
        std::uint64_t timestamp_ns;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride;
        std::uint32_t payload_bytes;
    };

    struct LLRingHeader
    {
        alignas(kLine) std::atomic<std::uint64_t> head; // producer only
        alignas(kLine) std::atomic<std::uint64_t> tail; // consumer only
    };

    struct LLCmdHeader
    {
        std::uint64_t id;
        std::uint64_t reply_to;
        std::uint64_t timestamp_ns;
        std::uint32_t type;
        std::uint32_t size;
    };

    struct LLHeader
    {
        // Published last, with release ordering: seeing the magic implies
        // seeing a fully initialised header.
        alignas(kLine) std::atomic<std::uint32_t> magic;

        std::uint32_t version;
        std::uint64_t session_id;
        std::uint64_t total_size;

        // Config, echoed so the subscriber needs no compile-time match.
        std::uint32_t max_width;
        std::uint32_t max_height;
        std::uint32_t bytes_per_pixel;
        std::uint32_t slot_count;
        std::uint32_t command_slots;
        std::uint32_t max_command_bytes;

        // Derived offsets, verified by the subscriber.
        std::uint64_t slots_offset;
        std::uint64_t slot_stride;
        std::uint64_t downstream_offset;
        std::uint64_t upstream_offset;
        std::uint64_t ring_stride;
        std::uint64_t cmd_stride;

        // (frame_id << 2) | slot_index; 0 means nothing published yet.
        alignas(kLine) std::atomic<std::uint64_t> latest;
        alignas(kLine) std::atomic<std::uint32_t> shutdown;

        // Updated by every commit() and by heartbeat(); lets a peer tell an
        // idle-but-alive producer apart from a crashed one, and lets a new
        // LLPublisher::create() tell a live producer apart from an
        // abandoned segment (see LLPublisher::create()).
        alignas(kLine) std::atomic<std::uint64_t> last_heartbeat_ns;

        // 0 = unclaimed. Compare-exchanged to 1 by the first LLSubscriber to
        // attach to this session; enforces the single-subscriber-per-session
        // rule on the command channel, which (unlike frames) is not safe for
        // multiple concurrent readers/writers.
        alignas(kLine) std::atomic<std::uint32_t> command_owner;
    };

    // ---------------------------------------------------------- layout

    struct LLLayout
    {
        std::uint64_t header_size       = 0;
        std::uint64_t slots_offset      = 0;
        std::uint64_t slot_stride       = 0;
        std::uint64_t slot_payload_off  = 0;
        std::uint64_t downstream_offset = 0;
        std::uint64_t upstream_offset   = 0;
        std::uint64_t ring_stride       = 0;
        std::uint64_t ring_payload_off  = 0;
        std::uint64_t cmd_stride        = 0;
        std::uint64_t total_size        = 0;
    };

    inline bool config_valid(const LLConfig& c)
    {
        if (c.name.empty() || c.name.size() > 200) return false;
        if (c.max_width == 0 || c.max_height == 0) return false;
        if (c.max_width > kMaxDimension || c.max_height > kMaxDimension) return false;
        if (c.bytes_per_pixel == 0 || c.bytes_per_pixel > 16) return false;
        if (c.slot_count < kMinSlots || c.slot_count > kMaxSlots) return false;
        if (c.command_slots == 0) return false;
        if ((c.command_slots & (c.command_slots - 1)) != 0) return false; // pow2
        if (c.max_command_bytes == 0 || c.max_command_bytes > (1u << 20)) return false;
        return true;
    }

    inline LLLayout compute_layout(const LLConfig& c)
    {
        LLLayout L;
        L.header_size      = align_up(sizeof(LLHeader), kLine);
        L.slot_payload_off = align_up(sizeof(LLSlotHeader), kLine);
        L.slot_stride      = align_up(L.slot_payload_off + c.max_payload(), kLine);
        L.slots_offset     = L.header_size;

        L.ring_payload_off = align_up(sizeof(LLRingHeader), kLine);
        L.cmd_stride       = align_up(sizeof(LLCmdHeader) + c.max_command_bytes, kLine);
        L.ring_stride      = align_up(
            L.ring_payload_off + std::uint64_t(c.command_slots) * L.cmd_stride, kLine);

        L.downstream_offset = L.slots_offset + std::uint64_t(c.slot_count) * L.slot_stride;
        L.upstream_offset   = L.downstream_offset + L.ring_stride;
        L.total_size        = L.upstream_offset + L.ring_stride;
        return L;
    }

    // ------------------------------------------------------- accessors

    inline std::uint8_t* base_of(void* p) { return static_cast<std::uint8_t*>(p); }

    inline LLSlotHeader* slot_at(void* base, const LLLayout& L, std::uint32_t i)
    {
        return reinterpret_cast<LLSlotHeader*>(
            base_of(base) + L.slots_offset + std::uint64_t(i) * L.slot_stride);
    }

    inline std::uint8_t* slot_payload(void* base, const LLLayout& L, std::uint32_t i)
    {
        return base_of(base) + L.slots_offset +
               std::uint64_t(i) * L.slot_stride + L.slot_payload_off;
    }

    inline LLRingHeader* ring_at(void* base, std::uint64_t offset)
    {
        return reinterpret_cast<LLRingHeader*>(base_of(base) + offset);
    }

    inline LLCmdHeader* cmd_at(void* base, std::uint64_t ring_offset,
                               const LLLayout& L, std::uint64_t index)
    {
        return reinterpret_cast<LLCmdHeader*>(
            base_of(base) + ring_offset + L.ring_payload_off + index * L.cmd_stride);
    }

    inline std::uint8_t* cmd_payload(LLCmdHeader* h)
    {
        return reinterpret_cast<std::uint8_t*>(h) + sizeof(LLCmdHeader);
    }

    // Re-derives the layout from a mapped header's own echoed config and
    // checks it against the stored offsets. Returns nullptr (and leaves
    // out_layout untouched) unless the mapping is a valid, ABI-matching
    // segment of at least the size the layout requires.
    inline LLHeader* validate_header(void* base, std::uint64_t size, LLLayout& out_layout)
    {
        if (!base || size < sizeof(LLHeader)) return nullptr;

        auto* h = reinterpret_cast<LLHeader*>(base);
        if (h->magic.load(std::memory_order_acquire) != kMagic) return nullptr;
        if (h->version != kVersion) return nullptr;

        LLConfig c;
        c.name              = "x"; // placeholder; config_valid only checks length/emptiness
        c.max_width         = h->max_width;
        c.max_height        = h->max_height;
        c.bytes_per_pixel   = h->bytes_per_pixel;
        c.slot_count        = h->slot_count;
        c.command_slots     = h->command_slots;
        c.max_command_bytes = h->max_command_bytes;
        if (!config_valid(c)) return nullptr;

        const LLLayout cl = compute_layout(c);
        if (cl.slots_offset      != h->slots_offset      ||
            cl.slot_stride       != h->slot_stride       ||
            cl.downstream_offset != h->downstream_offset ||
            cl.upstream_offset   != h->upstream_offset   ||
            cl.ring_stride       != h->ring_stride       ||
            cl.cmd_stride        != h->cmd_stride        ||
            cl.total_size        != h->total_size)
            return nullptr;

        if (size < cl.total_size) return nullptr;

        out_layout = cl;
        return h;
    }

    // ------------------------------------------------- SPSC ring logic
    //
    // The producer only ever writes head; the consumer only ever writes
    // tail. That is what removes the need for any CAS.

    struct LLRing
    {
        void*           base       = nullptr;
        std::uint64_t   offset     = 0;
        const LLLayout* L          = nullptr;
        std::uint32_t   slots      = 0;
        std::uint32_t   max_bytes  = 0;

        bool valid() const { return base != nullptr; }

        bool push(std::uint32_t type, const void* data, std::uint32_t size,
                  std::uint64_t id, std::uint64_t reply_to, std::uint64_t ts)
        {
            if (!valid() || size > max_bytes) return false;

            LLRingHeader* r = ring_at(base, offset);
            const std::uint64_t head = r->head.load(std::memory_order_relaxed);
            const std::uint64_t tail = r->tail.load(std::memory_order_acquire);
            if (head - tail >= slots) return false; // full

            LLCmdHeader* c = cmd_at(base, offset, *L, head & (slots - 1));
            c->id           = id;
            c->reply_to     = reply_to;
            c->timestamp_ns = ts;
            c->type         = type;
            c->size         = size;
            if (size && data) std::memcpy(cmd_payload(c), data, size);

            r->head.store(head + 1, std::memory_order_release);
            return true;
        }

        bool pop(LLCommand& out)
        {
            if (!valid()) return false;

            LLRingHeader* r = ring_at(base, offset);
            const std::uint64_t tail = r->tail.load(std::memory_order_relaxed);
            const std::uint64_t head = r->head.load(std::memory_order_acquire);
            if (tail == head) return false; // empty

            LLCmdHeader* c = cmd_at(base, offset, *L, tail & (slots - 1));
            out.id           = c->id;
            out.reply_to     = c->reply_to;
            out.timestamp_ns = c->timestamp_ns;
            out.type         = c->type;

            const std::uint32_t n = (c->size > max_bytes) ? max_bytes : c->size;
            out.data.assign(cmd_payload(c), cmd_payload(c) + n);

            // Only now may the producer reuse this slot.
            r->tail.store(tail + 1, std::memory_order_release);
            return true;
        }

        std::uint32_t pending() const
        {
            if (!valid()) return 0;
            LLRingHeader* r = ring_at(base, offset);
            return static_cast<std::uint32_t>(
                r->head.load(std::memory_order_relaxed) -
                r->tail.load(std::memory_order_acquire));
        }
    };

    std::uint64_t make_session_id();
} // namespace detail
