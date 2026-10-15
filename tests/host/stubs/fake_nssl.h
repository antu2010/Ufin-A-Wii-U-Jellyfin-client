#pragma once
#include <string>

// Test-side controls and counters of stubs/fake_nssl.cpp.
struct FakeNsslControl {
    bool failInit = false;
    bool emulatorStub = false;            // every call "succeeds" with 0 (Cemu's unsupported-call behaviour)
    bool noRoots = false;                 // every NSSLAddServerPKI refused
    bool failOnGivenSocket = false;       // handshake fails when a socket is passed in
    bool closeSocketOnDestroy = false;    // NSSLDestroyConnection closes the socket (as it might)
    bool closesGivenSocketOnDestroy = false;
    bool wantWriteOnce = false;
    int fallbackFd = -1;                  // the connection NSSL "opens itself" (socket == 0)
    int wantReadEvery = 0;                // every Nth read answers WANT_READ
    int maxRead = 1 << 20;                // bytes per read (small values mean many reads)
    int maxWrite = 1 << 20;               // bytes per write (partial writes)
    // counters
    int initCalls = 0, rootsAsked = 0, rootsAdded = 0, createCalls = 0, ownSocketAttempts = 0, fallbackAttempts = 0,
        destroyCalls = 0, reads = 0;
    int handle = -1;
    std::string lastHost;
};
extern FakeNsslControl g_fakeNssl;
