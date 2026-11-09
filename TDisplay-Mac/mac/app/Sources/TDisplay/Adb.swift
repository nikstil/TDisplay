// Talks to a private adb server that only this program uses, as host/src/adb.cpp does.
//
// Running on our own port means other software on the Mac that bundles adb (Android Studio, emulators) can't kill our
// server by restarting theirs with a different version. Streams go straight through the server with no listening
// port, and only USB devices are ever selected.
import Foundation
import IOKit

struct AdbDevice {
    var serial: String
    var state: String  // "device", "unauthorized", "offline", ...
    var model: String
}

final class Adb {
    private static let controlTimeoutMs = 4000

    let exe: String
    let port: Int

    init(exe: String, port: Int) {
        self.exe = exe
        self.port = port
    }

    private func connectServer() -> Socket? {
        guard let s = connectLoopback(port: port) else { return nil }
        setTimeouts(s, ms: Adb.controlTimeoutMs)
        setNoDelay(s)
        return s
    }

    private func sendRequest(_ s: Socket, _ request: String) -> Bool {
        let body = Array(request.utf8)
        return sendAll(s, Array(String(format: "%04x", body.count).utf8) + body)
    }

    // Reads adb's four-byte status. On FAIL, error gets the message that follows.
    private func readStatus(_ s: Socket, error: inout String) -> Bool {
        guard let status = recvBytes(s, 4) else {
            error = "no reply from adb server"
            return false
        }
        if status == Array("OKAY".utf8) { return true }
        var message = ""
        if let length = recvBytes(s, 4), let n = Int(String(decoding: length, as: UTF8.self), radix: 16), n > 0, let text = recvBytes(s, n) {
            message = String(decoding: text, as: UTF8.self)
        }
        error = message.isEmpty ? "adb request failed" : message
        return false
    }

    private static func isUsbSerial(_ serial: String) -> Bool {
        !serial.contains(":") && !serial.hasPrefix("emulator-") && !serial.contains("_adb-tls-connect")
    }

    // Runs adb with these arguments. With wait false it doesn't wait and reports only the launch.
    @discardableResult
    private func runTool(_ arguments: [String], wait: Bool) -> (ok: Bool, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: exe)
        process.arguments = ["-P", String(port)] + arguments
        let pipe = Pipe()
        // The server that start-server forks would keep a pipe open for good, so it gets none.
        process.standardOutput = wait ? pipe : FileHandle.nullDevice
        process.standardError = wait ? pipe : FileHandle.nullDevice
        process.standardInput = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            logLine("adb: could not run \(exe) (\(error.localizedDescription))")
            return (false, "")
        }
        guard wait else { return (true, "") }
        let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        return (process.terminationStatus == 0, output)
    }

    // Starts the server if it isn't already answering.
    func ensureServer() -> Bool {
        if let probe = connectServer() {
            close(probe)
            return true
        }
        guard runTool(["start-server"], wait: false).ok else { return false }
        // The start-server command itself takes seconds to report back, well after the server is listening, so watch
        // for the port rather than waiting on the command.
        let deadline = nowUs() + 15_000_000
        while nowUs() < deadline {
            if let probe = connectServer() {
                close(probe)
                logLine("adb: server started on port \(port)")
                return true
            }
            sleepMs(50)
        }
        logLine("adb: server did not come up on port \(port)")
        return false
    }

    // Stops the server. The next ensureServer starts a fresh one, which looks at USB from scratch.
    func killServer() {
        logLine("adb: restarting the server")
        guard let s = connectServer() else { return }
        _ = sendRequest(s, "host:kill")
        _ = recvBytes(s, 4)
        close(s)
    }

    // True if macOS currently sees an adb USB interface, whether or not our server has it.
    static func usbInterfacePresent() -> Bool {
        let matching = IOServiceMatching("IOUSBHostInterface") as NSMutableDictionary
        // The interface class, subclass and protocol every Android device's adb function has.
        matching[kIOPropertyMatchKey] = ["bInterfaceClass": 0xFF, "bInterfaceSubClass": 0x42, "bInterfaceProtocol": 0x01]
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(iterator) }
        let first = IOIteratorNext(iterator)
        guard first != 0 else { return false }
        IOObjectRelease(first)
        return true
    }

    // Devices attached by cable. Wireless debugging devices are left out. Nil if the server didn't answer.
    func listUsbDevices() -> [AdbDevice]? {
        guard let s = connectServer() else { return nil }
        defer { close(s) }
        var error = ""
        guard sendRequest(s, "host:devices-l"), readStatus(s, error: &error), let length = recvBytes(s, 4),
              let n = Int(String(decoding: length, as: UTF8.self), radix: 16) else { return nil }
        guard let text = n > 0 ? recvBytes(s, n) : [] else { return nil }
        var devices: [AdbDevice] = []
        for line in String(decoding: text, as: UTF8.self).split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard fields.count >= 2, Adb.isUsbSerial(fields[0]) else { continue }
            let model = fields.first { $0.hasPrefix("model:") }.map { String($0.dropFirst(6)) } ?? ""
            devices.append(AdbDevice(serial: fields[0], state: fields[1], model: model))
        }
        return devices
    }

    // Opens a stream to any service on the device, such as "shell:ls". On failure, error gets adb's reason.
    func openService(serial: String, service: String, error: inout String) -> Socket? {
        guard let s = connectServer() else {
            error = "adb server not reachable"
            return nil
        }
        guard sendRequest(s, "host:transport:" + serial), readStatus(s, error: &error), sendRequest(s, service), readStatus(s, error: &error) else {
            close(s)
            return nil
        }
        return s
    }

    // Opens a stream to an abstract Unix socket on the device.
    func openAbstract(serial: String, name: String, error: inout String) -> Socket? {
        openService(serial: serial, service: "localabstract:" + name, error: &error)
    }

    // Runs a shell command on the device and returns what it printed, or nil if it couldn't run.
    @discardableResult
    func shell(serial: String, _ command: String) -> String? {
        var error = ""
        guard let s = openService(serial: serial, service: "shell:" + command, error: &error) else {
            logLine("adb: shell failed: \(error)")
            return nil
        }
        defer { close(s) }
        var output = [UInt8]()
        var buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = buffer.withUnsafeMutableBytes { recv(s, $0.baseAddress, $0.count, 0) }
            if n <= 0 { break }
            output.append(contentsOf: buffer[0..<n])
        }
        return String(decoding: output, as: UTF8.self)
    }

    // Installs or updates an app on the device. On failure, reason gets the error adb reported.
    func install(serial: String, apk: String, reason: inout String) -> Bool {
        let (ok, output) = runTool(["-s", serial, "install", "-r", apk], wait: true)
        logLine("adb: app install \(ok ? "succeeded" : "failed"): \(output.trimmingCharacters(in: .whitespacesAndNewlines))")
        if !ok { reason = output }
        return ok
    }
}
