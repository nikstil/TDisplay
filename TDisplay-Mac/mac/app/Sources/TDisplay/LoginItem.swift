// Start at login, the counterpart of the Run key on Windows, through SMAppService (macOS 13 and newer). It can only
// register a real app bundle, so it isn't offered in the development build, where the program runs outside one.
import ServiceManagement

enum LoginItem {
    static var available: Bool { Bundle.main.bundleURL.pathExtension == "app" }
    static var status: SMAppService.Status { SMAppService.mainApp.status }

    static func toggle() {
        do {
            if status == .enabled || status == .requiresApproval {
                try SMAppService.mainApp.unregister()
                logLine("login item: off")
            } else {
                try SMAppService.mainApp.register()
                logLine("login item: on")
            }
        } catch {
            logLine("login item: \(error.localizedDescription)")
        }
        // macOS may want the user to allow it first, under General > Login Items.
        if status == .requiresApproval { SMAppService.openSystemSettingsLoginItems() }
    }
}
