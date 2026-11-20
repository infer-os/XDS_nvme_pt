// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "debugfs_test.h"
#include "file_p2p_api.h"

#define TEST_REG_ADDR 512UL
#define TEST_REG_SIZE ((4UL << 20) - TEST_REG_ADDR)
#define TEST_ADJ_REG_ADDR (TEST_REG_ADDR + TEST_REG_SIZE)
#define TEST_ADJ_REG_SIZE (2UL << 20)
#define TEST_SECOND_REG_ADDR ((8UL << 20) + 512)
#define TEST_SECOND_REG_SIZE ((4UL << 20) - 512)
#define TEST_THIRD_REG_ADDR ((16UL << 20) + 512)
#define TEST_THIRD_REG_SIZE (1UL << 20)
#define TEST_RACE_THREADS 8
#define TEST_UNREGISTER_TIMEOUT_SEC 30
#define MEMORY_DEBUGFS_PATH "/sys/kernel/debug/p2p_device/memory"
#define DEBUGFS_RACE_ITERATIONS 128
#define CONCURRENT_SWAP_ITERATIONS 64

struct race_context {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool go;
	const char *source;
	int mem_fd;
	int io_fd;
};

struct race_worker {
	struct race_context *context;
	unsigned int index;
	int result;
};

struct unregister_worker {
	int dev_fd;
	struct p2p_mem_range range;
	int result;
};

struct swap_submit_context {
	const char *source;
	int mem_fd;
	int io_fd;
	atomic_bool stop;
	int result;
};

static int expect(const char *operation, int actual, int expected)
{
	printf("api=c case=%s ret=%d expected=%d\n", operation, actual, expected);
	if (actual == expected)
		return 0;
	fprintf(stderr, "%s returned %d, expected %d\n", operation, actual,
		expected);
	return -EINVAL;
}

static int submit_read(int io_fd, const char *source,
		       struct p2p_iov *iov, unsigned int iov_nr,
		       unsigned int flags, int mem_fd)
{
	struct io_parameter param = {
		.op = P2P_IO_READ,
		.file_name = source,
		.file_offset = 0,
		.iov = iov,
		.iov_nr = iov_nr,
		.flags = flags,
		.mem_fd = mem_fd,
	};

	return rw_file(io_fd, &param);
}

static int register_ranges(int dev_fd, const struct p2p_mem_range *ranges,
			   uint32_t nr)
{
	struct p2p_mem_register_param param = {
		.ranges = (uint64_t)(uintptr_t)ranges,
		.nr = nr,
	};

	return register_mem(dev_fd, &param);
}

static int unregister_ranges(int dev_fd, const struct p2p_mem_range *ranges,
			     uint32_t nr)
{
	struct p2p_mem_unregister_param param = {
		.ranges = (uint64_t)(uintptr_t)ranges,
		.nr = nr,
	};

	return unregister_mem(dev_fd, &param);
}

static int register_range_at(int dev_fd, unsigned long addr,
			     unsigned long size)
{
	struct p2p_mem_range range = {
		.addr = addr,
		.size = size,
	};

	return register_ranges(dev_fd, &range, 1);
}

static int unregister_range_at(int dev_fd, unsigned long addr,
			       unsigned long size)
{
	struct p2p_mem_range range = {
		.addr = addr,
		.size = size,
	};

	return unregister_ranges(dev_fd, &range, 1);
}

static int register_primary(int dev_fd)
{
	return register_range_at(dev_fd, TEST_REG_ADDR, TEST_REG_SIZE);
}

static int unregister_primary(int dev_fd)
{
	return unregister_range_at(dev_fd, TEST_REG_ADDR, TEST_REG_SIZE);
}

static void *race_submit(void *argument)
{
	struct race_worker *worker = argument;
	struct race_context *context = worker->context;
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR + (worker->index + 32) * 512,
		.size = 512,
	};

	pthread_mutex_lock(&context->lock);
	while (!context->go)
		pthread_cond_wait(&context->cond, &context->lock);
	pthread_mutex_unlock(&context->lock);

	worker->result = submit_read(context->io_fd, context->source, &iov, 1,
				     P2P_IO_F_REGISTERED_MEM,
				     context->mem_fd);
	return NULL;
}

static void *race_unregister(void *argument)
{
	struct unregister_worker *worker = argument;

	worker->result = unregister_ranges(worker->dev_fd, &worker->range, 1);
	return NULL;
}

static int timed_join(pthread_t thread, unsigned int timeout_sec)
{
	struct timespec deadline;
	int err;

	if (clock_gettime(CLOCK_REALTIME, &deadline))
		return -errno;
	deadline.tv_sec += timeout_sec;
	err = pthread_timedjoin_np(thread, NULL, &deadline);
	return err ? -err : 0;
}

static int test_debugfs_memory_race(int dev_fd)
{
	struct debugfs_reader_context reader;
	char expected[64];
	bool registered = false;
	unsigned int i;
	int reader_err;
	int err;

	err = register_range_at(dev_fd, TEST_SECOND_REG_ADDR,
				TEST_SECOND_REG_SIZE);
	if (err)
		return err;
	registered = true;
	snprintf(expected, sizeof(expected), "pid=%d ranges=", (int)getpid());
	err = debugfs_read_file(MEMORY_DEBUGFS_PATH, expected);
	if (err)
		goto out;
	err = unregister_range_at(dev_fd, TEST_SECOND_REG_ADDR,
				  TEST_SECOND_REG_SIZE);
	if (err)
		goto out;
	registered = false;

	err = debugfs_reader_start(&reader, MEMORY_DEBUGFS_PATH);
	if (err)
		return err;
	for (i = 0; i < DEBUGFS_RACE_ITERATIONS; i++) {
		err = register_range_at(dev_fd, TEST_SECOND_REG_ADDR,
					TEST_SECOND_REG_SIZE);
		if (err)
			break;
		registered = true;
		sched_yield();
		err = unregister_range_at(dev_fd, TEST_SECOND_REG_ADDR,
					  TEST_SECOND_REG_SIZE);
		if (err)
			break;
		registered = false;
		sched_yield();
	}
	reader_err = debugfs_reader_stop(&reader);
	if (!err)
		err = reader_err;

out:
	if (registered)
		unregister_range_at(dev_fd, TEST_SECOND_REG_ADDR,
				    TEST_SECOND_REG_SIZE);
	return err;
}

static int test_validation(int dev_fd, int other_fd, const char *source)
{
	struct p2p_mem_range valid_range = {
		.addr = TEST_REG_ADDR,
		.size = TEST_REG_SIZE,
	};
	struct p2p_mem_range wrong_size_range = {
		.addr = TEST_REG_ADDR,
		.size = TEST_REG_SIZE - 512,
	};
	struct p2p_mem_range missing_range = {
		.addr = TEST_THIRD_REG_ADDR,
		.size = TEST_THIRD_REG_SIZE,
	};
	struct p2p_mem_range zero_range = {
		.addr = TEST_REG_ADDR,
	};
	struct p2p_mem_range overflow_range = {
		.addr = UINT64_MAX,
		.size = 2,
	};
	struct p2p_mem_range overlap_ranges[2] = {
		{ .addr = TEST_SECOND_REG_ADDR, .size = 4096 },
		{ .addr = TEST_SECOND_REG_ADDR + 2048, .size = 4096 },
	};
	struct p2p_mem_range existing_overlap_range = {
		.addr = TEST_REG_ADDR + 4096,
		.size = 4096,
	};
	struct p2p_mem_register_param bad_register = {
		.ranges = (uint64_t)(uintptr_t)&valid_range,
		.nr = 1,
		.reserved[0] = 1,
	};
	struct p2p_mem_register_param bad_register_res3 = {
		.ranges = (uint64_t)(uintptr_t)&valid_range,
		.nr = 1,
		.reserved[3] = 1,
	};
	struct p2p_mem_register_param zero_register = {
		.ranges = (uint64_t)(uintptr_t)&zero_range,
		.nr = 1,
	};
	struct p2p_mem_register_param overflow_register = {
		.ranges = (uint64_t)(uintptr_t)&overflow_range,
		.nr = 1,
	};
	struct p2p_mem_register_param overlap_register = {
		.ranges = (uint64_t)(uintptr_t)overlap_ranges,
		.nr = 2,
	};
	struct p2p_mem_unregister_param bad_unregister = {
		.ranges = (uint64_t)(uintptr_t)&valid_range,
		.nr = 1,
		.reserved[0] = 1,
	};
	struct p2p_iov valid = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	struct p2p_iov outside = {
		.addr = TEST_REG_ADDR + TEST_REG_SIZE,
		.size = 512,
	};
	int null_fd = -1;
	int err;

	err = expect("register reserved[0]", register_mem(dev_fd, &bad_register),
		     -EINVAL);
	if (err)
		return err;
	err = expect("register reserved[3]", register_mem(dev_fd, &bad_register_res3),
		     -EINVAL);
	if (err)
		return err;
	err = expect("register zero size", register_mem(dev_fd, &zero_register),
		     -EINVAL);
	if (err)
		return err;
	err = expect("register overflow",
		     register_mem(dev_fd, &overflow_register), -EOVERFLOW);
	if (err)
		return err;
	err = expect("register self overlap",
		     register_mem(dev_fd, &overlap_register), -EINVAL);
	if (err)
		return err;
	err = expect("register overlap existing",
		     register_ranges(dev_fd, &existing_overlap_range, 1), -EEXIST);
	if (err)
		return err;
	err = expect("unregister reserved",
		     unregister_mem(dev_fd, &bad_unregister), -EINVAL);
	if (err)
		return err;
	err = expect("unregister size mismatch",
		     unregister_ranges(dev_fd, &wrong_size_range, 1), -EINVAL);
	if (err)
		return err;
	err = expect("unregister missing range",
		     unregister_ranges(dev_fd, &missing_range, 1), -ENOENT);
	if (err)
		return err;
	err = expect("negative mem_fd",
		     submit_read(dev_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, -1), -EBADF);
	if (err)
		return err;
	null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (null_fd < 0)
		return -errno;
	err = expect("non-p2p mem_fd",
		     submit_read(dev_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, null_fd), -EINVAL);
	close(null_fd);
	if (err)
		return err;
	err = expect("empty mem_fd",
		     submit_read(dev_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, other_fd), -ENOENT);
	if (err)
		return err;
	err = expect("unknown read flag",
		     submit_read(dev_fd, source, &valid, 1,
				 P2P_IO_F_MASK + 1U, dev_fd),
		     -EOPNOTSUPP);
	if (err)
		return err;
	err = expect("registered range bounds",
		     submit_read(dev_fd, source, &outside, 1,
				 P2P_IO_F_REGISTERED_MEM, dev_fd), -ERANGE);
	if (err)
		return err;
	err = expect("mem_fd on another io_fd",
		     submit_read(other_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, dev_fd), 0);
	if (err)
		return err;
	err = drain_io(other_fd);
	if (err)
		return err;
	err = expect("unregister on another fd (empty)",
		     unregister_ranges(other_fd, &valid_range, 1), -ENOENT);
	if (err)
		return err;

	return 0;
}

static int send_fd(int sock, int fd)
{
	char buf = 'x';
	struct iovec iov = { .iov_base = &buf, .iov_len = 1 };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} u;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = u.buf,
		.msg_controllen = sizeof(u.buf),
	};
	struct cmsghdr *cmsg;

	memset(&u, 0, sizeof(u));
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

static int recv_fd(int sock)
{
	char buf = 0;
	struct iovec iov = { .iov_base = &buf, .iov_len = 1 };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} u;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = u.buf,
		.msg_controllen = sizeof(u.buf),
	};
	struct cmsghdr *cmsg;
	int fd = -1;

	if (recvmsg(sock, &msg, 0) < 0)
		return -errno;
	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS)
		return -EINVAL;
	memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
	return fd;
}

static int test_cross_process_sharing(int mem_fd, const char *source)
{
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	int sv[2] = { -1, -1 };
	pid_t child;
	int status;
	int err;

	/* 1. fork() inherited mem_fd + mem_fd == 0 test + -EPERM on reg/unreg */
	child = fork();
	if (child < 0)
		return -errno;
	if (!child) {
		int io_fd = new_p2p_fd();
		int read_err = -1;
		int fd0_err = -1;
		int reg_err = 0;
		int unreg_err = 0;

		if (io_fd >= 0) {
			read_err = submit_read(io_fd, source, &iov, 1,
					       P2P_IO_F_REGISTERED_MEM, mem_fd);
			if (!read_err)
				read_err = drain_io(io_fd);
			if (dup2(mem_fd, 0) == 0) {
				fd0_err = submit_read(io_fd, source, &iov, 1,
						      P2P_IO_F_REGISTERED_MEM, 0);
				if (!fd0_err)
					fd0_err = drain_io(io_fd);
			}
			close_p2p_fd(io_fd);
		}
		reg_err = register_range_at(mem_fd, TEST_THIRD_REG_ADDR,
					    TEST_THIRD_REG_SIZE);
		unreg_err = unregister_primary(mem_fd);
		_exit(read_err == 0 && fd0_err == 0 &&
		      reg_err == -EPERM && unreg_err == -EPERM ?
		      EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (waitpid(child, &status, 0) < 0)
		return -errno;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
		fprintf(stderr, "fork cross-process sharing / fd0 check failed\n");
		return -EINVAL;
	}

	/* 2. SCM_RIGHTS sharing over AF_UNIX socketpair */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
		return -errno;
	child = fork();
	if (child < 0) {
		err = -errno;
		close(sv[0]);
		close(sv[1]);
		return err;
	}
	if (!child) {
		int shared_fd;
		int io_fd;
		int read_err = -1;
		int unreg_err = 0;

		close(sv[0]);
		/* Close inherited mem_fd so child only uses the SCM_RIGHTS fd. */
		close(mem_fd);
		shared_fd = recv_fd(sv[1]);
		close(sv[1]);
		if (shared_fd < 0)
			_exit(EXIT_FAILURE);
		io_fd = new_p2p_fd();
		if (io_fd >= 0) {
			read_err = submit_read(io_fd, source, &iov, 1,
					       P2P_IO_F_REGISTERED_MEM, shared_fd);
			if (!read_err)
				read_err = drain_io(io_fd);
			close_p2p_fd(io_fd);
		}
		unreg_err = unregister_primary(shared_fd);
		close(shared_fd);
		_exit(read_err == 0 && unreg_err == -EPERM ?
		      EXIT_SUCCESS : EXIT_FAILURE);
	}
	close(sv[1]);
	err = send_fd(sv[0], mem_fd);
	close(sv[0]);
	if (waitpid(child, &status, 0) < 0)
		return -errno;
	if (err)
		return err;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
		fprintf(stderr, "SCM_RIGHTS cross-process sharing check failed\n");
		return -EINVAL;
	}
	return 0;
}

static int test_incremental_and_adjacent(int mem_fd, int io_fd,
					 const char *source)
{
	/* Register adjacent range and disjoint second range incrementally. */
	struct p2p_iov cross_reg_scatter[2] = {
		{ .addr = TEST_REG_ADDR, .size = 512 },
		{ .addr = TEST_SECOND_REG_ADDR, .size = 512 },
	};
	struct p2p_iov adjacent_span = {
		.addr = TEST_ADJ_REG_ADDR - 1024,
		.size = 2048,
	};
	struct p2p_iov gap_iov = {
		.addr = TEST_ADJ_REG_ADDR + TEST_ADJ_REG_SIZE - 512,
		.size = 1024,
	};
	struct p2p_iov second_iov = {
		.addr = TEST_SECOND_REG_ADDR,
		.size = 512,
	};
	struct p2p_iov primary_iov = {
		.addr = TEST_REG_ADDR + 512,
		.size = 512,
	};
	bool adj_registered = false;
	bool second_registered = false;
	int err;

	err = register_range_at(mem_fd, TEST_ADJ_REG_ADDR, TEST_ADJ_REG_SIZE);
	if (err)
		goto out;
	adj_registered = true;

	err = register_range_at(mem_fd, TEST_SECOND_REG_ADDR, TEST_SECOND_REG_SIZE);
	if (err)
		goto out;
	second_registered = true;

	/* Scatter across ranges registered in separate calls. */
	err = expect("cross-registration scatter",
		     submit_read(io_fd, source, cross_reg_scatter, 2,
				 P2P_IO_F_REGISTERED_MEM, mem_fd), 0);
	if (err)
		goto out;

	/* Single IOV straddling VA-adjacent segments from separate calls must fail with -ERANGE. */
	err = expect("adjacent-range straddling IOV rejected",
		     submit_read(io_fd, source, &adjacent_span, 1,
				 P2P_IO_F_REGISTERED_MEM, mem_fd), -ERANGE);
	if (err)
		goto out;

	/* Single IOV crossing into unmapped gap must fail with -ERANGE. */
	err = expect("gap crossing IOV",
		     submit_read(io_fd, source, &gap_iov, 1,
				 P2P_IO_F_REGISTERED_MEM, mem_fd), -ERANGE);
	if (err)
		goto out;

	err = drain_io(io_fd);
	if (err)
		goto out;

	/* Unregister second range out-of-order while primary & adjacent remain. */
	err = expect("out-of-order partial unregister",
		     unregister_range_at(mem_fd, TEST_SECOND_REG_ADDR,
					 TEST_SECOND_REG_SIZE), 0);
	if (err)
		goto out;
	second_registered = false;

	err = expect("removed range rejected",
		     submit_read(io_fd, source, &second_iov, 1,
				 P2P_IO_F_REGISTERED_MEM, mem_fd), -ERANGE);
	if (err)
		goto out;

	err = expect("surviving primary range",
		     submit_read(io_fd, source, &primary_iov, 1,
				 P2P_IO_F_REGISTERED_MEM, mem_fd), 0);
	if (err)
		goto out;

out:
	{
		int drain_err = drain_io(io_fd);

		if (!err)
			err = drain_err;
	}
	if (second_registered)
		unregister_range_at(mem_fd, TEST_SECOND_REG_ADDR, TEST_SECOND_REG_SIZE);
	if (adj_registered)
		unregister_range_at(mem_fd, TEST_ADJ_REG_ADDR, TEST_ADJ_REG_SIZE);
	return err;
}

static int test_normal_mem_fd_ignored(int dev_fd, const char *source)
{
	struct p2p_iov iov = {
		.addr = 8UL << 20,
		.size = 512,
	};
	int err;

	err = submit_read(dev_fd, source, &iov, 1, 0, -1);
	if (err)
		return err;
	err = submit_read(dev_fd, source, &iov, 1, 0, 99999);
	if (err)
		return err;
	return drain_io(dev_fd);
}

static void *concurrent_swap_submitter(void *argument)
{
	struct swap_submit_context *ctx = argument;
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};

	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		int err = submit_read(ctx->io_fd, ctx->source, &iov, 1,
				      P2P_IO_F_REGISTERED_MEM, ctx->mem_fd);
		if (err) {
			ctx->result = err;
			break;
		}
		err = drain_io(ctx->io_fd);
		if (err) {
			ctx->result = err;
			break;
		}
	}
	return NULL;
}

static int test_concurrent_table_swap(int mem_fd, int io_fd, const char *source)
{
	struct swap_submit_context ctx = {
		.source = source,
		.mem_fd = mem_fd,
		.io_fd = io_fd,
		.result = 0,
	};
	pthread_t submitter;
	unsigned int i;
	int err = 0;

	atomic_init(&ctx.stop, false);
	if (pthread_create(&submitter, NULL, concurrent_swap_submitter, &ctx))
		return -ENOMEM;

	for (i = 0; i < CONCURRENT_SWAP_ITERATIONS; i++) {
		err = register_range_at(mem_fd, TEST_SECOND_REG_ADDR,
					TEST_SECOND_REG_SIZE);
		if (err)
			break;
		sched_yield();
		err = unregister_range_at(mem_fd, TEST_SECOND_REG_ADDR,
					  TEST_SECOND_REG_SIZE);
		if (err)
			break;
		sched_yield();
	}

	atomic_store_explicit(&ctx.stop, true, memory_order_relaxed);
	pthread_join(submitter, NULL);
	if (!err && ctx.result) {
		fprintf(stderr, "concurrent submit on range A during range B swap failed: %d\n",
			ctx.result);
		err = ctx.result;
	}
	return err;
}

static int test_unregister_before_drain(int mem_fd, int io_fd,
					const char *source)
{
	struct p2p_iov multi_iov[] = {
		{ .addr = TEST_REG_ADDR, .size = 512 },
		{ .addr = (2UL << 20) - 512, .size = 1024 },
		{ .addr = (2UL << 20) + 512, .size = 1536 },
	};
	struct race_context context = {
		.source = source,
		.mem_fd = mem_fd,
		.io_fd = io_fd,
	};
	struct race_worker workers[TEST_RACE_THREADS];
	struct unregister_worker unregister = {
		.dev_fd = mem_fd,
		.range = { .addr = TEST_REG_ADDR, .size = TEST_REG_SIZE },
		.result = -EINPROGRESS,
	};
	pthread_t threads[TEST_RACE_THREADS];
	pthread_t unregister_thread;
	bool unregister_started = false;
	unsigned int started = 0;
	unsigned int i;
	int err;

	err = submit_read(io_fd, source, multi_iov,
			  sizeof(multi_iov) / sizeof(multi_iov[0]),
			  P2P_IO_F_REGISTERED_MEM, mem_fd);
	if (err)
		return err;

	for (i = 0; i < 16; i++) {
		struct p2p_iov iov = {
			.addr = TEST_REG_ADDR + (unsigned long)i * 512,
			.size = 512,
		};

		err = submit_read(io_fd, source, &iov, 1,
				  P2P_IO_F_REGISTERED_MEM, mem_fd);
		if (err)
			return err;
	}

	if (pthread_mutex_init(&context.lock, NULL))
		return -ENOMEM;
	if (pthread_cond_init(&context.cond, NULL)) {
		pthread_mutex_destroy(&context.lock);
		return -ENOMEM;
	}
	for (i = 0; i < TEST_RACE_THREADS; i++) {
		workers[i].context = &context;
		workers[i].index = i;
		workers[i].result = -EINPROGRESS;
		if (pthread_create(&threads[i], NULL, race_submit, &workers[i])) {
			err = -ENOMEM;
			goto release_workers;
		}
		started++;
	}

	pthread_mutex_lock(&context.lock);
	context.go = true;
	pthread_cond_broadcast(&context.cond);
	pthread_mutex_unlock(&context.lock);
	if (pthread_create(&unregister_thread, NULL, race_unregister, &unregister)) {
		err = -ENOMEM;
		goto release_workers;
	}
	unregister_started = true;

release_workers:
	pthread_mutex_lock(&context.lock);
	context.go = true;
	pthread_cond_broadcast(&context.cond);
	pthread_mutex_unlock(&context.lock);
	for (i = 0; i < started; i++) {
		pthread_join(threads[i], NULL);
		if (!workers[i].result || workers[i].result == -ENOENT)
			continue;
		fprintf(stderr, "racing read %u returned %d\n", i,
			workers[i].result);
		if (!err)
			err = workers[i].result;
	}
	if (unregister_started) {
		int join_err;

		/* Unregister must depend on hardware completion, not io_fd drain. */
		join_err = timed_join(unregister_thread,
				      TEST_UNREGISTER_TIMEOUT_SEC);
		if (join_err) {
			fprintf(stderr,
				"unregister did not finish before drain: %s\n",
				strerror(-join_err));
			if (!err)
				err = join_err;
		} else {
			unregister_started = false;
			if (!err)
				err = unregister.result;
		}
	}
	{
		int drain_err = drain_io(io_fd);

		if (!err)
			err = drain_err;
	}
	if (unregister_started) {
		pthread_join(unregister_thread, NULL);
		if (!err)
			err = unregister.result;
	}
	pthread_cond_destroy(&context.cond);
	pthread_mutex_destroy(&context.lock);
	return err;
}

static int test_close_inflight(const char *source)
{
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	int same_fd = -1;
	int mem_fd = -1;
	int io_fd = -1;
	int err;

	/* 1. mem_fd == io_fd close with in-flight I/Os (no f_count leak). */
	same_fd = new_p2p_fd();
	if (same_fd < 0)
		return same_fd;
	err = register_primary(same_fd);
	if (err) {
		close_p2p_fd(same_fd);
		return err;
	}
	err = submit_read(same_fd, source, &iov, 1,
			  P2P_IO_F_REGISTERED_MEM, same_fd);
	close_p2p_fd(same_fd);
	if (err)
		return err;

	/* 2. mem_fd != io_fd: close mem_fd while io_fd has in-flight I/Os. */
	mem_fd = new_p2p_fd();
	io_fd = new_p2p_fd();
	if (mem_fd < 0 || io_fd < 0) {
		err = mem_fd < 0 ? mem_fd : io_fd;
		if (mem_fd >= 0)
			close_p2p_fd(mem_fd);
		if (io_fd >= 0)
			close_p2p_fd(io_fd);
		return err;
	}
	err = register_primary(mem_fd);
	if (err)
		goto out;
	err = submit_read(io_fd, source, &iov, 1,
			  P2P_IO_F_REGISTERED_MEM, mem_fd);
	close_p2p_fd(mem_fd);
	mem_fd = -1;
	if (!err)
		err = drain_io(io_fd);

out:
	if (mem_fd >= 0)
		close_p2p_fd(mem_fd);
	if (io_fd >= 0)
		close_p2p_fd(io_fd);
	return err;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s --topology <block-device>\n", program);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topology = NULL;
	bool primary_registered = false;
	int other_fd = -1;
	int dev_fd = -1;
	int option;
	int err;

	while ((option = getopt_long(argc, argv, "t:", options, NULL)) != -1) {
		if (option == 't') {
			topology = optarg;
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!topology || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	dev_fd = new_p2p_fd();
	other_fd = new_p2p_fd();
	if (dev_fd < 0 || other_fd < 0) {
		err = dev_fd < 0 ? dev_fd : other_fd;
		goto out;
	}
	err = expect("add topology owner", add_topo(dev_fd, topology), 0);
	if (err)
		goto out;
	err = expect("add topology io", add_topo(other_fd, topology), 0);
	if (err)
		goto out;
	err = expect("register primary", register_primary(dev_fd), 0);
	if (err)
		goto out;
	primary_registered = true;

	err = test_validation(dev_fd, other_fd, topology);
	if (err)
		goto out;
	err = expect("incremental and adjacent ranges",
		     test_incremental_and_adjacent(dev_fd, other_fd, topology),
		     0);
	if (err)
		goto out;
	err = expect("debugfs memory register/unregister race",
		     test_debugfs_memory_race(dev_fd), 0);
	if (err)
		goto out;
	err = expect("cross-process sharing",
		     test_cross_process_sharing(dev_fd, topology), 0);
	if (err)
		goto out;
	err = expect("normal mem_fd ignored",
		     test_normal_mem_fd_ignored(dev_fd, topology), 0);
	if (err)
		goto out;
	err = expect("concurrent table swap",
		     test_concurrent_table_swap(dev_fd, other_fd, topology), 0);
	if (err)
		goto out;
	err = expect("unregister before drain",
		     test_unregister_before_drain(dev_fd, other_fd, topology),
		     0);
	if (err)
		goto out;
	primary_registered = false;

	err = expect("close with inflight io", test_close_inflight(topology), 0);
	if (err)
		goto out;

	printf("memory registration tests passed\n");
	err = 0;

out:
	if (primary_registered)
		unregister_primary(dev_fd);
	if (other_fd >= 0)
		close_p2p_fd(other_fd);
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	if (err) {
		fprintf(stderr, "memory registration test failed: %d\n", err);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
