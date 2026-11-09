// Plain blocking TCP sockets, as host/src/net.cpp has them.
import Darwin

typealias Socket = Int32

func connectLoopback(port: Int) -> Socket? {
    let s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
    guard s >= 0 else { return nil }
    var address = sockaddr_in()
    address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    address.sin_family = sa_family_t(AF_INET)
    address.sin_port = in_port_t(UInt16(port).bigEndian)
    address.sin_addr.s_addr = INADDR_LOOPBACK.bigEndian
    let result = withUnsafePointer(to: &address) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
    }
    guard result == 0 else {
        close(s)
        return nil
    }
    // A closed connection should be an error to handle, not a signal that ends the program.
    var on: Int32 = 1
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
    return s
}

func sendAll(_ s: Socket, _ data: UnsafeRawBufferPointer) -> Bool {
    var offset = 0
    while offset < data.count {
        let n = send(s, data.baseAddress! + offset, data.count - offset, 0)
        if n < 0, errno == EINTR { continue }
        if n <= 0 { return false }
        offset += n
    }
    return true
}

func sendAll(_ s: Socket, _ bytes: [UInt8]) -> Bool { bytes.withUnsafeBytes { sendAll(s, $0) } }

func recvAll(_ s: Socket, _ buffer: UnsafeMutableRawBufferPointer) -> Bool {
    var offset = 0
    while offset < buffer.count {
        let n = recv(s, buffer.baseAddress! + offset, buffer.count - offset, 0)
        if n < 0, errno == EINTR { continue }
        if n <= 0 { return false }
        offset += n
    }
    return true
}

func recvBytes(_ s: Socket, _ count: Int) -> [UInt8]? {
    var bytes = [UInt8](repeating: 0, count: count)
    let ok = bytes.withUnsafeMutableBytes { recvAll(s, $0) }
    return ok ? bytes : nil
}

// Sends several buffers as one write, so one message reaches adb as one packet.
func sendGather(_ s: Socket, _ buffers: [UnsafeRawBufferPointer]) -> Bool {
    var vectors = buffers.filter { $0.count > 0 }.map { iovec(iov_base: UnsafeMutableRawPointer(mutating: $0.baseAddress), iov_len: $0.count) }
    var first = 0
    while first < vectors.count {
        let n = vectors[first...].withUnsafeBufferPointer { writev(s, $0.baseAddress, Int32($0.count)) }
        if n < 0, errno == EINTR { continue }
        if n <= 0 { return false }
        var sent = n
        while first < vectors.count, sent >= vectors[first].iov_len {
            sent -= vectors[first].iov_len
            first += 1
        }
        if first < vectors.count {
            vectors[first].iov_base = vectors[first].iov_base! + sent
            vectors[first].iov_len -= sent
        }
    }
    return true
}

func setTimeouts(_ s: Socket, ms: Int) {
    var value = timeval(tv_sec: ms / 1000, tv_usec: Int32(ms % 1000 * 1000))
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, socklen_t(MemoryLayout<timeval>.size))
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, socklen_t(MemoryLayout<timeval>.size))
}

func setNoDelay(_ s: Socket) {
    var on: Int32 = 1
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, socklen_t(MemoryLayout<Int32>.size))
}

func setSendBuffer(_ s: Socket, bytes: Int) {
    var size = Int32(bytes)
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout<Int32>.size))
}
