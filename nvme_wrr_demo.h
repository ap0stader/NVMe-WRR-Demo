#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/event.h"

// outstanding == problems have not yet been resolved
int	g_outstanding_commands = 0;

struct arb_context {
	// Specify by options
	const char		*core_mask;
	int				io_queue_depth;
	uint32_t		io_size_bytes;
	const char		*io_pattern_type;
	int				is_random;
	int				rw_percentage;
	int				time_in_sec;
	uint32_t		arbitration_burst;
	uint32_t		high_priority_weight;
	uint32_t		medium_priority_weight;
	uint32_t		low_priority_weight;
	// Get by using SPDK
	uint64_t		tsc_rate;
	// Other
	int				num_workers;
	int				num_namespaces;
};

static struct arb_context g_arbitration = {
	// Default value
	// cores 0-3 for urgent/high/medium/low
	.core_mask					= "0xf",
	.io_queue_depth				= 64,
	.io_size_bytes				= 131072,
	.io_pattern_type			= "randrw",
	.is_random					= 1,
	.rw_percentage				= 50,
	.time_in_sec				= 20,
	.arbitration_burst			= 0x7,
	.high_priority_weight		= 16-1, // Weights are 0's based number
	.medium_priority_weight		= 8-1,
	.low_priority_weight		= 4-1,
	// Initial value
	.num_workers				= 0,
	.num_namespaces				= 0,
};

// Use to store the features fetched from controller
struct feature_entry {
	uint32_t				result;
	bool					valid;
};

static struct feature_entry g_features[SPDK_NVME_FEAT_ARBITRATION + 1] = {};

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
	// The size of namespace in io size
	uint64_t				    size_in_ios;
	// The amount of blocks of io size
	uint32_t				    io_size_blocks;
	char					    name[1024];
};

static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

struct worker_ns_ctx {
	struct ns_entry				*ns_entry;
	TAILQ_ENTRY(worker_ns_ctx)	link;
	struct spdk_nvme_qpair		*qpair;
	uint64_t					current_queue_depth;
	uint64_t					io_completed;
	uint64_t					offset_in_ios;
	bool						is_draining;
};

struct worker_thread {
	TAILQ_HEAD(, worker_ns_ctx)		ns_ctx;
	TAILQ_ENTRY(worker_thread)		link;
	// Logical core
	unsigned						lcore;
	enum spdk_nvme_qprio			qprio;
};

static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);

struct arb_task {
	struct worker_ns_ctx	*ns_ctx;
	void					*buf;
};

static struct spdk_mempool *g_task_pool = NULL;

static inline const char *
print_qprio(enum spdk_nvme_qprio qprio)
{
	switch (qprio) {
	case SPDK_NVME_QPRIO_URGENT:
		return "urgent priority queue";
	case SPDK_NVME_QPRIO_HIGH:
		return "high priority queue";
	case SPDK_NVME_QPRIO_MEDIUM:
		return "medium priority queue";
	case SPDK_NVME_QPRIO_LOW:
		return "low priority queue";
	default:
		return "invalid priority queue";
	}
}

static int
parse_args(int argc, char **argv);

static int
register_workers(void);

static int
register_controllers(void);

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr_opts *opts);

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts);

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts);

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns);

static void
print_arb_feature(struct spdk_nvme_ctrlr *ctrlr);

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl);

static void
set_arb_feature(struct spdk_nvme_ctrlr *ctrlr);

static void
set_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl);

static int
associate_workers_with_ns(void);

// TODO

static void
cleanup(uint32_t task_count);
