import Foundation

// What config.ini can change, with the same keys and defaults as Windows. It's read at the start of every session from
// ~/Library/Application Support/TDisplay/config.ini, under [stream]. Every line is optional.
struct StreamSettings {
    var maxRefreshHz = 240
    var streamFps = 0  // Frames per second to send. 0 means the same as the refresh rate.
    var bitrate = 80_000_000
    var codec: UInt32?  // Proto.codecHevc or Proto.codecH264; nil is HEVC, falling back to H.264.
    var minQp = 22
    var maxFramesInFlight = 0  // 0 chooses from the refresh rate.
    var penCurvePercent = 50  // Pressure curve exponent, in hundredths. 100 is a straight line.
    var penMinimumPercent = 6  // What the lightest pen contact registers as, in percent.

    static var path: URL { Paths.support.appendingPathComponent("config.ini") }

    static func load() -> StreamSettings {
        var s = StreamSettings()
        guard let text = try? String(contentsOf: path, encoding: .utf8) else { return s }
        var section = ""
        var values: [String: String] = [:]
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.split(separator: ";", maxSplits: 1, omittingEmptySubsequences: false)[0].trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("[") {
                section = line.trimmingCharacters(in: CharacterSet(charactersIn: "[]")).lowercased()
            } else if section == "stream", let equals = line.firstIndex(of: "=") {
                values[line[..<equals].trimmingCharacters(in: .whitespaces).lowercased()] = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            }
        }
        let number = { (key: String, fallback: Int) in values[key].flatMap { Int($0) } ?? fallback }
        s.maxRefreshHz = number("refresh", s.maxRefreshHz)
        s.streamFps = number("stream_fps", s.streamFps)
        s.bitrate = number("bitrate_mbps", s.bitrate / 1_000_000) * 1_000_000
        switch values["codec"]?.lowercased() {
        case "hevc": s.codec = Proto.codecHevc
        case "h264": s.codec = Proto.codecH264
        default: break
        }
        s.minQp = number("min_qp", s.minQp)
        s.maxFramesInFlight = number("max_frames_in_flight", s.maxFramesInFlight)
        s.penCurvePercent = number("pen_pressure_curve", s.penCurvePercent)
        s.penMinimumPercent = number("pen_pressure_minimum", s.penMinimumPercent)
        return s
    }
}
