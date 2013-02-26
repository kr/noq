#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <errno.h>

#define nil ((void*)0)

typedef struct Buffer Buffer;

struct Buffer {
	int  r, w;
	int  reof, weof;
	int  head, tail;
	char dat[32*1024];
};

static pid_t origpid;

// Kill the original process if something goes horribly wrong.
static void
abortorig(void)
{
	kill(origpid, SIGABRT);
}

// Function accepttcp opens a socket listening on port, accepts a single
// connection, closes the listener socket, and finally returns the
// accepted connection.
static int
accepttcp(unsigned short port)
{
	int listener = socket(PF_INET, SOCK_STREAM, 0);
	if (listener == -1) {
		perror("noq: socket");
		exit(1);
	}
	int one = 1;
	int r = setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (r == -1) {
		perror("noq: setsockopt SO_REUSEADDR");
		exit(1);
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	r = bind(listener, (struct sockaddr *) &addr, sizeof addr);
	if (r == -1) {
		perror("noq: bind");
		exit(1);
	}
	r = listen(listener, 1);
	if (r == -1) {
		perror("noq: listen");
		exit(1);
	}
	int conn = accept(listener, nil, 0);
	if (conn == -1) {
		perror("noq: accept");
		exit(1);
	}
	close(listener);
	return conn;
}

// Function diallocal dials the given port on addres 127.0.0.1.
static int
diallocal(unsigned short port)
{
	int conn = socket(PF_INET, SOCK_STREAM, 0);
	if (conn == -1) {
		perror("noq: socket");
		exit(1);
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_aton("127.0.0.1", &addr.sin_addr);

	for (;;) {
		int r = connect(conn, (struct sockaddr *) &addr, sizeof addr);
		if (r == -1 && errno == ECONNREFUSED) {
			usleep(100*1000);
			continue;
		}
		if (r == -1) {
			perror("noq: connect");
			exit(1);
		}
		return conn;
	}
}

// Function spc returns the number of bytes of contiguous available space
// in buffer b.
static int
spc(Buffer *b)
{
	if (b->tail > b->head) {
		return b->tail - 1 - b->head;
	}
	if (b->tail == 0) {
		return sizeof b->dat - 1 - b->head;
	}
	return sizeof b->dat - b->head;
}

// Function len returns the number of bytes of contiguous available data
// in buffer b.
static int
len(Buffer *b)
{
	if (b->head >= b->tail) {
		return b->head - b->tail;
	}
	return sizeof b->dat - b->tail;
}

// Reads available data from b.r into b.dat, writes available data from
// b.dat into b.w, and closes b.w if necessary.
static void
bufrw(Buffer *b, fd_set *rfd, fd_set *wfd)
{
	if (FD_ISSET(b->r, rfd)) {
		int r = read(b->r, b->dat+b->head, spc(b));
		if (r == -1 && errno == ECONNRESET) {
			b->reof = 1;
		} else if (r == -1) {
			perror("noq: read");
			exit(1);
		} else if (r == 0) {
			b->reof = 1;
		} else {
			b->head += r;
			b->head %= sizeof b->dat;
		}
	}
	if (FD_ISSET(b->w, wfd)) {
		int r = write(b->w, b->dat+b->tail, len(b));
		if (r == -1) {
			perror("noq: write");
			exit(1);
		}
		b->tail += r;
		b->tail %= sizeof b->dat;
	}
	if (b->reof && len(b)==0 && !b->weof) {
		shutdown(b->w, SHUT_WR);
		b->weof = 1;
	}
}

// Copies data between fd0 and fd1 until EOF in both directions.
static void
proxy(int fd0, int fd1)
{
	Buffer b[] = {
		{fd0, fd1},
		{fd1, fd0},
	};
	const int n = 2;
	const int nfd = fd1>fd0 ? fd1+1 : fd0+1;
	for (; !b[0].weof || !b[1].weof;) {
		int i;
		fd_set rfd, wfd;
		FD_ZERO(&rfd);
		FD_ZERO(&wfd);
		for (i=0; i<n; i++) {
			if (!b[i].reof && spc(&b[i])) {
				FD_SET(b[i].r, &rfd);
			}
			if (!b[i].weof && len(&b[i])) {
				FD_SET(b[i].w, &wfd);
			}
		}
		int r = select(nfd, &rfd, &wfd, nil, nil);
		if (r == -1) {
			perror("noq: select");
			exit(1);
		}
		for (i=0; i<n; i++) {
			bufrw(&b[i], &rfd, &wfd);
		}
	}
}

unsigned short
scanshort(char *s)
{
	char *end;
	errno = 0;
	long n = strtol(s, &end, 10);
	if (errno) {
		perror("noq: strtol");
		exit(1);
	}
	if (*end) {
		fprintf(stderr, "noq: bad number: %s\n", s);
		exit(1);
	}
	if (n != (long)(unsigned short)n) {
		fprintf(stderr, "noq: overflow: %s\n", s);
		exit(1);
	}
	return n;
}

int
main(int narg, char **arg)
{
	setlinebuf(stdout);
	if (narg < 2) {
		fprintf(stderr, "usage: noq cmd [arg...]\n");
		exit(2);
	}
	if (!getenv("PORT")) {
		fprintf(stderr, "env var PORT is required\n");
		exit(2);
	}
	unsigned short aport = scanshort(getenv("PORT"));
	unsigned short dport = 5000;
	int i;
	for (i=0; i<narg; i++) {
		if (strcmp(getenv("PORT"), arg[i]) == 0) {
			arg[i] = "5000";
		}
	}
	setenv("PORT", "5000", 1);

	origpid = getpid();
	int pid = fork();
	if (pid < 0) {
		perror("noq: fork");
		exit(2);
	}
	if (pid > 0) {
		int st;
		wait(&st);
		if (WIFSIGNALED(st)) {
			fprintf(stderr, "noq: trampoline killed: %d\n", WTERMSIG(st));
			exit(2);
		}
		if (WEXITSTATUS(st) != 0) {
			fprintf(stderr, "noq: trampoline exited: %d\n", WEXITSTATUS(st));
			exit(2);
		}
		execvp(arg[1], arg+1);
		perror(arg[1]);
		exit(2);
	}

	pid = fork();
	if (pid < 0) {
		exit(1);
	}
	if (pid > 0) {
		exit(0);
	}
	atexit(abortorig);

	for (;;) {
		int aconn = accepttcp(aport);
		int dconn = diallocal(dport);
		proxy(aconn, dconn);
		close(aconn);
		close(dconn);
	}
}
