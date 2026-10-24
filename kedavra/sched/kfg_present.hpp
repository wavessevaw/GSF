// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>

// Caller serializes reserve/cancel/complete. Tokens never restart on history,
// connector or trace reset; connector-local DRM present IDs can restart.
template<typename Row, std::size_t Capacity = 64>
class KfgPresentQueue
{
public:
    uint64_t reserve(const Row &row)
    {
        if (rows.size() >= Capacity) { ++dropped; return 0; }
        const uint64_t token = ++next;
        rows.emplace(token, row);
        return token;
    }
    std::optional<Row> complete(uint64_t token)
    {
        auto it = rows.find(token);
        if (it == rows.end()) return std::nullopt;
        Row row = it->second;
        rows.erase(it);
        return row;
    }
    void cancel(uint64_t token) { rows.erase(token); }
    void reset() { rows.clear(); }
    uint64_t dropped = 0;
private:
    uint64_t next = 0;
    std::map<uint64_t, Row> rows;
};
