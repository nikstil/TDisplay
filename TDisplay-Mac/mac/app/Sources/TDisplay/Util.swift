// Logging, the clock, small threading helpers and where things live.
import Foundation
import os

// Writes timestamped lines to ~/Library/Logs/TDisplay/host.log, in the same format as Windows. With TDISPLAY_CONSOLE set
// in the environment, lines go to standard output as well.
enum Log {
    static let directory = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/TDisplay")
    private static let lock = NSLock()
    nonisolated(unsafe) private static var file: UnsafeMutablePointer<FILE>?
    private static let maxBytes = 8 * 1024 * 1024
    private static let console = ProcessInfo.processInfo.environment["TDISPLAY_CONSOLE"] != nil

    private static func open() {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let path = directory.appendingPathComponent("host.log").path
        file = fopen(path, "a")
        guard let current = file else { return }
        // Past the size limit, keep one previous file and start fresh.
        if ftell(current) > maxBytes {
            fclose(current)
            _ = try? FileManager.default.removeItem(atPath: path + ".1")
            try? FileManager.default.moveItem(atPath: path, toPath: path + ".1")
            file = fopen(path, "a")
        }
    }

    static func line(_ text: String) {
        var now = timeval()
        gettimeofday(&now, nil)
        var seconds = now.tv_sec
        var local = tm()
        localtime_r(&seconds, &local)
        let stamp = String(format: "%04d-%02d-%02d %02d:%02d:%02d.%03d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                           local.tm_hour, local.tm_min, local.tm_sec, Int(now.tv_usec) / 1000)
        lock.lock()
        defer { lock.unlock() }
        if file == nil { open() }
        if let file {
            fputs("\(stamp) \(text)\n", file)
            fflush(file)
        }
        if console {
            print("\(stamp) \(text)")
            fflush(stdout)
        }
    }
}

func logLine(_ text: String) { Log.line(text) }

private let timebase: mach_timebase_info_data_t = {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    return info
}()

// Microseconds on the mach_absolute_time clock, which keeps counting through sleep only as far as the CPU does.
func nowUs() -> UInt64 { mach_absolute_time() * UInt64(timebase.numer) / UInt64(timebase.denom) / 1000 }

func sleepMs(_ ms: Int) { usleep(useconds_t(ms * 1000)) }

// Time since an event another thread recorded, which may be slightly newer than the "now" in hand.
func since(_ now: UInt64, _ then: UInt64) -> UInt64 { now > then ? now - then : 0 }

// A value several threads read and write, the counterpart of std::atomic in the Windows code.
final class Shared<Value: Sendable>: @unchecked Sendable {
    private let state: OSAllocatedUnfairLock<Value>

    init(_ value: Value) { state = OSAllocatedUnfairLock(initialState: value) }

    func get() -> Value { state.withLock { $0 } }
    func put(_ value: Value) { state.withLock { $0 = value } }

    // Sets the value and returns the one it replaced.
    func exchange(_ value: Value) -> Value {
        state.withLock {
            let old = $0
            $0 = value
            return old
        }
    }
}

// A Windows-style auto-reset event: set wakes one wait, and sets with nobody waiting collapse into one.
final class Event: @unchecked Sendable {
    private let condition = NSCondition()
    private var signaled = false

    func set() {
        condition.lock()
        signaled = true
        condition.signal()
        condition.unlock()
    }

    // True if it was set, false on timeout.
    @discardableResult
    func wait(ms: Int) -> Bool {
        condition.lock()
        defer { condition.unlock() }
        let deadline = Date(timeIntervalSinceNow: Double(ms) / 1000)
        while !signaled {
            if !condition.wait(until: deadline) { break }
        }
        let was = signaled
        signaled = false
        return was
    }
}

enum Paths {
    // The tablet app sits in the bundle's Resources once packaged. Until then, it and adb are found by walking up from the
    // executable to the project: mac/tools/platform-tools/adb and dist/TDisplay.apk.
    static func resource(_ name: String, project: String) -> String {
        if let bundled = Bundle.main.resourceURL?.appendingPathComponent(name).path, FileManager.default.fileExists(atPath: bundled) { return bundled }
        var directory = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        for _ in 0..<8 {
            let candidate = directory.appendingPathComponent(project).path
            if FileManager.default.fileExists(atPath: candidate) { return candidate }
            directory.deleteLastPathComponent()
        }
        return project
    }

    // In the packaged app, adb sits beside the program in Contents/MacOS.
    static var adb: String { Bundle.main.url(forAuxiliaryExecutable: "adb")?.path ?? resource("adb", project: "mac/tools/platform-tools/adb") }
    static var apk: String { resource("TDisplay.apk", project: "dist/TDisplay.apk") }
    static let support = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/TDisplay")
}
