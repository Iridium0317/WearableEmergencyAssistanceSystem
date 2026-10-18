import XCTest
@testable import ReachGateway

// Store-then-ack: the fakes write to one shared journal so the tests can check
// the order of "saved" and "acked", not just that both happened.
private final class Journal {
    var entries: [String] = []
}

private struct SaveError: Error {}

private final class FakeStore: EventStoring {
    private(set) var events: [AlertEvent] = []
    var failSaves = false
    let journal: Journal

    init(_ journal: Journal) { self.journal = journal }

    func insert(_ event: AlertEvent) throws {
        if failSaves { throw SaveError() }
        events.insert(event, at: 0)
        journal.entries.append("insert \(event.alertId)")
    }

    func update(_ eventId: String, _ change: (inout AlertEvent) -> Void) throws {
        if failSaves { throw SaveError() }
        guard let i = events.firstIndex(where: { $0.eventId == eventId }) else { return }
        change(&events[i])
        journal.entries.append("update \(events[i].alertId)")
    }
}

private final class FakeLink: AckSending {
    var connected = true
    let journal: Journal

    init(_ journal: Journal) { self.journal = journal }

    func sendAck(alertId: UInt16) -> Bool {
        guard connected else { return false }
        journal.entries.append("ack \(alertId)")
        return true
    }
}

final class AlertIntakeTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_000_000)
    private var journal: Journal!
    private var store: FakeStore!
    private var link: FakeLink!

    override func setUp() {
        journal = Journal()
        store = FakeStore(journal)
        link = FakeLink(journal)
    }

    private func receive(id: UInt16, attempt: UInt8, skipAck: Bool = false,
                         after seconds: TimeInterval = 0) -> AlertIntake.Outcome {
        let packet = AlertPacket(kind: .alert, alertId: id, type: .emergency, attempt: attempt)
        return AlertIntake.receive(packet, store: store, acker: link, deviceId: "nano-01",
                                   now: now.addingTimeInterval(seconds), skipAck: skipAck)
    }

    func testNewAlertIsSavedBeforeAck() {
        guard case .new(let eventId, .sent) = receive(id: 7, attempt: 1) else { return XCTFail() }
        XCTAssertEqual(journal.entries, ["insert 7", "ack 7"])
        XCTAssertEqual(store.events.map(\.eventId), [eventId])
        XCTAssertEqual(store.events.first?.upload, .pending)
    }

    func testNoAckWhenSaveFails() {
        store.failSaves = true
        guard case .saveFailed = receive(id: 7, attempt: 1) else { return XCTFail() }
        XCTAssertEqual(journal.entries, [])
        XCTAssertTrue(store.events.isEmpty)
    }

    func testNoAckWhenRetransmissionCannotBeSaved() {
        _ = receive(id: 7, attempt: 1)
        store.failSaves = true
        guard case .saveFailed = receive(id: 7, attempt: 2, after: 7) else { return XCTFail() }
        XCTAssertEqual(journal.entries, ["insert 7", "ack 7"])
    }

    func testRetransmissionReusesEventAndIsAckedAgain() {
        guard case .new(let first, _) = receive(id: 7, attempt: 1),
              case .retransmission(let second, .sent) = receive(id: 7, attempt: 2, after: 7)
        else { return XCTFail() }
        XCTAssertEqual(first, second)
        XCTAssertEqual(journal.entries, ["insert 7", "ack 7", "update 7", "ack 7"])
        XCTAssertEqual(store.events.count, 1)
        XCTAssertEqual(store.events.first?.lastAttempt, 2)
    }

    func testLinkDownKeepsEventForTheRetransmission() {
        link.connected = false
        guard case .new(let first, .linkDown) = receive(id: 7, attempt: 1) else { return XCTFail() }
        XCTAssertEqual(store.events.count, 1)
        link.connected = true
        guard case .retransmission(let second, .sent) = receive(id: 7, attempt: 2, after: 7)
        else { return XCTFail() }
        XCTAssertEqual(first, second)
        XCTAssertEqual(journal.entries, ["insert 7", "update 7", "ack 7"])
    }

    func testSkippedAckStillSavesTheEvent() {
        guard case .new(_, .skipped) = receive(id: 7, attempt: 1, skipAck: true) else { return XCTFail() }
        XCTAssertEqual(journal.entries, ["insert 7"])
    }

    func testBoardResetStartsANewEvent() {
        // After a reset the wearable sends alert_id 1, attempt 1 again: a new alarm.
        guard case .new(let first, _) = receive(id: 1, attempt: 1),
              case .new(let second, _) = receive(id: 1, attempt: 1, after: 30)
        else { return XCTFail() }
        XCTAssertNotEqual(first, second)
        XCTAssertEqual(store.events.count, 2)
    }
}
