#include <unity.h>

#include "alert_protocol.h"

using namespace protocol;

void setUp() {}
void tearDown() {}

void test_alert_packet_layout_is_little_endian() {
    uint8_t buf[kAlertPacketSize];
    encodeAlert(buf, 0x1234, AlertType::Medical, 2);
    const uint8_t expected[] = {1, 0x01, 0x34, 0x12, 2, 2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, kAlertPacketSize);
}

void test_cancel_packet_has_zero_attempt() {
    uint8_t buf[kAlertPacketSize];
    encodeCancel(buf, 7, AlertType::Family);
    const uint8_t expected[] = {1, 0x02, 7, 0, 3, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, kAlertPacketSize);
}

void test_status_packet_layout() {
    uint8_t buf[kStatusPacketSize];
    encodeStatus(buf, 3, 0x0102, AlertType::Emergency, 1);
    const uint8_t expected[] = {1, 3, 0x02, 0x01, 1, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, kStatusPacketSize);
}

void test_valid_ack_decodes_id() {
    const uint8_t ack[] = {1, 0x81, 0x34, 0x12};
    uint16_t id = 0;
    TEST_ASSERT_EQUAL(AckError::None, decodeAck(ack, sizeof(ack), id));
    TEST_ASSERT_EQUAL_HEX16(0x1234, id);
}

void test_ack_rejects_wrong_length() {
    const uint8_t ack[] = {1, 0x81, 0x01, 0x00, 0xFF};
    uint16_t id = 99;
    TEST_ASSERT_EQUAL(AckError::BadLength, decodeAck(ack, 3, id));
    TEST_ASSERT_EQUAL(AckError::BadLength, decodeAck(ack, 5, id));
    TEST_ASSERT_EQUAL(AckError::BadLength, decodeAck(nullptr, 4, id));
    TEST_ASSERT_EQUAL_UINT16(99, id);  // untouched on error
}

void test_ack_rejects_wrong_version_type_and_zero_id() {
    uint16_t id = 0;
    const uint8_t bad_version[] = {2, 0x81, 1, 0};
    const uint8_t bad_type[] = {1, 0x01, 1, 0};
    const uint8_t zero_id[] = {1, 0x81, 0, 0};
    TEST_ASSERT_EQUAL(AckError::BadVersion, decodeAck(bad_version, 4, id));
    TEST_ASSERT_EQUAL(AckError::BadType, decodeAck(bad_type, 4, id));
    TEST_ASSERT_EQUAL(AckError::BadId, decodeAck(zero_id, 4, id));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alert_packet_layout_is_little_endian);
    RUN_TEST(test_cancel_packet_has_zero_attempt);
    RUN_TEST(test_status_packet_layout);
    RUN_TEST(test_valid_ack_decodes_id);
    RUN_TEST(test_ack_rejects_wrong_length);
    RUN_TEST(test_ack_rejects_wrong_version_type_and_zero_id);
    return UNITY_END();
}
