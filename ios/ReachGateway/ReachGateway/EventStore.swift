import Foundation

enum UploadState: String, Codable {
    case pending          // not yet answered 201/200; will retry
    case uploaded         // backend answered 201 (first receipt)
    case duplicate        // backend answered 200 (an earlier try already landed)
    case rejected         // 401/403/409/413/422: retrying cannot help

    var isFinal: Bool { self != .pending }
}

struct AlertEvent: Codable, Identifiable, Equatable {
    var id: String { eventId }
    let eventId: String
    let alertId: UInt16
    let type: AlertType
    let receivedAt: Date
    var lastAttempt: UInt8
    var cancelledOnDevice: Bool = false
    var upload: UploadState = .pending
    var uploadTries: Int = 0
    var lastHTTPStatus: Int?
    var lastError: String?
    var uploadedAt: Date?
}

// Decides whether an incoming alert is a retransmission of an event we already
// hold. The wearable reuses alert_id for every attempt of one alarm, and
// restarts alert_id at 1 after a reset, so attempt 1 always starts a new event.
enum EventMatcher {
    static let reuseWindow: TimeInterval = 10 * 60

    static func existingEvent(for packet: AlertPacket, in events: [AlertEvent], now: Date) -> AlertEvent? {
        guard packet.attempt > 1 else { return nil }
        return events.first {
            $0.alertId == packet.alertId && $0.type == packet.type
                && now.timeIntervalSince($0.receivedAt) < reuseWindow
        }
    }

    static func newEventId(deviceId: String) -> String {
        "\(deviceId)-\(UUID().uuidString.lowercased())"
    }
}

// Events newest first, persisted to a JSON file after every change so a crash
// or relaunch never loses an alarm. Writes are synchronous and atomic: the
// BLE ack is only sent after save() returns.
final class EventStore {
    private(set) var events: [AlertEvent] = []
    private let url: URL

    init(filename: String = "events.json") {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appendingPathComponent(filename)
        if let data = try? Data(contentsOf: url),
           let saved = try? JSONDecoder().decode([AlertEvent].self, from: data) {
            events = saved
        }
    }

    func insert(_ event: AlertEvent) throws {
        events.insert(event, at: 0)
        try save()
    }

    func update(_ eventId: String, _ change: (inout AlertEvent) -> Void) throws {
        guard let i = events.firstIndex(where: { $0.eventId == eventId }) else { return }
        change(&events[i])
        try save()
    }

    func clear() throws {
        events.removeAll()
        try save()
    }

    private func save() throws {
        let data = try JSONEncoder().encode(events)
        try data.write(to: url, options: .atomic)
    }
}
