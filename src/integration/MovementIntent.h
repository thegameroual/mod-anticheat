/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <algorithm>
#include <array>

namespace anticheat
{
// All supported server movement orders begin with a packed mover GUID.
// Decode without copying/mutating a WorldPacket or trusting its read cursor.
inline bool ReadPackedMoverGuid(std::uint8_t const* bytes, std::size_t size,
    std::uint64_t& guid, std::size_t* payloadOffset = nullptr)
{
    guid = 0;
    if (!bytes || !size)
        return false;
    std::uint8_t const mask = bytes[0];
    std::size_t offset = 1;
    for (unsigned i = 0; i != 8; ++i)
        if (mask & (1u << i))
        {
            if (offset >= size)
                return false;
            guid |= static_cast<std::uint64_t>(bytes[offset++]) << (8u * i);
        }
    // A server movement order also contains its uint32 order counter.
    if (!guid || size - offset < 4)
        return false;
    if (payloadOffset)
        *payloadOffset = offset + 4;
    return true;
}

inline bool ReadServerSpeed(std::uint8_t const* bytes, std::size_t size, bool run, float& speed)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
        "WotLK movement packets require IEEE-754 binary32 floats");
    std::uint64_t guid;
    std::size_t offset;
    if (!ReadPackedMoverGuid(bytes, size, guid, &offset))
        return false;
    offset += run ? 1 : 0; // SMSG_FORCE_RUN_SPEED_CHANGE has one extra byte.
    if (size != offset + 4)
        return false;
    std::uint32_t bits = 0;
    for (unsigned i = 0; i != 4; ++i)
        bits |= static_cast<std::uint32_t>(bytes[offset + i]) << (8u * i);
    std::memcpy(&speed, &bits, sizeof(bits));
    return std::isfinite(speed) && speed >= 0;
}

struct SpeedAllowance
{
    float maximum = 0;
    std::uint64_t expires = 0;
};

struct SpeedBudget
{
    double distance = 0;
    float peak = 0;
};

// Retain independently expiring, mode-specific server speed transitions.
// Dominated entries are removed; an old high speed never inherits a later
// lower speed's deadline. No allocation occurs on outgoing movement orders.
class SpeedTransitionHistory
{
public:
    bool Record(unsigned mode, float previous, float current, std::uint64_t now,
        std::uint32_t duration, std::uint64_t retainSince)
    {
        if (!std::isfinite(previous) || !std::isfinite(current) || previous < 0 || current < 0)
            return false;
        if (std::abs(previous - current) < 0.001f)
            return true;
        Event next{mode, std::max(previous, current), now + duration};
        for (std::size_t i = 0; i < _size;)
        {
            Event const& existing = _events[i];
            if (existing.expires <= retainSince || (existing.mode == mode &&
                existing.maximum <= next.maximum && existing.expires <= next.expires))
                _events[i] = _events[--_size];
            else
                ++i;
        }
        if (_size == _events.size())
            return false;
        _events[_size++] = next;
        return true;
    }

    SpeedAllowance ForMode(unsigned mode, std::uint64_t intervalStart) const
    {
        SpeedAllowance result;
        for (std::size_t i = 0; i < _size; ++i)
            if (_events[i].mode == mode && _events[i].expires > intervalStart &&
                _events[i].maximum > result.maximum)
                result = {_events[i].maximum, _events[i].expires};
        return result;
    }

    bool Active(std::uint64_t now) const
    {
        for (std::size_t i = 0; i < _size; ++i)
            if (_events[i].expires > now)
                return true;
        return false;
    }

    SpeedBudget Budget(unsigned mode, std::uint64_t begin, std::uint64_t end, float baseSpeed) const
    {
        SpeedBudget result{0, baseSpeed};
        while (begin < end)
        {
            auto boundary = end;
            float speed = baseSpeed;
            for (std::size_t i = 0; i < _size; ++i)
                if (_events[i].mode == mode && _events[i].expires > begin)
                {
                    boundary = std::min(boundary, _events[i].expires);
                    speed = std::max(speed, _events[i].maximum);
                }
            result.distance += speed * (static_cast<double>(boundary - begin) / 1000.0);
            result.peak = std::max(result.peak, speed);
            begin = boundary;
        }
        return result;
    }
    void Clear() { _size = 0; }

private:
    struct Event { unsigned mode; float maximum; std::uint64_t expires; };
    std::array<Event, 32> _events{};
    std::size_t _size = 0;
};
}
