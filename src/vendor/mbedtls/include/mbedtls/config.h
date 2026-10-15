/* Ufin's Mbed TLS configuration. This file REPLACES the stock include/mbedtls/config.h of Mbed TLS 2.28.9
 * (see ../../VENDORED.md): a TLS 1.2 *client* that checks a server certificate against a
 * built-in root list (src/tls_roots.cpp) -- nothing else. Everything not named here is off, which keeps
 * the code small and keeps it from touching files, threads, the clock-as-RNG or a network stack. */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* system */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY   /* no /dev/urandom here: src/https_client.cpp feeds the entropy pool */
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_NO_64BIT_MULTIPLICATION

/* crypto */
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C               /* some older roots and intermediates are still signed with SHA-1 */
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C

/* TLS 1.2 client */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

/* X.509: parse and verify certificates */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#include "mbedtls/check_config.h"
#endif
