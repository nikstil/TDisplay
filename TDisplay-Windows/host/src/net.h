#pragma once
#include <winsock2.h>

#include <cstddef>

bool sendAll(SOCKET s, const void* data, size_t length);
bool recvAll(SOCKET s, void* data, size_t length);

// Sends several buffers as one write, so one message reaches adb as one packet.
bool sendGather(SOCKET s, WSABUF* buffers, DWORD count);

void setRecvTimeout(SOCKET s, int milliseconds);
void setSendTimeout(SOCKET s, int milliseconds);
void setNoDelay(SOCKET s);
