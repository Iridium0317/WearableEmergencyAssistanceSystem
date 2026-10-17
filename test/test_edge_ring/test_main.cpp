#include <unity.h>

#include "edge_ring.h"

void setUp() {}
void tearDown() {}

namespace {

ButtonEdge edge(uint8_t button, uint32_t t) { return ButtonEdge{button, (t % 2) == 0, t, t * 1000}; }

}  // namespace

void test_empty_ring_pops_nothing() {
    EdgeRing ring;
    ButtonEdge out{};
    TEST_ASSERT_FALSE(ring.pop(out));
}

void test_preserves_fifo_order_and_contents() {
    EdgeRing ring;
    for (uint32_t t = 0; t < 5; ++t) {
        TEST_ASSERT_TRUE(ring.push(edge(t % 3, t)));
    }
    for (uint32_t t = 0; t < 5; ++t) {
        ButtonEdge out{};
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT8(t % 3, out.button);
        TEST_ASSERT_EQUAL(t % 2 == 0, out.pressed);
        TEST_ASSERT_EQUAL_UINT32(t, out.t_ms);
        TEST_ASSERT_EQUAL_UINT32(t * 1000, out.t_us);
    }
    ButtonEdge out{};
    TEST_ASSERT_FALSE(ring.pop(out));
}

void test_full_ring_rejects_push_without_corruption() {
    EdgeRing ring;
    const uint32_t usable = EdgeRing::kCapacity - 1;
    for (uint32_t t = 0; t < usable; ++t) {
        TEST_ASSERT_TRUE(ring.push(edge(0, t)));
    }
    TEST_ASSERT_FALSE(ring.push(edge(0, 999)));  // dropped, not overwritten
    ButtonEdge out{};
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT32(0, out.t_ms);  // oldest entry intact
    TEST_ASSERT_TRUE(ring.push(edge(0, 1000)));  // one slot freed
}

void test_wraps_around_many_times() {
    EdgeRing ring;
    uint32_t next_out = 0;
    for (uint32_t t = 0; t < EdgeRing::kCapacity * 10; ++t) {
        TEST_ASSERT_TRUE(ring.push(edge(1, t)));
        if (t % 3 == 2) {  // consumer lags, then catches up
            ButtonEdge out{};
            while (ring.pop(out)) {
                TEST_ASSERT_EQUAL_UINT32(next_out++, out.t_ms);
            }
        }
    }
    ButtonEdge out{};
    while (ring.pop(out)) {
        TEST_ASSERT_EQUAL_UINT32(next_out++, out.t_ms);
    }
    TEST_ASSERT_EQUAL_UINT32(EdgeRing::kCapacity * 10, next_out);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_empty_ring_pops_nothing);
    RUN_TEST(test_preserves_fifo_order_and_contents);
    RUN_TEST(test_full_ring_rejects_push_without_corruption);
    RUN_TEST(test_wraps_around_many_times);
    return UNITY_END();
}
