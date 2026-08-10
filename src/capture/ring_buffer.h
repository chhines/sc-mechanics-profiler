#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace smp {

// A bounded single-producer/single-consumer queue. Capacity must be a power of two.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line padding prevents false sharing.
#endif
template <typename T, std::size_t Capacity> class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_copy_assignable_v<T>);

  public:
    SpscRingBuffer() : entries_(std::make_unique<T[]>(Capacity)) {}
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool tryPush(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & mask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        entries_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        value = entries_[tail];
        tail_.store((tail + 1) & mask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

  private:
    static constexpr std::size_t mask = Capacity - 1;
    std::unique_ptr<T[]> entries_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace smp
