/*
 * A TCP port that is free on 127.0.0.1 right now, chosen by the operating
 * system. The tests that run a server used to derive a port from their process
 * id, and tests running in parallel occasionally picked the same one.
 */
#ifndef FREE_PORT_H
#define FREE_PORT_H

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned
test_free_port(void)
{
	struct sockaddr_in a;
	socklen_t len = sizeof a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	unsigned port;

	assert(fd >= 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	assert(bind(fd, (struct sockaddr *) &a, sizeof a) == 0);
	assert(getsockname(fd, (struct sockaddr *) &a, &len) == 0);
	port = ntohs(a.sin_port);
	close(fd);
	return port;
}

#endif
