#include "net.h"

#include <ws2tcpip.h>

bool sendAll(SOCKET s, const void* data, size_t length) {
    const char* p = static_cast<const char*>(data);
    while (length > 0) {
        int n = send(s, p, static_cast<int>(length), 0);
        if (n <= 0) return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(SOCKET s, void* data, size_t length) {
    char* p = static_cast<char*>(data);
    while (length > 0) {
        int n = recv(s, p, static_cast<int>(length), 0);
        if (n <= 0) return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

bool sendGather(SOCKET s, WSABUF* buffers, DWORD count) {
    while (count > 0) {
        DWORD sent = 0;
        if (WSASend(s, buffers, count, &sent, 0, nullptr, nullptr) != 0) return false;
        while (count > 0 && sent >= buffers->len) {
            sent -= buffers->len;
            ++buffers;
            --count;
        }
        if (count > 0) {
            buffers->buf += sent;
            buffers->len -= sent;
        }
    }
    return true;
}

void setRecvTimeout(SOCKET s, int milliseconds) {
    DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}

void setSendTimeout(SOCKET s, int milliseconds) {
    DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}

void setNoDelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}
