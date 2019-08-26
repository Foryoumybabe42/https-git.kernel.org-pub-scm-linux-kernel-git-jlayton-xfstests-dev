/*
 * t_setlease.c: test basic F_SETLEASE functionality
 *
 * Open file, set lease on it. Then fork off children that open the file with
 * different openflags. Ensure we get signals as expected.
 *
 * Copyright (c) 2019: Jeff Layton <jlayton@redhat.com>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/wait.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static volatile bool signalled;

struct leasetest {
	int	openflags;
	int	leasetype;
	int	conf_openflags;
	bool	expect_signal;
};

static struct leasetest testcase[] = {
	{ O_RDONLY, F_RDLCK, O_RDONLY, false },
	{ O_RDONLY, F_RDLCK, O_WRONLY, true },
	{ O_WRONLY, F_WRLCK, O_RDONLY, true },
	{ O_WRONLY, F_WRLCK, O_WRONLY, true },
};

static void usage()
{
	printf("Usage: t_setlease <filename>\n");
}

static void lease_break(int signum)
{
	if (signum == SIGIO)
		signalled = true;
}

/* Open/create a file, set up signal handler and set lease on file. */
static int setlease(const char *fname, int openflags, int leasetype)
{
	int fd, ret;

	fd = open(fname, openflags | O_CREAT, 0644);
	if (fd < 0) {
		perror("open");
		return -errno;
	}

	ret = fcntl(fd, F_SETLEASE, leasetype);
	if (ret) {
		perror("setlease");
		return -errno;
	}
	return fd;
}

static int open_conflict(const char *fname, int openflags)
{
	int fd;

	fd = open(fname, openflags);
	if (fd < 0) {
		perror("open");
		return -errno;
	}
	close(fd);
	return 0;
}

static int simple_lease_break(const char *fname, struct leasetest *test)
{
	int fd, ret, status;
	pid_t pid, exited;

	signalled = false;
	fd = setlease(fname, test->openflags, test->leasetype);
	if (fd < 0)
		return fd;

	pid = fork();
	if (pid < 0) {
		return -errno;
	} else if (pid == 0) {
		/* child */
		close(fd);
		int ret = open_conflict(fname, test->conf_openflags);
		exit(ret ? 1 : 0);
	}

	/* parent */
	while (!signalled) {
		/* Break out if child exited */
		exited = waitpid(pid, &status, WNOHANG);
		if (exited)
			break;
		usleep(1000);
	}

	fcntl(fd, F_SETLEASE, F_UNLCK);
	close(fd);

	/* If it didn't already exit, then wait now */
	if (!exited)
		waitpid(pid, &status, 0);

	if (!WIFEXITED(status)) {
		ret = 1;
	} else {
		ret = WEXITSTATUS(status);
		if (test->expect_signal != signalled)
			ret = 1;
	}

	return ret;
}

int main(int argc, char **argv)
{
	int ret, i;
	char *fname;
	struct sigaction sa = { .sa_handler = lease_break };

	if (argc < 2) {
		usage();
		return 1;
	}

	fname = argv[1];

	ret = sigaction(SIGIO, &sa, NULL);
	if (ret) {
		perror("sigaction");
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(testcase); ++i) {
		struct leasetest *t = &testcase[i];

		ret = simple_lease_break(fname, t);
		if (ret) {
			fprintf(stderr, "Test failure: openflags=%d leasetype=%d conf_openflags=%d expect_signal=%d\n", t->openflags, t->leasetype, t->conf_openflags, t->expect_signal);
			exit(1);
		}
	}
	return 0;
}
