// TDisplay for macOS: a menu bar item around the background service (the tray icon on Windows).
import AppKit

final class StatusMenu: NSObject, NSMenuDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private var text = "Starting"
    private var state = LinkState.waiting

    override init() {
        super.init()
        update(.waiting, "Starting")
    }

    func update(_ state: LinkState, _ text: String) {
        self.state = state
        self.text = text
        item.button?.image = StatusMenu.icon(state)
        item.button?.toolTip = "TDisplay: \(text)"
        if item.menu == nil {
            item.menu = NSMenu()
            item.menu?.delegate = self
        }
    }

    // Filled in as it opens, so the login item's state is current even if it was changed in System Settings.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let status = NSMenuItem(title: text, action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)
        menu.addItem(.separator())
        let screen = NSMenuItem(title: "Tablet screen", action: nil, keyEquivalent: "")
        let choices = NSMenu()
        for arrangement in Arrangement.allCases {
            if arrangement == .mirror { choices.addItem(.separator()) }
            let choice = NSMenuItem(title: arrangement.title, action: #selector(arrange(_:)), keyEquivalent: "")
            choice.target = self
            choice.representedObject = arrangement.rawValue
            choice.state = arrangement == Arrangement.current ? .on : .off
            choices.addItem(choice)
        }
        screen.submenu = choices
        menu.addItem(screen)
        let touch = NSMenuItem(title: "Touch gestures", action: #selector(toggleGestures), keyEquivalent: "")
        touch.target = self
        touch.state = TouchGestures.enabled ? .on : .off
        menu.addItem(touch)
        let login = NSMenuItem(title: "Start at login", action: #selector(toggleLogin), keyEquivalent: "")
        login.target = self
        if !LoginItem.available {
            login.title = "Start at login (in the packaged app)"
            login.action = nil
        } else if LoginItem.status == .requiresApproval {
            login.title = "Start at login (allow it in System Settings)"
            login.state = .mixed
        } else {
            login.state = LoginItem.status == .enabled ? .on : .off
        }
        menu.addItem(login)
        menu.addItem(withTitle: "Open log folder", action: #selector(openLogs), keyEquivalent: "").target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit TDisplay", action: #selector(quit), keyEquivalent: "q").target = self
    }

    @objc private func toggleLogin() { LoginItem.toggle() }

    @objc private func toggleGestures() {
        TouchGestures.enabled.toggle()
        logLine("touch gestures: \(TouchGestures.enabled ? "on" : "off")")
    }

    @objc private func arrange(_ item: NSMenuItem) {
        guard let arrangement = (item.representedObject as? String).flatMap(Arrangement.init), arrangement != Arrangement.current else { return }
        Arrangement.current = arrangement
        service.arrangementChanged()
    }

    @objc private func openLogs() {
        try? FileManager.default.createDirectory(at: Log.directory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Log.directory)
    }

    @objc private func quit() { NSApp.terminate(nil) }

    // A small tablet, grey while waiting, green when connected and orange if something needs attention.
    private static func icon(_ state: LinkState) -> NSImage {
        let color: NSColor = switch state {
        case .waiting: NSColor(srgbRed: 150 / 255, green: 150 / 255, blue: 150 / 255, alpha: 1)
        case .streaming: NSColor(srgbRed: 52 / 255, green: 199 / 255, blue: 89 / 255, alpha: 1)
        case .problem: NSColor(srgbRed: 1, green: 159 / 255, blue: 10 / 255, alpha: 1)
        }
        return NSImage(size: NSSize(width: 18, height: 18), flipped: false) { _ in
            color.setFill()
            NSBezierPath(roundedRect: NSRect(x: 1, y: 3.5, width: 16, height: 11), xRadius: 2.5, yRadius: 2.5).fill()
            NSColor(white: 0.08, alpha: 1).setFill()
            NSRect(x: 2.75, y: 5.25, width: 12.5, height: 7.5).fill()
            return true
        }
    }
}

signal(SIGPIPE, SIG_IGN)

// TDisplay --place id x y: moves a display for VirtualDisplay.place, from a process of its own.
if CommandLine.arguments.count == 5, CommandLine.arguments[1] == "--place", let id = UInt32(CommandLine.arguments[2]),
   let x = Int32(CommandLine.arguments[3]), let y = Int32(CommandLine.arguments[4]) {
    exit(VirtualDisplay.move(id, x: x, y: y) ? 0 : 1)
}

if CommandLine.arguments.contains("--gesture-test") {
    let test = GestureTest()
    NSApplication.shared.setActivationPolicy(.regular)
    DispatchQueue.main.async { test.start() }
    NSApplication.shared.run()
}

if CommandLine.arguments.contains("--pen-test") {
    let test = PenTest()
    NSApplication.shared.setActivationPolicy(.regular)
    DispatchQueue.main.async { test.start() }
    NSApplication.shared.run()
}

// One copy at a time: a second would fight the first over the tablet.
try? FileManager.default.createDirectory(at: Paths.support, withIntermediateDirectories: true)
let lock = open(Paths.support.appendingPathComponent("host.lock").path, O_CREAT | O_RDWR, 0o600)
if lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0 { exit(0) }

logLine("==== TDisplay host starting ====")
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let menu = StatusMenu()
let service = Service { state, text in
    DispatchQueue.main.async { menu.update(state, text) }
}
service.start()

// Sessions end as the Mac goes to sleep and start again as it wakes.
let power = NSWorkspace.shared.notificationCenter
power.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: nil) { _ in service.sleep() }
power.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: nil) { _ in service.woke() }
power.addObserver(forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: nil) { _ in service.woke() }

// Quitting from the menu, logging out and being told to stop all end the same way, with the display taken away. The
// service stops on another thread, because a display is only removed while the main thread is free to see it go.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        DispatchQueue.global(qos: .userInitiated).async {
            service.stop()
            logLine("==== TDisplay host stopped ====")
            // AppKit waits for the reply in the modal panel run loop mode, where the main dispatch queue isn't served.
            RunLoop.main.perform(inModes: [.modalPanel, .default]) { NSApp.reply(toApplicationShouldTerminate: true) }
            CFRunLoopWakeUp(CFRunLoopGetMain())
        }
        return .terminateLater
    }
}
let delegate = AppDelegate()
app.delegate = delegate
var stopSignals: [DispatchSourceSignal] = []
for number in [SIGTERM, SIGINT] {
    signal(number, SIG_IGN)
    let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
    source.setEventHandler { NSApp.terminate(nil) }
    source.resume()
    stopSignals.append(source)
}
app.run()
