import Foundation

// Mirror of lib/protocol/alert_protocol.h in the firmware. Pure, so it can be
// unit-tested without Bluetooth. All multi-byte fields are little-endian.
//
// Device -> phone, Alert characteristic (notify), 6 bytes:
//   [0] version (1)  [1] type: 0x01 ALERT, 0x02 CANCEL
//   [2..3] alert_id  [4] alert type (1..3)  [5] attempt (1..n, 0 for CANCEL)
//
// Device -> phone, Status characteristic (read/notify), 6 bytes:
//   [0] version (1)  [1] state  [2..3] alert_id  [4] alert type  [5] attempts so far
//
// Phone -> device, Ack characteristic (write), 4 bytes:
//   [0] version (1)  [1] type: 0x81 ACK  [2..3] alert_id (non-zero)
enum AlertProtocol {
    static let version: UInt8 = 1
    static let msgAlert: UInt8 = 0x01
    static let msgCancel: UInt8 = 0x02
    static let msgAck: UInt8 = 0x81

    static let deviceName = "WEA-Nano33"
    static let serviceUUID = "8f1d0001-6c2a-4b7e-9e3d-5a4c3b2a1f00"
    static let alertUUID = "8f1d0002-6c2a-4b7e-9e3d-5a4c3b2a1f00"
    static let ackUUID = "8f1d0003-6c2a-4b7e-9e3d-5a4c3b2a1f00"
    static let statusUUID = "8f1d0004-6c2a-4b7e-9e3d-5a4c3b2a1f00"

    static func encodeAck(alertId: UInt16) -> Data {
        Data([version, msgAck, UInt8(alertId & 0xFF), UInt8(alertId >> 8)])
    }

    static func decodeAlert(_ data: Data) -> AlertPacket? {
        let b = [UInt8](data)
        guard b.count == 6, b[0] == version, let type = AlertType(rawValue: b[4]) else { return nil }
        let alertId = UInt16(b[2]) | UInt16(b[3]) << 8
        guard alertId != 0 else { return nil }
        switch b[1] {
        case msgAlert:
            guard b[5] >= 1 else { return nil }
            return AlertPacket(kind: .alert, alertId: alertId, type: type, attempt: b[5])
        case msgCancel:
            return AlertPacket(kind: .cancel, alertId: alertId, type: type, attempt: 0)
        default:
            return nil
        }
    }

    static func decodeStatus(_ data: Data) -> DeviceStatus? {
        let b = [UInt8](data)
        guard b.count == 6, b[0] == version, let state = DeviceState(rawValue: b[1]) else { return nil }
        return DeviceStatus(state: state, alertId: UInt16(b[2]) | UInt16(b[3]) << 8,
                            type: AlertType(rawValue: b[4]), attempts: b[5])
    }
}

enum AlertType: UInt8, Codable {
    case emergency = 1
    case medical = 2
    case family = 3

    var label: String {
        switch self {
        case .emergency: return "Emergency"
        case .medical: return "Medical"
        case .family: return "Family"
        }
    }
}

struct AlertPacket: Equatable {
    enum Kind { case alert, cancel }
    let kind: Kind
    let alertId: UInt16
    let type: AlertType
    let attempt: UInt8
}

// Order matches the firmware State enum.
enum DeviceState: UInt8 {
    case idle, pending, confirmed, alertSent, acknowledged, cancelled, retry, error

    var label: String {
        switch self {
        case .idle: return "Idle"
        case .pending: return "Pending (can cancel)"
        case .confirmed: return "Confirmed"
        case .alertSent: return "Alert sent"
        case .acknowledged: return "Acknowledged"
        case .cancelled: return "Cancelled"
        case .retry: return "Retrying"
        case .error: return "Not delivered"
        }
    }
}

struct DeviceStatus: Equatable {
    let state: DeviceState
    let alertId: UInt16
    let type: AlertType?
    let attempts: UInt8
}
