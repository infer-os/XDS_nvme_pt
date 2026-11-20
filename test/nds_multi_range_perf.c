/*
 * nds_multi_range_perf — NDS PREAD bandwidth with one HBM range per vector.
 *
 * Each iocb reads one contiguous file region of (ranges * io-size) bytes and
 * scatters it as one io-size vector per registered HBM range. Every vector of
 * an iocb therefore comes from a different HBM range. With the defaults an
 * iocb covers 128 * 16 KiB = 2 MiB and 64 iocbs are in flight, so one pass
 * covers an 8 GiB file and the registered set is 128 ranges of 2 MiB.
 *
 * Build: make -C test nds_multi_range_perf
 * Example (physical NPU HBM via aclrt):
 *   source /usr/local/Ascend/ascend-toolkit/set_env.sh
 *   sudo -E ./test/nds_multi_range_perf --target /mnt/data/nds8g.bin \
 *        --npu-device 0 --warmup 1 --repeat 3
 *
 * The target file is used as its own topology descriptor, so no separate
 * --topology argument is needed. When the target is missing or has the wrong
 * size, it is prefaulted by default (truncated and allocated). --prefault
 * forces this; --no-prefault makes a size mismatch an error. Extents are
 * unwritten but readable.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "nds_api.h"
#include "hbm_acl.h"

#define SECTOR 512u
#define DEFAULT_FILE_SIZE (8ull << 30)
#define DEFAULT_RANGES 128u
#define DEFAULT_RANGE_SIZE (2u << 20)
#define DEFAULT_IO_SIZE (16u << 10)
#define DEFAULT_DEPTH 64u

struct slot {
	bool busy;
	struct nds_io_cb cb;
	struct nds_io_vec *iov;
};

struct bench {
	struct slot *slots;
	struct nds_io_cb *batch_cbs;
	unsigned int *batch_slots;
	struct nds_io_event *events;
	struct nds_io_ctx *ctx;
	unsigned int depth;
	uint64_t nr_iocbs;
	uint64_t iocb_size;
};

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s (%d)\n", what, err < 0 ? strerror(-err) : strerror(errno),
		err < 0 ? -err : -errno);
	exit(EXIT_FAILURE);
}

static void die_target(const char *what, const char *target, int err)
{
	fprintf(stderr, "%s %s: %s (%d)\n", what, target,
		err < 0 ? strerror(-err) : strerror(errno), err < 0 ? -err : -errno);
	exit(EXIT_FAILURE);
}

static uint64_t parse_size(const char *s)
{
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 0);
	unsigned long long mul = 1;

	if (!end || end == s)
		die("parse size", -EINVAL);
	switch (*end) {
	case 'G':
	case 'g':
		mul = 1ull << 30;
		end++;
		break;
	case 'M':
	case 'm':
		mul = 1ull << 20;
		end++;
		break;
	case 'K':
	case 'k':
		mul = 1ull << 10;
		end++;
		break;
	case '\0':
		break;
	default:
		die("parse size suffix", -EINVAL);
	}
	if (*end)
		die("parse size trailing junk", -EINVAL);
	if (v > UINT64_MAX / mul)
		die("parse size overflow", -EOVERFLOW);
	return (uint64_t)(v * mul);
}

static uint64_t parse_u64(const char *s, uint64_t max, const char *what)
{
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || !end || end == s || *end || v > max)
		die(what, -EINVAL);
	return (uint64_t)v;
}

static double seconds(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static double cpu_seconds(const struct rusage *a, const struct rusage *b)
{
	return (double)(b->ru_utime.tv_sec - a->ru_utime.tv_sec) +
	       (double)(b->ru_utime.tv_usec - a->ru_utime.tv_usec) / 1e6 +
	       (double)(b->ru_stime.tv_sec - a->ru_stime.tv_sec) +
	       (double)(b->ru_stime.tv_usec - a->ru_stime.tv_usec) / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;

	return (x > y) - (x < y);
}

static double median(double *v, unsigned int n)
{
	qsort(v, n, sizeof(*v), cmp_double);
	if (n % 2)
		return v[n / 2];
	return (v[n / 2 - 1] + v[n / 2]) / 2;
}

/*
 * Ensure the target exists, has exactly @size bytes, and has allocated
 * extents. Prefault runs by default when the target is missing or its size
 * does not match --file-size; --no-prefault turns that into a hard error.
 */
static void prefault_target(const char *path, uint64_t size)
{
	struct stat st;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		die_target("open target for prefault", path, -errno);
	if (fstat(fd, &st) < 0)
		die_target("fstat target for prefault", path, -errno);
	if (!S_ISREG(st.st_mode))
		die_target("target must be a regular file", path, -EINVAL);

	if ((uint64_t)st.st_size != size) {
		if (ftruncate(fd, (off_t)size) < 0)
			die_target("ftruncate target", path, -errno);
		printf("prefault: resized %s to %" PRIu64 " bytes\n", path, size);
	}
	if (fallocate(fd, 0, 0, (off_t)size) < 0)
		die_target("fallocate target", path, -errno);
	if (fsync(fd) < 0)
		die_target("fsync target", path, -errno);
	if (close(fd) < 0)
		die_target("close target", path, -errno);
	printf("prefault: %s ready (%" PRIu64 " bytes, unwritten extents)\n", path, size);
	fflush(stdout);
}

static void run_pass(struct bench *b, uint64_t *bytes_out, double *sec_out, double *cpu_out)
{
	uint64_t next = 0, done = 0, inflight = 0;
	struct rusage ru0, ru1;
	struct timespec t0, t1;
	unsigned int s;

	for (s = 0; s < b->depth; s++)
		b->slots[s].busy = false;

	if (getrusage(RUSAGE_SELF, &ru0))
		die("getrusage start", -errno);
	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		die("clock_gettime start", -errno);

	while (done < b->nr_iocbs) {
		unsigned int nr = 0, k;
		int ret, got;

		for (s = 0; s < b->depth && next < b->nr_iocbs; s++) {
			if (b->slots[s].busy)
				continue;
			b->slots[s].cb.offset = next * b->iocb_size;
			b->batch_cbs[nr] = b->slots[s].cb;
			b->batch_slots[nr] = s;
			nr++;
			next++;
		}

		if (nr) {
			ret = nds_io_submit(b->ctx, (int)nr, b->batch_cbs);
			if (ret < 0 || ret > (int)nr)
				die("nds_io_submit", ret < 0 ? ret : -EIO);
			if (!ret)
				die("nds_io_submit made no progress", -EIO);
			for (k = 0; k < (unsigned int)ret; k++)
				b->slots[b->batch_slots[k]].busy = true;
			inflight += (unsigned int)ret;
			/* Re-arm the candidates the kernel did not accept. */
			next -= (uint64_t)(nr - (unsigned int)ret);
		}

		if (!inflight)
			die("stalled with no I/O in flight", -EIO);

		got = nds_io_getevents(b->ctx, 1, (int)inflight, b->events, NULL);
		if (got < 0)
			die("nds_io_getevents", got);
		if (!got || got > (int)inflight)
			die("nds_io_getevents returned a bad count", -EIO);

		for (k = 0; k < (unsigned int)got; k++) {
			uint64_t id = b->events[k].user_data;

			if (id >= b->depth)
				die("bad completion cookie", -EINVAL);
			if (!b->slots[id].busy)
				die("duplicate completion", -EINVAL);
			if (b->events[k].res) {
				fprintf(stderr, "I/O failed slot=%" PRIu64 " res=%" PRId64 "\n", id,
					b->events[k].res);
				die("completion", (int)b->events[k].res);
			}
			b->slots[id].busy = false;
			inflight--;
			done++;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t1))
		die("clock_gettime end", -errno);
	if (getrusage(RUSAGE_SELF, &ru1))
		die("getrusage end", -errno);
	if (inflight)
		die("pass ended with I/O in flight", -EIO);

	*bytes_out = done * b->iocb_size;
	*sec_out = seconds(&t0, &t1);
	*cpu_out = cpu_seconds(&ru0, &ru1);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s --target PATH [options]\n"
		"  --target PATH        file to read (also used as topology)\n"
		"  --file-size SIZE     bytes read per pass (default 8G)\n"
		"  --ranges N           HBM ranges, one vector per range\n"
		"                       (default 128)\n"
		"  --range-size SIZE    bytes per HBM range (default 2M)\n"
		"  --io-size SIZE       bytes per vector / file read unit\n"
		"                       (default 16K)\n"
		"  --depth N            outstanding iocbs (default 64)\n"
		"  --repeat N           measured passes (default 1)\n"
		"  --warmup N           unmeasured passes before --repeat (default 0)\n"
		"  --npu-device ID      ACL device for HBM (default 0)\n"
		"  --acl-policy N       aclrtMalloc policy (default 0=HUGE_FIRST)\n"
		"  --no-register-mem    one-shot buffers instead of registered ones\n"
		"  --prefault           always truncate and allocate the target\n"
		"  --no-prefault        fail when the target size is not right\n"
		"                       (default: prefault a missing or wrong-size\n"
		"                       target)\n"
		"  --help               show this text\n"
		"\n"
		"Each iocb transfers ranges * io-size bytes (default 2 MiB).\n"
		"Options must be separated from PATH values; SIZE accepts K/M/G.\n",
		argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "target", required_argument, NULL, 't' },
		{ "file-size", required_argument, NULL, 's' },
		{ "ranges", required_argument, NULL, 'r' },
		{ "range-size", required_argument, NULL, 'R' },
		{ "io-size", required_argument, NULL, 'i' },
		{ "depth", required_argument, NULL, 'q' },
		{ "repeat", required_argument, NULL, 'p' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "npu-device", required_argument, NULL, 'd' },
		{ "acl-policy", required_argument, NULL, 'A' },
		{ "no-register-mem", no_argument, NULL, 'N' },
		{ "prefault", no_argument, NULL, 'P' },
		{ "no-prefault", no_argument, NULL, 'Z' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *target = NULL;
	uint64_t file_size = DEFAULT_FILE_SIZE;
	uint64_t range_size = DEFAULT_RANGE_SIZE;
	uint64_t iocb_size;
	uint64_t nr_iocbs;
	uint64_t range_bytes;
	uint64_t bytes = 0;
	unsigned int ranges = DEFAULT_RANGES;
	unsigned int depth = DEFAULT_DEPTH;
	unsigned int repeat = 1;
	unsigned int warmup = 0;
	uint32_t io_size = DEFAULT_IO_SIZE;
	int npu_device = 0;
	int acl_policy = XDS_ACL_MEM_MALLOC_HUGE_FIRST;
	bool register_mem = true;
	int prefault = 0; /* <0: never, 0: on size mismatch, >0: always */
	struct xds_hbm_set hbm = { 0 };
	uint64_t *range_addr = NULL;
	struct nds_mem_range *mem_ranges = NULL;
	struct nds_mem_desc reg = { 0 };
	int32_t fs_fds[1];
	struct nds_init_param init_param = { 0 };
	struct nds_fs_desc fs_desc = { 0 };
	struct nds_io_ctx *ctx = NULL;
	struct bench bench = { 0 };
	struct slot *slots = NULL;
	struct nds_io_vec *iovs = NULL;
	struct nds_io_event *events = NULL;
	struct nds_io_cb *batch_cbs = NULL;
	unsigned int *batch_slots = NULL;
	double *secs = NULL, *cpus = NULL;
	struct stat st;
	unsigned int i, s, r;
	int fd = -1, err, c;

	while ((c = getopt_long(argc, argv, "t:s:r:R:i:q:p:w:d:A:PZNh", opts, NULL)) != -1) {
		switch (c) {
		case 't':
			target = optarg;
			break;
		case 's':
			file_size = parse_size(optarg);
			break;
		case 'r':
			ranges = (unsigned int)parse_u64(optarg, NDS_MEM_MAX_RANGES, "ranges");
			break;
		case 'R':
			range_size = parse_size(optarg);
			break;
		case 'i': {
			uint64_t v = parse_size(optarg);

			if (v > UINT32_MAX)
				die("io-size", -EOVERFLOW);
			io_size = (uint32_t)v;
			break;
		}
		case 'q':
			depth = (unsigned int)parse_u64(optarg, NDS_IO_MAX_IO_CNT, "depth");
			break;
		case 'p':
			repeat = (unsigned int)parse_u64(optarg, UINT32_MAX, "repeat");
			break;
		case 'w':
			warmup = (unsigned int)parse_u64(optarg, UINT32_MAX, "warmup");
			break;
		case 'd':
			npu_device = (int)parse_u64(optarg, INT_MAX, "npu-device");
			break;
		case 'A':
			acl_policy = (int)parse_u64(optarg, INT_MAX, "acl-policy");
			break;
		case 'N':
			register_mem = false;
			break;
		case 'P':
			prefault = 1;
			break;
		case 'Z':
			prefault = -1;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!target || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!ranges || ranges > NDS_MEM_MAX_RANGES)
		die("ranges must be 1..NDS_MEM_MAX_RANGES", -EINVAL);
	if (!io_size || (io_size & (SECTOR - 1)))
		die("io-size must be a non-zero 512B multiple", -EINVAL);
	if (!range_size || (range_size & (SECTOR - 1)) || range_size > SIZE_MAX)
		die("range-size must be a non-zero 512B multiple", -EINVAL);
	if (!depth || depth > NDS_IO_MAX_IO_CNT)
		die("depth must be 1..NDS_IO_MAX_IO_CNT", -EINVAL);
	if ((uint64_t)depth * io_size > range_size)
		die("depth * io-size must fit one HBM range", -EINVAL);
	if (!repeat)
		die("repeat", -EINVAL);
	if (!file_size || file_size > INT64_MAX)
		die("file-size must be 1..INT64_MAX", -EINVAL);
	iocb_size = (uint64_t)ranges * io_size;
	if (file_size % iocb_size)
		die("file-size must be a multiple of ranges * io-size", -EINVAL);
	nr_iocbs = file_size / iocb_size;
	if (!nr_iocbs)
		die("file-size too small for one iocb", -EINVAL);
	range_bytes = (uint64_t)ranges * range_size;

	if (prefault > 0) {
		prefault_target(target, file_size);
	} else if (!prefault) {
		struct stat pst;

		if (stat(target, &pst)) {
			if (errno != ENOENT)
				die_target("stat target", target, -errno);
			prefault_target(target, file_size);
		} else if (!S_ISREG(pst.st_mode)) {
			die_target("target must be a regular file", target, -EINVAL);
		} else if ((uint64_t)pst.st_size != file_size) {
			prefault_target(target, file_size);
		}
	}

	fd = open(target, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (fd < 0)
		die_target("open target", target, -errno);
	if (fstat(fd, &st) < 0)
		die_target("fstat target", target, -errno);
	if (!S_ISREG(st.st_mode))
		die_target("target must be a regular file", target, -EINVAL);
	if ((uint64_t)st.st_size != file_size) {
		fprintf(stderr,
			"target %s size %" PRIu64 " does not match --file-size %" PRIu64 "\n",
			target, (uint64_t)st.st_size, file_size);
		return EXIT_FAILURE;
	}

	range_addr = calloc(ranges, sizeof(*range_addr));
	slots = calloc(depth, sizeof(*slots));
	iovs = calloc((size_t)depth * ranges, sizeof(*iovs));
	events = calloc(depth, sizeof(*events));
	batch_cbs = calloc(depth, sizeof(*batch_cbs));
	batch_slots = calloc(depth, sizeof(*batch_slots));
	mem_ranges = calloc(ranges, sizeof(*mem_ranges));
	secs = calloc(repeat, sizeof(*secs));
	cpus = calloc(repeat, sizeof(*cpus));
	if (!range_addr || !slots || !iovs || !events || !batch_cbs || !batch_slots ||
	    !mem_ranges || !secs || !cpus)
		die("calloc", -ENOMEM);

	err = xds_hbm_alloc_set(&hbm, npu_device, (size_t)range_size, ranges, acl_policy);
	if (err)
		die("xds_hbm_alloc_set", err);
	for (r = 0; r < ranges; r++)
		range_addr[r] = (uint64_t)(uintptr_t)hbm.addrs[r];

	/* Vector addresses are fixed: vector r of slot s lives in range r. */
	for (s = 0; s < depth; s++) {
		slots[s].iov = &iovs[(size_t)s * ranges];
		slots[s].cb.opcode = NDS_IO_OP_PREAD;
		slots[s].cb.rw_flags = register_mem ? NDS_IO_F_REGISTERED_MEM : 0;
		slots[s].cb.obj.fd = fd;
		slots[s].cb.iov = slots[s].iov;
		slots[s].cb.iov_cnt = ranges;
		slots[s].cb.user_data = s;
		for (r = 0; r < ranges; r++) {
			slots[s].iov[r].buf_addr = range_addr[r] + (uint64_t)s * io_size;
			slots[s].iov[r].buf_len = io_size;
		}
	}

	for (r = 0; r < ranges; r++) {
		mem_ranges[r].addr = range_addr[r];
		mem_ranges[r].size = range_size;
	}

	err = nds_init(&init_param);
	if (err)
		die("nds_init", err);
	fs_fds[0] = fd;
	fs_desc.fs_fd = fs_fds;
	fs_desc.fs_fd_cnt = 1;
	err = nds_register_fs(&fs_desc);
	if (err)
		die("nds_register_fs", err);
	if (register_mem) {
		reg.ranges = mem_ranges;
		reg.nr = ranges;
		err = nds_register_mem(&reg);
		if (err)
			die("nds_register_mem", err);
	}
	err = nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_cnt = depth }, &ctx);
	if (err)
		die("nds_io_new_ctx", err);

	printf("target=%s target_size=%" PRIu64 " file_bytes=%" PRIu64 " iocb_bytes=%" PRIu64
	       " io_size=%u ranges=%u range_bytes=%" PRIu64 " hbm_bytes=%" PRIu64
	       " depth=%u repeat=%u warmup=%u registered=%d device=%d uapi_version=%u\n",
	       target, (uint64_t)st.st_size, file_size, iocb_size, io_size, ranges, range_size,
	       range_bytes, depth, repeat, warmup, register_mem ? 1 : 0, npu_device,
	       init_param.version);
	printf("iocbs=%" PRIu64 " vectors=%" PRIu64 "\n", nr_iocbs, nr_iocbs * ranges);
	fflush(stdout);

	bench.slots = slots;
	bench.batch_cbs = batch_cbs;
	bench.batch_slots = batch_slots;
	bench.events = events;
	bench.ctx = ctx;
	bench.depth = depth;
	bench.nr_iocbs = nr_iocbs;
	bench.iocb_size = iocb_size;

	for (i = 0; i < warmup; i++) {
		double sec, cpu;

		run_pass(&bench, &bytes, &sec, &cpu);
		if (bytes != file_size || sec <= 0)
			die("warm-up byte count or time is invalid", -EIO);
		printf("WARMUP index=%u bytes=%" PRIu64 " iocbs=%" PRIu64 " sec=%.9f"
		       " mib_s=%.6f\n",
		       i, bytes, nr_iocbs, sec, (double)bytes / 1048576.0 / sec);
		fflush(stdout);
	}

	for (i = 0; i < repeat; i++) {
		double sec, cpu;

		run_pass(&bench, &bytes, &sec, &cpu);
		if (bytes != file_size || sec <= 0)
			die("pass byte count or time is invalid", -EIO);
		secs[i] = sec;
		cpus[i] = 100.0 * cpu / sec;
		printf("RUN index=%u bytes=%" PRIu64 " iocbs=%" PRIu64 " vectors=%" PRIu64
		       " sec=%.9f mib_s=%.6f iocbs_s=%.3f"
		       " vectors_s=%.3f proc_cpu_pct=%.2f\n",
		       i, bytes, nr_iocbs, nr_iocbs * ranges, sec, (double)bytes / 1048576.0 / sec,
		       (double)nr_iocbs / sec, (double)nr_iocbs * ranges / sec, cpus[i]);
		fflush(stdout);
	}

	{
		double sec = median(secs, repeat);
		double cpu = median(cpus, repeat);
		uint64_t vectors = nr_iocbs * ranges;

		printf("RESULT target=%s file_bytes=%" PRIu64 " iocb_bytes=%" PRIu64
		       " io_size=%u ranges=%u"
		       " range_bytes=%" PRIu64 " hbm_bytes=%" PRIu64
		       " depth=%u repeat=%u registered=%d bytes=%" PRIu64 " iocbs=%" PRIu64
		       " vectors=%" PRIu64 " sec=%.9f mib_s=%.6f iocbs_s=%.3f vectors_s=%.3f"
		       " proc_cpu_pct=%.2f\n",
		       target, file_size, iocb_size, io_size, ranges, range_size, range_bytes,
		       depth, repeat, register_mem ? 1 : 0, file_size, nr_iocbs, vectors, sec,
		       (double)file_size / 1048576.0 / sec, (double)nr_iocbs / sec,
		       (double)vectors / sec, cpu);
	}

	err = nds_io_destroy_ctx(ctx);
	if (err)
		fprintf(stderr, "nds_io_destroy_ctx: %s\n", strerror(-err));
	if (register_mem) {
		err = nds_unregister_mem(&reg);
		if (err)
			fprintf(stderr, "nds_unregister_mem: %s\n", strerror(-err));
	}
	err = nds_unregister_fs(&fs_desc);
	if (err)
		fprintf(stderr, "nds_unregister_fs: %s\n", strerror(-err));
	err = nds_exit();
	if (err)
		fprintf(stderr, "nds_exit: %s\n", strerror(-err));

	if (hbm.active)
		xds_hbm_free_set(&hbm);
	close(fd);
	free(range_addr);
	free(mem_ranges);
	free(secs);
	free(cpus);
	free(batch_slots);
	free(batch_cbs);
	free(events);
	free(iovs);
	free(slots);
	return EXIT_SUCCESS;
}
