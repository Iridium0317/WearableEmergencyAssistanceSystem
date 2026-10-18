import XCTest
@testable import ReachGateway

final class AlertProtocolTests: XCTestCase {
    func testAckMatchesFirmwareLayout() {
        XCTAssertEqual([UInt8](AlertProtocol.encodeAck(alertId: 0x0102)), [0x01, 0x81, 0x02, 0x01])
    }

    func testDecodeAlert() {
        let packet = AlertProtocol.decodeAlert(Data([0x01, 0x01, 0x07, 0x00, 0x02, 0x01]))
        XCTAssertEqual(packet, AlertPacket(kind: .alert, alertId: 7, type: .medical, attempt: 1))
    }

    func testDecodeCancel() {
        let packet = AlertProtocol.decodeAlert(Data([0x01, 0x02, 0x07, 0x00, 0x03, 0x00]))
        XCTAssertEqual(packet, AlertPacket(kind: .cancel, alertId: 7, type: .family, attempt: 0))
    }

    func testRejectsMalformedAlerts() {
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x01, 0x01, 0x07, 0x00, 0x01])))        // short
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x02, 0x01, 0x07, 0x00, 0x01, 0x01])))  // version
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x01, 0x09, 0x07, 0x00, 0x01, 0x01])))  // msg type
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x01, 0x01, 0x07, 0x00, 0x04, 0x01])))  // alert type
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x01, 0x01, 0x00, 0x00, 0x01, 0x01])))  // id 0
        XCTAssertNil(AlertProtocol.decodeAlert(Data([0x01, 0x01, 0x07, 0x00, 0x01, 0x00])))  // attempt 0
    }

    func testDecodeStatus() {
        let status = AlertProtocol.decodeStatus(Data([0x01, 0x04, 0x07, 0x00, 0x01, 0x02]))
        XCTAssertEqual(status, DeviceStatus(state: .acknowledged, alertId: 7, type: .emergency, attempts: 2))
        XCTAssertNil(AlertProtocol.decodeStatus(Data([0x01, 0x08, 0x07, 0x00, 0x01, 0x02])))
    }
}

final class EventMatcherTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_000_000)

    private func event(alertId: UInt16, type: AlertType = .emergency, age: TimeInterval = 5) -> AlertEvent {
        AlertEvent(eventId: "nano-01-\(alertId)-\(age)", alertId: alertId, type: type,
                   receivedAt: now.addingTimeInterval(-age), lastAttempt: 1)
    }

    func testFirstAttemptIsAlwaysNew() {
        // After a board reset alert_id restarts at 1 and must not merge with an old event.
        let packet = AlertPacket(kind: .alert, alertId: 1, type: .emergency, attempt: 1)
        XCTAssertNil(EventMatcher.existingEvent(for: packet, in: [event(alertId: 1)], now: now))
    }

    func testRetransmissionReusesEvent() {
        let original = event(alertId: 3)
        let packet = AlertPacket(kind: .alert, alertId: 3, type: .emergency, attempt: 2)
        XCTAssertEqual(EventMatcher.existingEvent(for: packet, in: [original], now: now), original)
    }

    func testRetransmissionPicksNewestMatch() {
        let newer = event(alertId: 3, age: 5)
        let older = event(alertId: 3, age: 60)
        let packet = AlertPacket(kind: .alert, alertId: 3, type: .emergency, attempt: 3)
        XCTAssertEqual(EventMatcher.existingEvent(for: packet, in: [newer, older], now: now), newer)
    }

    func testRetransmissionWithDifferentTypeOrStaleIsNew() {
        let packet = AlertPacket(kind: .alert, alertId: 3, type: .medical, attempt: 2)
        XCTAssertNil(EventMatcher.existingEvent(for: packet, in: [event(alertId: 3, type: .emergency)], now: now))
        let stale = AlertPacket(kind: .alert, alertId: 3, type: .emergency, attempt: 2)
        XCTAssertNil(EventMatcher.existingEvent(for: stale, in: [event(alertId: 3, age: 3600)], now: now))
    }

    func testRetransmissionWithoutOriginalIsNew() {
        let packet = AlertPacket(kind: .alert, alertId: 9, type: .emergency, attempt: 2)
        XCTAssertNil(EventMatcher.existingEvent(for: packet, in: [], now: now))
    }

    func testEventIdFormatFitsBackendLimits() {
        let id = EventMatcher.newEventId(deviceId: "nano-01")
        XCTAssertTrue(id.hasPrefix("nano-01-"))
        XCTAssertLessThanOrEqual(id.count, 128)
        XCTAssertNotNil(id.range(of: "^[A-Za-z0-9_.:-]+$", options: .regularExpression))
        XCTAssertNotEqual(id, EventMatcher.newEventId(deviceId: "nano-01"))
    }
}

final class UploadTests: XCTestCase {
    func testStatusClassification() {
        XCTAssertEqual(UploadOutcome.classify(status: 201), .success(.uploaded))
        XCTAssertEqual(UploadOutcome.classify(status: 200), .success(.duplicate))
        for code in [401, 403, 409, 413, 422] {
            guard case .stop = UploadOutcome.classify(status: code) else { return XCTFail("\(code) should stop") }
        }
        for code in [500, 502, 503, 404, 429] {
            guard case .retry = UploadOutcome.classify(status: code) else { return XCTFail("\(code) should retry") }
        }
    }

    func testBackoffDoublesAndCaps() {
        XCTAssertEqual(UploadOutcome.backoff(afterTries: 1), 2)
        XCTAssertEqual(UploadOutcome.backoff(afterTries: 2), 4)
        XCTAssertEqual(UploadOutcome.backoff(afterTries: 3), 8)
        XCTAssertEqual(UploadOutcome.backoff(afterTries: 10), 60)
    }

    func testRequestHasExactlyThreeFields() throws {
        let request = try XCTUnwrap(UploadRequest.make(baseURL: "https://example.test/ ", token: "t",
                                                       eventId: "nano-01-x", deviceId: "nano-01"))
        XCTAssertEqual(request.url?.absoluteString, "https://example.test/api/device-events")
        XCTAssertEqual(request.httpMethod, "POST")
        XCTAssertEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer t")
        let body = try XCTUnwrap(JSONSerialization.jsonObject(with: request.httpBody ?? Data()) as? [String: String])
        XCTAssertEqual(body, ["event_id": "nano-01-x", "device_id": "nano-01", "event_type": "help_request"])
    }

    func testRejectsBadBaseURL() {
        XCTAssertNil(UploadRequest.make(baseURL: "", token: "t", eventId: "e", deviceId: "d"))
        XCTAssertNil(UploadRequest.make(baseURL: "ftp://x", token: "t", eventId: "e", deviceId: "d"))
    }
}
