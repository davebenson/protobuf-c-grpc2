/*
 * Certificates and keys for the TLS tests. TEST ONLY: the private keys are
 * public. Two unrelated self-signed ECDSA P-256 certificates for
 * CN=localhost / DNS:localhost, valid from 2020 to 2119.
 */
#ifndef TLS_TEST_CERTS_H
#define TLS_TEST_CERTS_H

static const char test_server_cert_pem[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBlTCCATugAwIBAgIUVC5wg29OxlVpC4Lg9LqXuc20dF0wCgYIKoZIzj0EAwIw\n"
	"FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTIwMDEwMTAwMDAwMFoYDzIxMTkxMjMx\n"
	"MjM1OTU5WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
	"PQMBBwNCAATJkwXk70Po5Nbll0oYUi9/Oy6oCMU8jgtbJL4QmTBTZ+L8OjdUY+BG\n"
	"p60H+pxTiKMQ8abqRD+5I9S4Mh7Mwwtno2kwZzAdBgNVHQ4EFgQUxuJyTY5HCkgk\n"
	"HO1hZvfdNXsOMBMwHwYDVR0jBBgwFoAUxuJyTY5HCkgkHO1hZvfdNXsOMBMwDwYD\n"
	"VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID\n"
	"SAAwRQIhAMQ4zc1LVIWO2SUL/jw01FXufydxrdYfZidyxoQUFpf4AiAXqdaOJ66Y\n"
	"HTTb79jGwKttjLRgU5BZARFP0f2vcfBhgA==\n"
	"-----END CERTIFICATE-----\n";

static const char test_server_key_pem[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZjxqbpGBWjXl+Px5\n"
	"yDPIhh03jb+NitrNwV33Y/Ikr46hRANCAATJkwXk70Po5Nbll0oYUi9/Oy6oCMU8\n"
	"jgtbJL4QmTBTZ+L8OjdUY+BGp60H+pxTiKMQ8abqRD+5I9S4Mh7Mwwtn\n"
	"-----END PRIVATE KEY-----\n";

/* a certificate the client does not expect */
static const char test_other_cert_pem[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBljCCATugAwIBAgIUBeM9PQE9xk9Udt8+2ixnASKkKqcwCgYIKoZIzj0EAwIw\n"
	"FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTIwMDEwMTAwMDAwMFoYDzIxMTkxMjMx\n"
	"MjM1OTU5WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
	"PQMBBwNCAASX5Cx3GTBkD+7AIgWwrPyFvgQyJSg9cafksBOmDgOxRdF1dAk4mwBC\n"
	"rNxwfDp9JAPgVwW+o4GkBHg9vwR31dX9o2kwZzAdBgNVHQ4EFgQU+GhW6545aYOb\n"
	"EWgnSU3d1KQAiOgwHwYDVR0jBBgwFoAU+GhW6545aYObEWgnSU3d1KQAiOgwDwYD\n"
	"VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID\n"
	"SQAwRgIhAKO4ASyefIvB0LVmVfQ+9G8Ns+bRecOfu/bhbXAzO9rhAiEA+iLSKcOu\n"
	"jl4GkVW6yR57ZODIgjiq5wcKBmOhsiiw2LY=\n"
	"-----END CERTIFICATE-----\n";

/* ---- mutual TLS: client certificates ----
 * Three self-signed ECDSA P-256 certificates for clients (valid for 100 years
 * from 2026). The server in the mTLS tests trusts the first two:
 *   A: CN=test-client, SAN URI spiffe://example.org/test-client,
 *      DNS client.example.org, email client@example.org
 *   B: CN=cn-only-client, no extensions
 *   C: CN=intruder (not trusted by the server)
 * The identity the transports derive from them is in the macros below. */
static const char test_client_A_cert_pem[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIB2jCCAYCgAwIBAgIUOmTEPf9P8ORV4JfUB1TbB8447g0wCgYIKoZIzj0EAwIw\n"
	"FjEUMBIGA1UEAwwLdGVzdC1jbGllbnQwIBcNMjYxMTI4MDMwNDAzWhgPMjEyNjEx\n"
	"MDQwMzA0MDNaMBYxFDASBgNVBAMMC3Rlc3QtY2xpZW50MFkwEwYHKoZIzj0CAQYI\n"
	"KoZIzj0DAQcDQgAEpunHk34cuKpD6SJegyTndrQKvzCmpsC0IWyAlb821pJEKcC1\n"
	"21Jr2uConbrS5Zpw4nFSiaZR8IimmcGFp2o336OBqTCBpjAdBgNVHQ4EFgQULgk8\n"
	"wbdcsyiZe7CFAUwaKIGaV1owHwYDVR0jBBgwFoAULgk8wbdcsyiZe7CFAUwaKIGa\n"
	"V1owDwYDVR0TAQH/BAUwAwEB/zBTBgNVHREETDBKhiBzcGlmZmU6Ly9leGFtcGxl\n"
	"Lm9yZy90ZXN0LWNsaWVudIISY2xpZW50LmV4YW1wbGUub3JngRJjbGllbnRAZXhh\n"
	"bXBsZS5vcmcwCgYIKoZIzj0EAwIDSAAwRQIhAJ5yOAh4dt34sFVbjVosGTYNXdYA\n"
	"oidAgaYnY8YvzuJ7AiAPlASI3/BbGu4hqr4G4txAP+rf5DW88+7vLz3EfZ6flA==\n"
	"-----END CERTIFICATE-----\n";

static const char test_client_A_key_pem[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgy0KH3aUPxlJP/+wK\n"
	"hn8W4kU+hwQ+p3wPyWNDm7Wo6/mhRANCAASm6ceTfhy4qkPpIl6DJOd2tAq/MKam\n"
	"wLQhbICVvzbWkkQpwLXbUmva4KidutLlmnDicVKJplHwiKaZwYWnajff\n"
	"-----END PRIVATE KEY-----\n";

static const char test_client_B_cert_pem[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBiTCCAS+gAwIBAgIULKEPkVdnBlFX9nwHrxIXhPmg3YUwCgYIKoZIzj0EAwIw\n"
	"GTEXMBUGA1UEAwwOY24tb25seS1jbGllbnQwIBcNMjYxMTI4MDMwNDAzWhgPMjEy\n"
	"NjExMDQwMzA0MDNaMBkxFzAVBgNVBAMMDmNuLW9ubHktY2xpZW50MFkwEwYHKoZI\n"
	"zj0CAQYIKoZIzj0DAQcDQgAETsfa1pmxoAvvV+Ac54YgeDJM4VU52eR2nidX+WNP\n"
	"PLaQbd8T7atUkTUM/VwSB/PFwXyxxHJOPKBaai6D0M61UaNTMFEwHQYDVR0OBBYE\n"
	"FFmGC7VD7MegOznpNYfiuXKR5hfYMB8GA1UdIwQYMBaAFFmGC7VD7MegOznpNYfi\n"
	"uXKR5hfYMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgf+LiUml8\n"
	"a7qeNpPRJYwPJIjTozCNp2JAdcK8He2OP2sCIQDKEepkMFwebv22JLggsoomQ7HR\n"
	"I/vKdj0OqP79jD4+0w==\n"
	"-----END CERTIFICATE-----\n";

static const char test_client_B_key_pem[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgLgO23xCs4SVP78rS\n"
	"cf+AhbWAyGY9uYhpWT7Po8eSBI6hRANCAAROx9rWmbGgC+9X4BznhiB4MkzhVTnZ\n"
	"5HaeJ1f5Y088tpBt3xPtq1SRNQz9XBIH88XBfLHEck48oFpqLoPQzrVR\n"
	"-----END PRIVATE KEY-----\n";

static const char test_client_C_cert_pem[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBpzCCAU2gAwIBAgIUflOA9QHNODMb10JdQE34rMovYnYwCgYIKoZIzj0EAwIw\n"
	"EzERMA8GA1UEAwwIaW50cnVkZXIwIBcNMjYxMTI4MDMwNDAzWhgPMjEyNjExMDQw\n"
	"MzA0MDNaMBMxETAPBgNVBAMMCGludHJ1ZGVyMFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
	"AQcDQgAEmkSV7LLMcmfXaEHdt6L4ToyKPOvnSBg0h4+Np9sj6PrHltPoEo1ftDQO\n"
	"eh/D8jwPnjP/U9/8BYhnBw6iYmopgaN9MHswHQYDVR0OBBYEFGo7lil7+JC2hxFW\n"
	"HC7d8arXxS18MB8GA1UdIwQYMBaAFGo7lil7+JC2hxFWHC7d8arXxS18MA8GA1Ud\n"
	"EwEB/wQFMAMBAf8wKAYDVR0RBCEwH4Ydc3BpZmZlOi8vZXhhbXBsZS5vcmcvaW50\n"
	"cnVkZXIwCgYIKoZIzj0EAwIDSAAwRQIgNveYBPUt4j/MLDZOveGdoqtkfsJUGZqr\n"
	"JateriuB9YMCIQDTNv75WFQGyiRif1AY2Ejr31VnpYcedXrnUQ4tiTii2A==\n"
	"-----END CERTIFICATE-----\n";

static const char test_client_C_key_pem[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgVAxprfaP7Q6ZDTo7\n"
	"4bchvKVRknBKn2pHrMe8iC/a5HKhRANCAASaRJXsssxyZ9doQd23ovhOjIo86+dI\n"
	"GDSHj42n2yPo+seW0+gSjV+0NA56H8PyPA+eM/9T3/wFiGcHDqJiaimB\n"
	"-----END PRIVATE KEY-----\n";

#define TEST_CLIENT_A_IDENTITY "spiffe://example.org/test-client"
#define TEST_CLIENT_A_SUBJECT "CN=test-client"
#define TEST_CLIENT_A_FINGERPRINT "a4b84b659b15b695618e2740c17d388a234805f73e0f29fd89962b2a81555de6"
#define TEST_CLIENT_B_IDENTITY "cn-only-client"

#endif
