import Foundation
import Observation

// Alert lifecycle on the phone:
//   BLE alert -> persist event -> write BLE ack -> upload (retry, same event_id).
// The ack is written only after the event is saved, so an acked alarm is
// never lost. A retransmission (attempt > 1) reuses its original event_id and
// is acked again, since the first ack may have been lost.
@MainActor
@Observable
final class Gateway {
    private(set) var link: BLEManager.Link = .disconnected
    private(set) var deviceStatus: DeviceStatus?
    private(set) var events: [AlertEvent] = []
    private(set) var log: [String] = []
    private(set) var configured = false

    private let ble = BLEManager()
    private let store = EventStore()
    private var uploading: Set<String> = []

    init() {
        events = store.events
        ble.delegate = self
        refreshConfig()
    }

    func start() {
        ble.start()
        resumeUploads()
    }

    func refreshConfig() {
        configured = !Settings.baseURL.isEmpty && Settings.hasToken
    }

    func resumeUploads() {
        for event in store.events where event.upload == .pending {
            upload(event.eventId)
        }
    }

    // Retry an event the backend rejected (e.g. after fixing the credential).
    func retryUpload(_ eventId: String) {
        try? store.update(eventId) { $0.upload = .pending; $0.lastError = nil }
        events = store.events
        upload(eventId)
    }

    func clearHistory() {
        try? store.clear()
        events = store.events
    }

    private func handle(_ packet: AlertPacket) {
        switch packet.kind {
        case .alert:
            handleAlert(packet)
        case .cancel:
            // No device-scoped cancel endpoint yet (phase 2): mark it locally only.
            if let event = store.events.first(where: { $0.alertId == packet.alertId && $0.type == packet.type }) {
                try? store.update(event.eventId) { $0.cancelledOnDevice = true }
                events = store.events
            }
            append("CANCEL id=\(packet.alertId) \(packet.type.label) (local only)")
        }
    }

    private func handleAlert(_ packet: AlertPacket) {
        let now = Date()
        let eventId: String
        if let existing = EventMatcher.existingEvent(for: packet, in: store.events, now: now) {
            eventId = existing.eventId
            do {
                try store.update(eventId) { $0.lastAttempt = max($0.lastAttempt, packet.attempt) }
            } catch {
                append("SAVE FAILED, no ack: \(error.localizedDescription)")
                return
            }
            append("ALERT id=\(packet.alertId) attempt=\(packet.attempt) retransmission of \(eventId)")
        } else {
            eventId = EventMatcher.newEventId(deviceId: Settings.deviceId)
            let event = AlertEvent(eventId: eventId, alertId: packet.alertId, type: packet.type,
                                   receivedAt: now, lastAttempt: packet.attempt)
            do {
                try store.insert(event)
            } catch {
                // Without an ack the wearable retries and eventually shows ERROR,
                // which is the honest outcome if the phone cannot keep the alarm.
                append("SAVE FAILED, no ack: \(error.localizedDescription)")
                return
            }
            append("ALERT id=\(packet.alertId) \(packet.type.label) attempt=\(packet.attempt) new \(eventId)")
        }
        events = store.events
        if !ble.sendAck(alertId: packet.alertId) {
            append("ACK not sent: link down (wearable will retry)")
        }
        upload(eventId)
    }

    private func upload(_ eventId: String) {
        guard !uploading.contains(eventId) else { return }
        uploading.insert(eventId)
        Task { await uploadLoop(eventId) }
    }

    private func uploadLoop(_ eventId: String) async {
        defer { uploading.remove(eventId) }
        while let event = store.events.first(where: { $0.eventId == eventId }), event.upload == .pending {
            guard let token = Settings.token, !token.isEmpty,
                  let request = UploadRequest.make(baseURL: Settings.baseURL, token: token,
                                                   eventId: eventId, deviceId: Settings.deviceId) else {
                record(eventId, error: "Backend URL or device credential not set", status: nil)
                try? await Task.sleep(for: .seconds(5))
                continue
            }
            let outcome = await Uploader.send(request)
            switch outcome {
            case .success(let state):
                try? store.update(eventId) {
                    $0.upload = state
                    $0.uploadTries += 1
                    $0.lastHTTPStatus = state == .uploaded ? 201 : 200
                    $0.lastError = nil
                    $0.uploadedAt = Date()
                }
                events = store.events
                append("UPLOAD \(state == .uploaded ? "201 received" : "200 duplicate") \(eventId)")
                return
            case .stop(let reason):
                try? store.update(eventId) {
                    $0.upload = .rejected
                    $0.uploadTries += 1
                    $0.lastError = reason
                }
                events = store.events
                append("UPLOAD STOPPED \(reason) \(eventId)")
                return
            case .retry(let reason):
                let tries = (event.uploadTries) + 1
                record(eventId, error: reason, status: nil, tries: tries)
                let delay = UploadOutcome.backoff(afterTries: tries)
                append("UPLOAD retry in \(Int(delay)) s: \(reason)")
                try? await Task.sleep(for: .seconds(delay))
            }
        }
    }

    private func record(_ eventId: String, error: String, status: Int?, tries: Int? = nil) {
        try? store.update(eventId) {
            $0.lastError = error
            if let status { $0.lastHTTPStatus = status }
            if let tries { $0.uploadTries = tries }
        }
        events = store.events
    }

    private func append(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        log.insert("\(stamp) \(line)", at: 0)
        if log.count > 200 { log.removeLast() }
    }
}

extension Gateway: BLEManagerDelegate {
    nonisolated func bleDidReceive(_ packet: AlertPacket) {
        MainActor.assumeIsolated { handle(packet) }
    }

    nonisolated func bleDidReceive(_ status: DeviceStatus) {
        MainActor.assumeIsolated { deviceStatus = status }
    }

    nonisolated func bleDidChange(_ link: BLEManager.Link) {
        MainActor.assumeIsolated {
            self.link = link
            append("BLE \(link)")
        }
    }

    nonisolated func bleLog(_ line: String) {
        MainActor.assumeIsolated { append(line) }
    }
}
