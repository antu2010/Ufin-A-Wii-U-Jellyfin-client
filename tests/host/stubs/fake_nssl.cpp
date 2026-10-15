// Fake NSSL for the host: no encryption, just recv()/send() on the socket it was given,
// with knobs for the failures the real library can produce. See stubs/nsysnet/nssl.h.
#include <nsysnet/nssl.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "fake_nssl.h"

FakeNsslControl g_fakeNssl;

static bool g_inited = false;

extern "C" {

NSSLError NSSLInit() {
    g_fakeNssl.initCalls++;
    if (g_fakeNssl.failInit) return NSSL_ERROR_GENERIC;
    g_inited = true;
    return NSSL_ERROR_OK;
}
NSSLError NSSLFinish() { return NSSL_ERROR_OK; }
static int g_nextContext = 7;
NSSLContextHandle NSSLCreateContext(int32_t) {
    if (g_fakeNssl.emulatorStub) return 0;
    return g_inited ? g_nextContext++ : -1;
}
NSSLError NSSLDestroyContext(NSSLContextHandle) { return NSSL_ERROR_OK; }
NSSLError NSSLAddServerPKIExternal(NSSLContextHandle, const void*, int32_t, int32_t) { return NSSL_ERROR_OK; }
NSSLError NSSLAddServerPKI(NSSLContextHandle, NSSLServerCertId id) {
    g_fakeNssl.rootsAsked++;
    if (g_fakeNssl.noRoots) return NSSL_ERROR_GENERIC;
    // Some ids are unknown to a given firmware: refuse those, accept the rest.
    if ((int)id % 5 == 0) return NSSL_ERROR_GENERIC;
    g_fakeNssl.rootsAdded++;
    return NSSL_ERROR_OK;
}

NSSLConnectionHandle NSSLCreateConnection(NSSLContextHandle, const char* host, int32_t hostLength, int32_t, int32_t socket,
                                          int32_t) {
    g_fakeNssl.lastHost = std::string(host, (size_t)hostLength);
    g_fakeNssl.createCalls++;
    if (socket != 0) {
        g_fakeNssl.ownSocketAttempts++;
        if (g_fakeNssl.failOnGivenSocket) return NSSL_ERROR_HANDSHAKE_ERROR;
        g_fakeNssl.handle = socket;
        g_fakeNssl.closesGivenSocketOnDestroy = g_fakeNssl.closeSocketOnDestroy;
        return 1;
    }
    // "NSSL opens its own connection" mode.
    g_fakeNssl.fallbackAttempts++;
    if (g_fakeNssl.fallbackFd < 0) return NSSL_ERROR_HANDSHAKE_ERROR;
    g_fakeNssl.handle = g_fakeNssl.fallbackFd;
    return 2;
}
NSSLError NSSLDestroyConnection(NSSLConnectionHandle) {
    g_fakeNssl.destroyCalls++;
    if (g_fakeNssl.closeSocketOnDestroy) close(g_fakeNssl.handle);
    return NSSL_ERROR_OK;
}
NSSLError NSSLRead(NSSLConnectionHandle, const void* buffer, int32_t length, int32_t* out) {
    if (g_fakeNssl.wantReadEvery > 0 && (++g_fakeNssl.reads % g_fakeNssl.wantReadEvery) == 0) return NSSL_ERROR_WANT_READ;
    int32_t n = (int32_t)recv(g_fakeNssl.handle, (void*)buffer, (size_t)(length > g_fakeNssl.maxRead ? g_fakeNssl.maxRead : length), 0);
    if (n < 0) return NSSL_ERROR_GENERIC;
    *out = n;
    return NSSL_ERROR_OK; // n == 0: the peer closed
}
NSSLError NSSLWrite(NSSLConnectionHandle, const void* buffer, int32_t length, int32_t* out) {
    if (g_fakeNssl.wantWriteOnce) {
        g_fakeNssl.wantWriteOnce = false;
        return NSSL_ERROR_WANT_WRITE;
    }
    int32_t chunk = length > g_fakeNssl.maxWrite ? g_fakeNssl.maxWrite : length; // partial writes
    int32_t n = (int32_t)send(g_fakeNssl.handle, buffer, (size_t)chunk, MSG_NOSIGNAL);
    if (n < 0) return NSSL_ERROR_GENERIC;
    *out = n;
    return NSSL_ERROR_OK;
}

} // extern "C"
