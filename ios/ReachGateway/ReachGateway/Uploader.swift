import Foundation

// Backend contract (callgate docs/REACH_INTEGRATION_HANDOFF.md):
//   POST {base}/api/device-events, Bearer device credential,
//   body exactly {"event_id","device_id","event_type":"help_request"}.
// 201 first receipt, 200 duplicate retry. Success means backend receipt only,
// not that a contact saw or accepted the request.
enum UploadOutcome: Equatable {
    case success(UploadState)    // .uploaded or .duplicate
    case retry(String)           // network error, timeout, 5xx, unexpected
    case stop(String)            // 401/403/409/413/422: do not retry

    static func classify(status: Int) -> UploadOutcome {
        switch status {
        case 201: return .success(.uploaded)
        case 200: return .success(.duplicate)
        case 401: return .stop("401 credential missing or rejected")
        case 403: return .stop("403 credential does not match device_id")
        case 409: return .stop("409 event_id already used for a different device or type")
        case 413: return .stop("413 body too large")
        case 422: return .stop("422 invalid body")
        default: return .retry("HTTP \(status)")
        }
    }

    // 2, 4, 8 ... s, capped at 60 s.
    static func backoff(afterTries tries: Int) -> TimeInterval {
        min(60, pow(2, Double(max(1, tries))))
    }
}

struct UploadRequest {
    static func make(baseURL: String, token: String, eventId: String, deviceId: String) -> URLRequest? {
        let trimmed = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        let base = trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
        guard let url = URL(string: base + "/api/device-events"), url.scheme == "https" || url.scheme == "http"
        else { return nil }
        var request = URLRequest(url: url, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        // ngrok free tunnels may show an interstitial to browser-like clients.
        request.setValue("1", forHTTPHeaderField: "ngrok-skip-browser-warning")
        let body = ["event_id": eventId, "device_id": deviceId, "event_type": "help_request"]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])
        return request
    }
}

enum Uploader {
    static func send(_ request: URLRequest) async -> UploadOutcome {
        do {
            let (_, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse else { return .retry("no HTTP response") }
            return UploadOutcome.classify(status: http.statusCode)
        } catch {
            return .retry(error.localizedDescription)
        }
    }
}
