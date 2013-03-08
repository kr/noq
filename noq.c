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
#include <assert.h>

#define nil ((void*)0)

typedef struct Buffer Buffer;

struct Buffer {
	int  r, w;
	int  open;
	int  reof, weof;
	int  head, tail;
	char dat[32*1024];
};

static pid_t origpid;

static int
max(int a, int b)
{
	return a>b ? a : b;
}

// Kill the original process if something goes horribly wrong.
static void
abortorig(void)
{
	kill(origpid, SIGABRT);
}

// Function tcplisten opens a socket listening on port.
static int
tcplisten(unsigned short port)
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
	r = listen(listener, 0);
	if (r == -1) {
		perror("noq: listen");
		exit(1);
	}
	return listener;
}

// Function diallocal dials the given port on address 127.0.0.1.
// Returns -1 if we got ECONNREFUSED.
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

	return connect(conn, (struct sockaddr *) &addr, sizeof addr);
}

static void
bufinit(Buffer *b, int r, int w)
{
	memset(b, 0, sizeof *b);
	b->r = r;
	b->w = w;
	b->open = 1;
}

// Causes b to be marked as finished and ready to close.
// The next time through the main loop its w fd will be closed.
static void
bufpoison(Buffer *b)
{
	b->reof = b->weof = 1;
}

static int
bufdone(Buffer *b)
{
	return b->open && b->weof;
}

static void
bufclose(Buffer *b)
{
	close(b->w);
	b->open = 0;
}

// Function bufspc returns the number of bytes of contiguous available space
// in buffer b.
static int
bufspc(Buffer *b)
{
	if (b->tail > b->head) {
		return b->tail - 1 - b->head;
	}
	if (b->tail == 0) {
		return sizeof b->dat - 1 - b->head;
	}
	return sizeof b->dat - b->head;
}

// Function buflen returns the number of bytes of contiguous available data
// in buffer b.
static int
buflen(Buffer *b)
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
		int r = read(b->r, b->dat+b->head, bufspc(b));
		if (r < 1) {
			b->reof = 1;
		} else {
			b->head += r;
			b->head %= sizeof b->dat;
		}
	}
	if (FD_ISSET(b->w, wfd)) {
		int r = write(b->w, b->dat+b->tail, buflen(b));
		if (r < 1) {
			bufpoison(b);
		} else {
			b->tail += r;
			b->tail %= sizeof b->dat;
		}
	}
	if (b->reof && buflen(b)==0 && !b->weof) {
		shutdown(b->w, SHUT_WR);
		b->weof = 1;
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

void
dfork(char **arg)
{
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
		execvp(*arg, arg);
		perror(*arg);
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
	const unsigned short N = scanshort(getenv("NOQMAXCONNS") ? : "1");
	const unsigned short aport = scanshort(getenv("PORT"));
	const unsigned short dport = 2000;
	int i;
	for (i=0; i<narg; i++) {
		if (strcmp(getenv("PORT"), arg[i]) == 0) {
			arg[i] = "2000";
		}
	}
	setenv("PORT", "2000", 1);

	dfork(arg+1);

	int nb = 0;
	Buffer b[N*2];
	memset(b, 0, sizeof b);
	int pub = tcplisten(aport);
	for (;;) {
		int i;
		int nfd = 0;
		fd_set rfd, wfd;
		FD_ZERO(&rfd);
		FD_ZERO(&wfd);
		if (pub != -1) {
			assert(nb < N);
			FD_SET(pub, &rfd);
			nfd = max(nfd, pub+1);
		}
		for (i=0; i<N*2; i++) {
			if (b[i].open && !b[i].reof && bufspc(&b[i])) {
				FD_SET(b[i].r, &rfd);
				nfd = max(nfd, b[i].r+1);
			}
			if (b[i].open && !b[i].weof && buflen(&b[i])) {
				FD_SET(b[i].w, &wfd);
				nfd = max(nfd, b[i].w+1);
			}
		}
		int r = select(nfd, &rfd, &wfd, nil, nil);
		if (r == -1) {
			perror("noq: select");
			exit(1);
		}
		if (pub != -1 && FD_ISSET(pub, &rfd)) {
			int aconn = accept(pub, nil, 0);
			if (aconn == -1) {
				perror("noq: accept");
				exit(1);
			}
			int dconn = diallocal(dport);
			for (i=0; i<N; i++) {
				Buffer *pair = &b[2*i];
				if (!pair[0].open && !pair[1].open) {
					bufinit(pair, aconn, dconn);
					bufinit(pair+1, dconn, aconn);
					if (dconn == -1) {
						// Error in connect(2).
						bufpoison(pair);
						bufpoison(pair+1);
					}
					nb++;
					break;
				}
			}
			assert(i < N);
			if (nb == N) {
				close(pub);
				pub = -1;
			}
		}
		for (i=0; i<N*2; i++) {
			if (b[i].open) {
				bufrw(&b[i], &rfd, &wfd);
			}
		}
		for (i=0; i<N; i++) {
			Buffer *pair = &b[2*i];
			if (bufdone(pair) && bufdone(pair+1)) {
				bufclose(pair);
				bufclose(pair+1);
				nb--;
				if (pub == -1) {
					pub = tcplisten(aport);
				}
			}
		}
	}
}
