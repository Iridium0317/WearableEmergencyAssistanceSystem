import SwiftUI
import UIKit

struct ContentView: View {
    @Environment(Gateway.self) private var gateway
    @State private var showSettings = false

    var body: some View {
        NavigationStack {
            List {
                Section("Wearable") {
                    LinkRow(link: gateway.link)
                    if let status = gateway.deviceStatus {
                        LabeledContent("Device state", value: status.state.label)
                    }
                }
                if !gateway.configured {
                    Section {
                        Button("Set backend URL and device credential") { showSettings = true }
                            .foregroundStyle(.orange)
                    }
                }
                Section("Alerts") {
                    if gateway.events.isEmpty {
                        Text("No alerts yet. Hold a button on the wearable.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(gateway.events) { event in
                        EventRow(event: event) { gateway.retryUpload(event.eventId) }
                    }
                }
                Section("Test tools") {
                    @Bindable var gateway = gateway
                    Toggle("Skip the next ACK", isOn: $gateway.dropNextAck)
                }
                Section("Log") {
                    ForEach(Array(gateway.log.prefix(60).enumerated()), id: \.offset) { _, line in
                        Text(line).font(.caption.monospaced())
                    }
                }
            }
            .navigationTitle("Reach")
            .toolbar {
                Button { showSettings = true } label: { Image(systemName: "gearshape") }
            }
            .sheet(isPresented: $showSettings, onDismiss: { gateway.refreshConfig(); gateway.resumeUploads() }) {
                SettingsView()
            }
        }
    }
}

private struct LinkRow: View {
    let link: BLEManager.Link

    var body: some View {
        switch link {
        case .ready: Label("Connected", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
        case .scanning: Label("Searching for wearable…", systemImage: "antenna.radiowaves.left.and.right")
        case .connecting: Label("Connecting…", systemImage: "hourglass")
        case .disconnected: Label("Disconnected", systemImage: "xmark.circle").foregroundStyle(.red)
        case .bluetoothOff(let reason): Label(reason, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
        }
    }
}

private struct EventRow: View {
    let event: AlertEvent
    let retry: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(event.type.label).font(.headline)
                if event.cancelledOnDevice {
                    Text("Cancelled on wearable").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Text(event.receivedAt, style: .time).font(.caption).foregroundStyle(.secondary)
            }
            uploadLabel
            Text(event.eventId)
                .font(.caption.monospaced())
                .textSelection(.enabled)
            HStack {
                Text("alert_id \(event.alertId) · attempt \(event.lastAttempt) · tries \(event.uploadTries)")
                    .font(.caption2).foregroundStyle(.secondary)
                Spacer()
                Button("Copy ID") { UIPasteboard.general.string = event.eventId }
                    .buttonStyle(.bordered).controlSize(.small)
                if event.upload == .rejected {
                    Button("Retry", action: retry).buttonStyle(.bordered).controlSize(.small)
                }
            }
        }
        .padding(.vertical, 4)
    }

    // Upload success means the backend recorded the request, nothing more.
    @ViewBuilder private var uploadLabel: some View {
        switch event.upload {
        case .uploaded:
            Label("Recorded by backend (201)", systemImage: "checkmark.icloud").foregroundStyle(.green)
        case .duplicate:
            Label("Recorded by backend (200 duplicate)", systemImage: "checkmark.icloud").foregroundStyle(.green)
        case .pending:
            Label("Sending to backend…\(event.lastError.map { " (\($0))" } ?? "")", systemImage: "icloud.and.arrow.up")
                .foregroundStyle(.orange)
        case .rejected:
            Label("Not recorded: \(event.lastError ?? "rejected")", systemImage: "xmark.icloud").foregroundStyle(.red)
        }
    }
}

struct SettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var baseURL = Settings.baseURL
    @State private var deviceId = Settings.deviceId
    @State private var token = ""
    @State private var hasToken = Settings.hasToken

    var body: some View {
        NavigationStack {
            Form {
                Section("Backend") {
                    TextField("https://…", text: $baseURL)
                        .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Device ID", text: $deviceId)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                }
                Section {
                    SecureField(hasToken ? "Saved (enter to replace)" : "Device credential", text: $token)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    if hasToken {
                        Button("Remove saved credential", role: .destructive) {
                            Settings.setToken("")
                            hasToken = false
                        }
                    }
                } header: {
                    Text("Device credential")
                } footer: {
                    Text("Stored in the iPhone Keychain only. Upload base URL, without /api/device-events.")
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        Settings.baseURL = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
                        let id = deviceId.trimmingCharacters(in: .whitespacesAndNewlines)
                        Settings.deviceId = id.isEmpty ? "nano-01" : id
                        if !token.isEmpty { Settings.setToken(token) }
                        dismiss()
                    }
                }
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            }
        }
    }
}
