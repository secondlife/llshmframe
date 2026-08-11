/**
 *
 * @file llutil.cpp
 * @brief Small standalone utilities: status-to-string, segment sizing, session-id generation, and a BGRA-to-BMP dumper
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

#include <chrono>
#include <cstdio>
#include <vector>

const char* to_string(LLStatus s)
{
    switch (s)
    {
        case LLStatus::Ok:              return "ok";
        case LLStatus::InvalidConfig:   return "invalid config";
        case LLStatus::AlreadyExists:   return "a publisher already owns this name";
        case LLStatus::VersionMismatch: return "incompatible segment layout";
        case LLStatus::MappingFailed:   return "mapping failed";
        case LLStatus::Internal:        return "internal error";
    }
    return "unknown";
}

std::uint64_t now_ns()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t segment_bytes(const LLConfig& cfg)
{
    if (!detail::config_valid(cfg)) return 0;
    return detail::compute_layout(cfg).total_size;
}

namespace detail
{
    std::uint64_t make_session_id()
    {
        const auto wall = static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        std::uint64_t h = wall * 0x9E3779B97F4A7C15ull;
        h ^= (now_ns() + 0x632BE59BD9B4E019ull);
        h ^= h >> 31;
        return h ? h : 1u;
    }
}

// ------------------------------------------------------------ BMP dump

namespace
{
    void put_u16(std::uint8_t* p, std::uint16_t v)
    {
        p[0] = std::uint8_t(v & 0xFF); p[1] = std::uint8_t((v >> 8) & 0xFF);
    }
    void put_u32(std::uint8_t* p, std::uint32_t v)
    {
        p[0] = std::uint8_t(v & 0xFF);         p[1] = std::uint8_t((v >> 8) & 0xFF);
        p[2] = std::uint8_t((v >> 16) & 0xFF); p[3] = std::uint8_t((v >> 24) & 0xFF);
    }
}

bool write_bmp_bgra(const std::string&  path,
                    const std::uint8_t* bgra,
                    std::uint32_t       width,
                    std::uint32_t       height,
                    std::uint32_t       stride,
                    bool                flip_vertically)
{
    if (!bgra || width == 0 || height == 0) return false;
    if (stride == 0) stride = width * 4u;

    const std::uint32_t row_bytes  = width * 3u;
    const std::uint32_t padding    = (4u - (row_bytes % 4u)) % 4u;
    const std::uint32_t padded_row = row_bytes + padding;
    const std::uint32_t pixels     = padded_row * height;
    const std::uint32_t offset     = 54u;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    std::uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2,  offset + pixels);
    put_u32(hdr + 10, offset);
    put_u32(hdr + 14, 40u);
    put_u32(hdr + 18, width);
    put_u32(hdr + 22, height);      // positive => bottom-up
    put_u16(hdr + 26, 1u);
    put_u16(hdr + 28, 24u);
    put_u32(hdr + 30, 0u);
    put_u32(hdr + 34, pixels);

    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    std::vector<std::uint8_t> row(padded_row, 0u);
    for (std::uint32_t i = 0; ok && i < height; ++i)
    {
        const std::uint32_t y = flip_vertically ? (height - 1u - i) : i;
        const std::uint8_t* s = bgra + std::size_t(y) * stride;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            row[x * 3u + 0] = s[x * 4u + 0];
            row[x * 3u + 1] = s[x * 4u + 1];
            row[x * 3u + 2] = s[x * 4u + 2];
        }
        ok = std::fwrite(row.data(), 1, padded_row, f) == padded_row;
    }

    ok = ok && (std::fflush(f) == 0);
    std::fclose(f);
    return ok;
}
