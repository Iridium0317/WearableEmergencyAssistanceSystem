import Foundation

// Store-then-ack, kept apart from Bluetooth and the UI so it can be tested
// with fakes. A BLE ack tells the wearable the phone holds the alarm, so it is
// written only after the event is saved. If saving fails there is no ack: the
// wearable keeps retrying and then shows ERROR, the honest outcome.
protocol EventStoring: AnyObject {
    var events: [AlertEvent] { get }
    func insert(_ event: AlertEvent) throws
    func update(_ eventId: String, _ change: (inout AlertEvent) -> Void) throws
}

protocol AckSending: AnyObject {
    // Returns false if the ack could not be queued (not connected).
    func sendAck(alertId: UInt16) -> Bool
}

extension EventStore: EventStoring {}
extension BLEManager: AckSending {}

enum AlertIntake {
    enum Ack: Equatable {
        case sent
        case linkDown      // wearable will retransmit; that attempt reuses the event
        case skipped       // test tool
    }

    enum Outcome {
        case new(eventId: String, ack: Ack)
        case retransmission(eventId: String, ack: Ack)
        case saveFailed(Error)
    }

    static func receive(_ packet: AlertPacket, store: EventStoring, acker: AckSending,
                        deviceId: String, now: Date, skipAck: Bool) -> Outcome {
        let isNew: Bool
        let eventId: String
        do {
            if let existing = EventMatcher.existingEvent(for: packet, in: store.events, now: now) {
                isNew = false
                eventId = existing.eventId
                try store.update(eventId) { $0.lastAttempt = max($0.lastAttempt, packet.attempt) }
            } else {
                isNew = true
                eventId = EventMatcher.newEventId(deviceId: deviceId)
                try store.insert(AlertEvent(eventId: eventId, alertId: packet.alertId, type: packet.type,
                                            receivedAt: now, lastAttempt: packet.attempt))
            }
        } catch {
            return .saveFailed(error)
        }
        // A retransmission is acked again: the first ack may have been lost.
        let ack: Ack = skipAck ? .skipped : (acker.sendAck(alertId: packet.alertId) ? .sent : .linkDown)
        return isNew ? .new(eventId: eventId, ack: ack) : .retransmission(eventId: eventId, ack: ack)
    }
}
