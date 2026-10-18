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
                    // Phase 1 is foreground-only: keep the screen on while testing.
                    UIApplication.shared.isIdleTimerDisabled = true
                    gateway.start()
                }
                .onChange(of: scenePhase) { _, phase in
                    if phase == .active { gateway.resumeUploads() }
                }
        }
    }
}
