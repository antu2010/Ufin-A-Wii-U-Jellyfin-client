# Mbed TLS (vendored)

Mbed TLS 2.28.9 (https://github.com/Mbed-TLS/mbedtls, Apache-2.0 OR GPL-2.0-or-later, see LICENSE),
trimmed to what Ufin's TLS 1.2 client needs (`src/https_client.cpp`):

* `library/*.c` and the headers they include are unmodified upstream files, only the modules listed in
  `include/mbedtls/config.h` are kept (no net_sockets, file I/O, threading, TLS server, PSA, DTLS ...).
* `include/mbedtls/config.h` is **Ufin's own** and replaces upstream's stock one: it is the whole
  configuration. There is no `-D MBEDTLS_CONFIG_FILE` anywhere, the default name is used.
* The root certificates it trusts are in `src/tls_roots.cpp` (generated, `tools/make_roots.py`).

To update: unpack a new 2.28.x release, copy `library/*.c` (same file list) and the headers they need over
these, keep `config.h`, and run `tests/host/run_tests.sh` (it builds this library for the host and talks TLS to
a real `openssl s_server`).
