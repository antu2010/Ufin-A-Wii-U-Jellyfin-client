// Host stand-in for wut's <nsysnet/nssl.h>: same names and signatures (taken from
// wut's header), implemented by stubs/fake_nssl.cpp as a plain pass-through over a
// socket. Only what src/https_client.cpp uses.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t NSSLError;
typedef int32_t NSSLContextHandle;
typedef int32_t NSSLConnectionHandle;

enum {
    NSSL_ERROR_OK = 0,
    NSSL_ERROR_GENERIC = -1,
    NSSL_ERROR_WANT_READ = -0x280007,
    NSSL_ERROR_WANT_WRITE = -0x280008,
    NSSL_ERROR_HANDSHAKE_ERROR = -0x28000e,
    NSSL_ERROR_NOT_READY = -0x280028,
};

typedef enum NSSLServerCertId {
    NSSL_SERVER_CERT_NINTENDO_CA = 100,
    NSSL_SERVER_CERT_GLOBALSIGN_ROOT_CA = 1004,
    NSSL_SERVER_CERT_DIGICERT_GLOBAL_ROOT_CA_G2 = 1033,
    NSSL_SERVER_CERT_COMODO_RSA_CA = 1900,
    NSSL_SERVER_CERT_USERTRUST_RSA_CA = 1901,
} NSSLServerCertId;

NSSLError NSSLInit();
NSSLError NSSLFinish();
NSSLContextHandle NSSLCreateContext(int32_t unk);
NSSLError NSSLDestroyContext(NSSLContextHandle context);
NSSLError NSSLAddServerPKIExternal(NSSLContextHandle context, const void* cert, int32_t length, int32_t unk);
NSSLError NSSLAddServerPKI(NSSLContextHandle context, NSSLServerCertId pki);
NSSLConnectionHandle NSSLCreateConnection(NSSLContextHandle context, const char* host, int32_t hostLength, int32_t options,
                                          int32_t socket, int32_t block);
NSSLError NSSLDestroyConnection(NSSLConnectionHandle connection);
NSSLError NSSLRead(NSSLConnectionHandle connection, const void* buffer, int32_t length, int32_t* outBytesRead);
NSSLError NSSLWrite(NSSLConnectionHandle connection, const void* buffer, int32_t length, int32_t* outBytesWritten);

#ifdef __cplusplus
}
#endif
