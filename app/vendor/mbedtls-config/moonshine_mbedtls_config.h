/* What this app changes in Mbed TLS's default configuration (MBEDTLS_USER_CONFIG_FILE:
 * read after include/mbedtls/mbedtls_config.h, which is left as upstream ships it).
 *
 * The library is here for one job: HTTPS as a client, from cpp-httplib (vendor/httplib.h),
 * to a model API the player named and to the download server. So:
 *
 * TLS 1.2 only. Mbed TLS 3.6 does TLS 1.3 through PSA Crypto, whose key store is one
 * table for the whole process, safe from several threads only with MBEDTLS_THREADING_C --
 * and 3.6 has no threading layer for Windows but one the application writes
 * (MBEDTLS_THREADING_ALT). The window makes connections from more than one thread (an
 * update check, a download, a test of the API), and each cpp-httplib client carries its
 * own entropy and DRBG, so with TLS 1.2 on the classic crypto there is nothing shared to
 * guard. Every server this talks to accepts 1.2. To go to 1.3: remove the #undefs below,
 * turn on MBEDTLS_THREADING_C with a mutex for each platform, and call psa_crypto_init()
 * once before the first connection.
 *
 * No DTLS and no server: nothing here listens over TLS.
 */

#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED

#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_COOKIE_C

/* The AES tables as constants in the binary rather than filled in on first use: the fill
 * is a write to memory every thread shares, and 8 KB of table is not worth a race. */
#define MBEDTLS_AES_ROM_TABLES
