#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wr64::audio_ring_copy {

// Callers serialize access. Positions are monotonically increasing sample
// counters; count must fit within capacity and buffers must not overlap.
inline void write(int16_t* ring, size_t capacity, uint64_t position,
                  const int16_t* samples, size_t count) {
    if (count == 0) return;
    const size_t offset = static_cast<size_t>(position % capacity);
    const size_t first = std::min(count, capacity - offset);
    std::memcpy(ring + offset, samples, first * sizeof(int16_t));
    if (count > first) {
        std::memcpy(ring, samples + first, (count - first) * sizeof(int16_t));
    }
}

// This is a peek: the caller commits its read counter only after submission
// succeeds, so an audout failure leaves exactly the same samples available.
inline void read(const int16_t* ring, size_t capacity, uint64_t position,
                 int16_t* samples, size_t count) {
    if (count == 0) return;
    const size_t offset = static_cast<size_t>(position % capacity);
    const size_t first = std::min(count, capacity - offset);
    std::memcpy(samples, ring + offset, first * sizeof(int16_t));
    if (count > first) {
        std::memcpy(samples + first, ring, (count - first) * sizeof(int16_t));
    }
}

inline bool try_write(int16_t* ring, size_t capacity, uint64_t read_position,
                      uint64_t& write_position, const int16_t* samples, size_t count) {
    const size_t fill = static_cast<size_t>(write_position - read_position);
    if (count > capacity - fill) return false;
    write(ring, capacity, write_position, samples, count);
    write_position += count;
    return true;
}

} // namespace wr64::audio_ring_copy
