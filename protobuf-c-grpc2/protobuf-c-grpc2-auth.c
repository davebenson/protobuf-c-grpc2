/*
 * Ready-made authentication plugins: bearer-token credentials for clients and
 * a bearer-token authenticator for servers.
 */
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

/* ---- client: a fixed bearer token ---- */

typedef struct {
	ProtobufCGrpc2CallCredentials base;	/* must be first */
	int refs;
	int floating;
	char *value;			/* "Bearer <token>" */
} BearerCredentials;

static void
bearer_get(ProtobufCGrpc2CallCredentials *self, ProtobufCGrpc2IoSystem *io,
	   const ProtobufCGrpc2CallInfo *info,
	   ProtobufCGrpc2CredentialsDone done, void *done_data)
{
	BearerCredentials *b = (BearerCredentials *) self;
	ProtobufCGrpc2Header h;
	(void) io; (void) info;

	h.name = "authorization";
	h.value = b->value;
	done(done_data, PROTOBUF_C_GRPC2_OK, NULL, &h, 1);	/* at once */
}

static void
bearer_ref(ProtobufCGrpc2CallCredentials *self)
{
	BearerCredentials *b = (BearerCredentials *) self;

	if (b->floating)
		b->floating = 0;
	else
		b->refs++;
}

static void
bearer_destroy(ProtobufCGrpc2CallCredentials *self)
{
	BearerCredentials *b = (BearerCredentials *) self;

	if (--b->refs > 0)
		return;
	if (b->value != NULL) {
		memset(b->value, 0, strlen(b->value));	/* a secret */
		free(b->value);
	}
	free(b);
}

ProtobufCGrpc2CallCredentials *
protobuf_c_grpc2_call_credentials_new_bearer(
	const ProtobufCGrpc2BearerCredentialsOptions *options)
{
	ProtobufCGrpc2BearerCredentialsOptions o;
	BearerCredentials *b;
	size_t n;

	if (options == NULL ||
	    PROTOBUF_C_GRPC2__COPY_OPTIONS(&o, options) != 0 || o.token == NULL)
		return NULL;
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return NULL;
	n = strlen(o.token) + sizeof "Bearer ";
	b->value = malloc(n);
	if (b->value == NULL) {
		free(b);
		return NULL;
	}
	strcpy(b->value, "Bearer ");
	strcat(b->value, o.token);
	b->base.get = bearer_get;
	b->base.destroy = bearer_destroy;
	b->base.ref = bearer_ref;
	b->refs = 1;
	b->floating = 1;
	return &b->base;
}

/* ---- server: bearer tokens, decided by a callback ---- */

typedef struct {
	ProtobufCGrpc2ServerAuth base;		/* must be first */
	int refs;
	int floating;
	ProtobufCGrpc2BearerVerifyFn verify;
	void *user_data;
	void (*free_user_data)(void *);
} BearerAuth;

static void
bearer_check(ProtobufCGrpc2ServerAuth *self, const ProtobufCGrpc2CallInfo *info,
	     const ProtobufCGrpc2Header *headers, size_t n_headers,
	     ProtobufCGrpc2AuthDone done, void *done_data)
{
	BearerAuth *a = (BearerAuth *) self;
	size_t i;

	for (i = 0; i < n_headers; i++) {
		const char *v;

		if (strcmp(headers[i].name, "authorization") != 0)
			continue;
		v = headers[i].value;
		if (strlen(v) > 7 && strncasecmp(v, "Bearer ", 7) == 0) {
			a->verify(a->user_data, v + 7, info, done, done_data);
			return;
		}
	}
	done(done_data, PROTOBUF_C_GRPC2_UNAUTHENTICATED,
	     "missing or malformed credentials", NULL);
}

static void
bearer_auth_ref(ProtobufCGrpc2ServerAuth *self)
{
	BearerAuth *a = (BearerAuth *) self;

	if (a->floating)
		a->floating = 0;
	else
		a->refs++;
}

static void
bearer_auth_destroy(ProtobufCGrpc2ServerAuth *self)
{
	BearerAuth *a = (BearerAuth *) self;

	if (--a->refs > 0)
		return;
	if (a->free_user_data != NULL)
		a->free_user_data(a->user_data);
	free(a);
}

ProtobufCGrpc2ServerAuth *
protobuf_c_grpc2_server_auth_new_bearer(const ProtobufCGrpc2BearerAuthOptions *options)
{
	ProtobufCGrpc2BearerAuthOptions o;
	BearerAuth *a;

	if (options == NULL ||
	    PROTOBUF_C_GRPC2__COPY_OPTIONS(&o, options) != 0 || o.verify == NULL)
		return NULL;
	a = calloc(1, sizeof(*a));
	if (a == NULL)
		return NULL;
	a->verify = o.verify;
	a->user_data = o.user_data;
	a->free_user_data = o.free_user_data;
	a->base.check = bearer_check;
	a->base.destroy = bearer_auth_destroy;
	a->base.ref = bearer_auth_ref;
	a->refs = 1;
	a->floating = 1;
	return &a->base;
}

