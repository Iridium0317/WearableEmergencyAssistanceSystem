import SwiftUI
import UIKit

@main
struct ReachGatewayApp: App {
    @State private var gateway = Gateway()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(gateway)
                .onAppear {
                    // Keeps the screen on while the app is open (demos); locking the
                    // phone is fine, Bluetooth keeps running in the background.
                    UIApplication.shared.isIdleTimerDisabled = true
                }
                .onChange(of: scenePhase) { _, phase in
                    if phase == .active { gateway.resumeUploads() }
                }
        }
    }
}
