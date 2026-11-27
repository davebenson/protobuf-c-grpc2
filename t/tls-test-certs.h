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

#endif
