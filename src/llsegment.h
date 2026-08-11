// src/llsegment.h  --  private.
//
// The one and only place Boost.Interprocess appears. Keeping it behind this
// interface is what lets the public header stay Boost-free, and makes it a
// small job to swap in a native backend later (windows_shared_memory, plain
// shm_open, or a DXGI-backed variant).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace detail
{
    class LLSegment
    {
    public:
        // Fails with already_exists = true if a live segment holds the name.
        static std::unique_ptr<LLSegment> create(const std::string& name,
                                                  std::uint64_t      bytes,
                                                  bool&              already_exists);

        // Returns nullptr if absent or not yet sized.
        static std::unique_ptr<LLSegment> open(const std::string& name);

        // Unlinks the name only. Existing mappings survive -- see the note
        // in LLSubscriber about orphan detection.
        static void unlink(const std::string& name);

        ~LLSegment();
        LLSegment(const LLSegment&)            = delete;
        LLSegment& operator=(const LLSegment&) = delete;

        void*         address() const;
        std::uint64_t size() const;

    private:
        LLSegment();
        struct Impl;
        std::unique_ptr<Impl> d_;
    };
} // namespace detail
