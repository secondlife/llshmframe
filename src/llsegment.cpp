/**
 *
 * @file llsegment.cpp
 * @brief LLSegment implementation: Windows and POSIX shared-memory create/open/unlink via Boost.Interprocess
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

#include "llsegment.h"

#include <boost/interprocess/mapped_region.hpp>
#if defined(_WIN32)
#  include <boost/interprocess/windows_shared_memory.hpp>
#else
#  include <boost/interprocess/shared_memory_object.hpp>
#endif

namespace bip = boost::interprocess;

namespace detail
{
#if defined(_WIN32)
    // Native kernel section object: no filesystem-visible name, no
    // separate truncate() (size goes straight into create_only), and no
    // remove() -- it is destroyed automatically once the last handle and
    // mapped_region referencing it are gone.
    using SharedMem = bip::windows_shared_memory;
#else
    using SharedMem = bip::shared_memory_object;
#endif

    struct LLSegment::Impl
    {
        std::unique_ptr<SharedMem>           obj;
        std::unique_ptr<bip::mapped_region>  region;
    };

    LLSegment::LLSegment() : d_(new Impl) {}
    LLSegment::~LLSegment() = default;

    std::unique_ptr<LLSegment> LLSegment::create(const std::string& name,
                                                  std::uint64_t      bytes,
                                                  bool&              already_exists)
    {
        already_exists = false;
        try
        {
            std::unique_ptr<LLSegment> s(new LLSegment);
#if defined(_WIN32)
            s->d_->obj = std::make_unique<SharedMem>(
                bip::create_only, name.c_str(), bip::read_write,
                static_cast<std::size_t>(bytes));
#else
            s->d_->obj = std::make_unique<SharedMem>(
                bip::create_only, name.c_str(), bip::read_write);
            s->d_->obj->truncate(static_cast<bip::offset_t>(bytes));
#endif
            s->d_->region = std::make_unique<bip::mapped_region>(
                *s->d_->obj, bip::read_write);

            if (!s->d_->region->get_address() ||
                s->d_->region->get_size() < bytes)
                return nullptr;

            return s;
        }
        catch (const bip::interprocess_exception& e)
        {
            // Boost reports EEXIST / ERROR_ALREADY_EXISTS / ERROR_FILE_EXISTS
            // as already_exists_error, so this is an exact check rather than
            // a probe-and-guess -- no second open racing the caller's retry.
            already_exists = (e.get_error_code() == bip::already_exists_error);
            return nullptr;
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    std::unique_ptr<LLSegment> LLSegment::open(const std::string& name)
    {
        try
        {
            std::unique_ptr<LLSegment> s(new LLSegment);
            s->d_->obj = std::make_unique<SharedMem>(
                bip::open_only, name.c_str(), bip::read_write);

            // read_write even though we mostly read: locking-free atomics
            // are still read-modify-write instructions on some targets,
            // and the command channel genuinely writes.
            s->d_->region = std::make_unique<bip::mapped_region>(
                *s->d_->obj, bip::read_write);

            if (!s->d_->region->get_address() || s->d_->region->get_size() == 0)
                return nullptr;

            return s;
        }
        catch (const bip::interprocess_exception&) { return nullptr; }
        catch (const std::exception&)              { return nullptr; }
    }

    void LLSegment::unlink(const std::string& name)
    {
#if defined(_WIN32)
        // Nothing to unlink: windows_shared_memory has no name in the
        // filesystem/registry to remove. The section dies on its own
        // when the owning LLSegment (and any peer still mapped into it)
        // releases its handle.
        (void)name;
#else
        try { bip::shared_memory_object::remove(name.c_str()); }
        catch (...) {}
#endif
    }

    void* LLSegment::address() const
    {
        return d_->region ? d_->region->get_address() : nullptr;
    }

    std::uint64_t LLSegment::size() const
    {
        return d_->region ? static_cast<std::uint64_t>(d_->region->get_size()) : 0;
    }
} // namespace detail
