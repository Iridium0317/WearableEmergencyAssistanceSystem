import Foundation
import Security

// Backend URL and device ID live in UserDefaults; the device credential lives
// only in the Keychain. Nothing here is hardcoded, and the credential is never
// logged or shown after it is saved.
enum Settings {
    private static let baseURLKey = "backendBaseURL"
    private static let deviceIdKey = "deviceId"
    private static let tokenAccount = "deviceToken"
    private static let service = "com.iridium0317.reachgateway"

    static var baseURL: String {
        get { UserDefaults.standard.string(forKey: baseURLKey) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: baseURLKey) }
    }

    static var deviceId: String {
        get { UserDefaults.standard.string(forKey: deviceIdKey) ?? "nano-01" }
        set { UserDefaults.standard.set(newValue, forKey: deviceIdKey) }
    }

    static var token: String? {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static var hasToken: Bool { !(token ?? "").isEmpty }

    @discardableResult
    static func setToken(_ value: String) -> Bool {
        SecItemDelete(baseQuery as CFDictionary)
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return true }
        var query = baseQuery
        query[kSecValueData as String] = Data(trimmed.utf8)
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(query as CFDictionary, nil) == errSecSuccess
    }

    private static var baseQuery: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: tokenAccount]
    }
}
