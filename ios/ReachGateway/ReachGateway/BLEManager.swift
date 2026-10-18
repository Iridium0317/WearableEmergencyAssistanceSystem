import CoreBluetooth
import Foundation

protocol BLEManagerDelegate: AnyObject {
    func bleDidReceive(_ packet: AlertPacket)
    func bleDidReceive(_ status: DeviceStatus)
    func bleDidChange(_ link: BLEManager.Link)
    func bleLog(_ line: String)
}

// Foreground BLE central for the Reach wearable: scan by service UUID, connect,
// subscribe to Alert and Status, write Acks. Reconnects automatically after a
// disconnect. All callbacks arrive on the main queue.
final class BLEManager: NSObject {
    enum Link: Equatable {
        case bluetoothOff(String)
        case scanning
        case connecting
        case ready            // connected and subscribed to Alert
        case disconnected
    }

    weak var delegate: BLEManagerDelegate?
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var alertChar: CBCharacteristic?
    private var ackChar: CBCharacteristic?

    private let serviceUUID = CBUUID(string: AlertProtocol.serviceUUID)
    private let alertUUID = CBUUID(string: AlertProtocol.alertUUID)
    private let ackUUID = CBUUID(string: AlertProtocol.ackUUID)
    private let statusUUID = CBUUID(string: AlertProtocol.statusUUID)

    private(set) var link: Link = .disconnected {
        didSet { if link != oldValue { delegate?.bleDidChange(link) } }
    }

    func start() {
        if central == nil {
            central = CBCentralManager(delegate: self, queue: .main)
        } else {
            startScan()
        }
    }

    // Returns false if the ack could not be queued (not connected).
    @discardableResult
    func sendAck(alertId: UInt16) -> Bool {
        guard let peripheral, let ackChar, peripheral.state == .connected else { return false }
        peripheral.writeValue(AlertProtocol.encodeAck(alertId: alertId), for: ackChar, type: .withResponse)
        delegate?.bleLog("ACK queued id=\(alertId)")
        return true
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        // Prefer a peripheral iOS is already connected to (e.g. after app relaunch).
        if let known = central.retrieveConnectedPeripherals(withServices: [serviceUUID]).first {
            connect(known)
            return
        }
        link = .scanning
        central.scanForPeripherals(withServices: [serviceUUID])
    }

    private func connect(_ p: CBPeripheral) {
        central.stopScan()
        peripheral = p
        p.delegate = self
        link = .connecting
        central.connect(p)
    }

    private func resetCharacteristics() {
        alertChar = nil
        ackChar = nil
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            link = .bluetoothOff("Bluetooth permission denied. Allow it in Settings > Reach.")
        case .poweredOff:
            link = .bluetoothOff("Bluetooth is off.")
        default:
            link = .bluetoothOff("Bluetooth unavailable (\(central.state.rawValue)).")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        delegate?.bleLog("found \(peripheral.name ?? "?") rssi=\(RSSI)")
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        delegate?.bleLog("connected")
        resetCharacteristics()
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        delegate?.bleLog("connect failed: \(error?.localizedDescription ?? "unknown")")
        link = .disconnected
        startScan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        delegate?.bleLog("disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
        resetCharacteristics()
        link = .disconnected
        startScan()
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            delegate?.bleLog("Reach service not found")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics([alertUUID, ackUUID, statusUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] {
            switch c.uuid {
            case alertUUID:
                alertChar = c
                peripheral.setNotifyValue(true, for: c)
            case ackUUID:
                ackChar = c
            case statusUUID:
                peripheral.setNotifyValue(true, for: c)
                peripheral.readValue(for: c)
            default:
                break
            }
        }
        if alertChar == nil || ackChar == nil {
            delegate?.bleLog("missing Alert or Ack characteristic")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            delegate?.bleLog("subscribe failed: \(error.localizedDescription)")
            return
        }
        if characteristic.uuid == alertUUID, characteristic.isNotifying {
            delegate?.bleLog("subscribed to Alert")
            link = .ready
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let data = characteristic.value else { return }
        switch characteristic.uuid {
        case alertUUID:
            // Only notifications arrive here: the app never reads Alert, whose
            // readable value is just the last packet sent.
            if let packet = AlertProtocol.decodeAlert(data) {
                delegate?.bleDidReceive(packet)
            } else {
                delegate?.bleLog("malformed alert packet \(data.map { String(format: "%02x", $0) }.joined())")
            }
        case statusUUID:
            if let status = AlertProtocol.decodeStatus(data) {
                delegate?.bleDidReceive(status)
            }
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            delegate?.bleLog("ACK write failed: \(error.localizedDescription)")
        } else {
            delegate?.bleLog("ACK written")
        }
    }
}
