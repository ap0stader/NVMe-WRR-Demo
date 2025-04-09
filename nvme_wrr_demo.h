#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/nvme.h"

static struct spdk_nvme_transport_id g_trid = {};

struct arb_context {
	// Specify by option
	int				io_queue_depth;
	uint32_t		io_size_bytes;
	const char		*io_pattern_type;
	int				is_random;
	int				rw_percentage;
	int				time_in_sec;
};

static struct arb_context g_arbitration = {
	// Default value
	.io_queue_depth				= 64,
	.io_size_bytes				= 131072,
	.io_pattern_type			= "randrw",
	.is_random					= 1,
	.rw_percentage				= 50,
	.time_in_sec				= 20,
};

// cores 0-3 for urgent/high/medium/low
#define CORE_MASK "0xf"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char					    name[1024];
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);

struct ns_entry {
	struct {
		struct spdk_nvme_ctrlr	*ctrlr;
		struct spdk_nvme_ns		*ns;
	} nvme;

	TAILQ_ENTRY(ns_entry)		link;
	uint32_t				    io_size_blocks;
	uint64_t				    size_in_ios;
	char					    name[1024];
};

static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)		ns_ctx;
	TAILQ_ENTRY(worker_thread)		link;
	unsigned						lcore;
	enum spdk_nvme_qprio			qprio;
};

static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);

static int
parse_args(int argc, char **argv);
