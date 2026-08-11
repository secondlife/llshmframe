/**
 *
 * @file llsegment.h
 * @brief Private cross-platform shared-memory segment wrapper -- the only header that touches Boost.Interprocess
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
