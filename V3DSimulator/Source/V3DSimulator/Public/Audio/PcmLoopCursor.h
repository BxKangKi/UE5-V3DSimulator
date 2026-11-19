// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace V3DAudio
{
// PCM16 cursor owned by one playback. The sink receives bounded spans without copying the clip.
template<class Sink>
bool QueueLoopedPcm(const std::uint8_t* bytes, std::size_t length, std::size_t requested,
    std::size_t& cursor, Sink&& sink)
{
    if (!bytes || length == 0 || length % 2 || requested % 2 || cursor >= length || cursor % 2) return false;
    while (requested)
    {
        const auto count = std::min(requested, length - cursor);
        sink(bytes + cursor, count);
        cursor = (cursor + count) % length;
        requested -= count;
    }
    return true;
}
}
