#include "test_framework.h"

#include "capture/ring_buffer.h"

TEST_CASE("bounded ring buffer reports full and preserves FIFO order") {
    smp::SpscRingBuffer<int, 4> queue;
    REQUIRE(queue.tryPush(10));
    REQUIRE(queue.tryPush(20));
    REQUIRE(queue.tryPush(30));
    REQUIRE(!queue.tryPush(40));
    int value = 0;
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 10);
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 20);
    REQUIRE(queue.tryPush(40));
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 30);
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 40);
    REQUIRE(!queue.tryPop(value));
    REQUIRE(queue.empty());
}
