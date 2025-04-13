#include "nvme_wrr_demo.h"

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\t\n");
	printf("\t[-d io queue depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-p io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(cores priority are urgent/high/medium/low in turn)\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-b arbitration burst, default: 7 (unlimited)]\n");
	printf("\t[-h high priority weight, default: 16]\n");
	printf("\t[-m medium priority weight, default: 8]\n");
	printf("\t[-l low priority weight, default: 4]\n");
}

int main(int argc, char **argv) {
    int rc;
    struct spdk_env_opts opts;

	char task_pool_name[30];
	uint32_t task_count = 0;

    rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}
    
    opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
    opts.name = "nvme_wrr_demo";
    opts.core_mask = g_arbitration.core_mask;

    // Initialize the SPDK environment
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

    // Get tick rate to convert second into ticks in order to limit the work
    g_arbitration.tsc_rate = spdk_get_ticks_hz();

    if (register_workers() != 0) {
		rc = 1;
		goto exit;
	}
	if (register_controllers() != 0) {
		rc = 1;
		goto exit;
	}
	if (associate_workers_with_ns() != 0) {
		rc = 1;
		goto exit;
	}

	// Create a task pool
	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", getpid());
	task_count = g_arbitration.num_namespaces > g_arbitration.num_workers ?
				 g_arbitration.num_namespaces : g_arbitration.num_workers;
	task_count *= g_arbitration.io_queue_depth;
	g_task_pool = spdk_mempool_create(task_pool_name, task_count,
					sizeof(struct arb_task), 0, SPDK_ENV_NUMA_ID_ANY);
	if (g_task_pool == NULL) {
		fprintf(stderr, "could not initialize task pool\n");
		rc = 1;
		goto exit;
	}

	printf("Initialization complete. Launching workers.\n");


	// TODO

exit:
	cleanup(task_count);
    spdk_env_fini();
    if (rc != 0) {
        fprintf(stderr, "%s: errors occurred\n", argv[0]);
    }
    return rc;
}

static int
parse_args(int argc, char **argv)
{
    int op;
    long int val;
	const char *io_pattern_type = NULL;
	bool mix_specified = false;

	while ((op = getopt(argc, argv, "b:c:d:h:l:m:p:s:t:M:")) != -1) {
		switch (op) {
		case 'c':
			g_arbitration.core_mask = optarg;
			break;
		case 'p':
			g_arbitration.io_pattern_type = optarg;
			break;
        case '?':
            usage(argv[0]);
            return 1;
		default:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
            case 'd':
				g_arbitration.io_queue_depth = val;
				break;
			case 's':
				g_arbitration.io_size_bytes = val;
				break;
            case 'M':
				g_arbitration.rw_percentage = val;
				mix_specified = true;
				break;
            case 't':
				g_arbitration.time_in_sec = val;
				break;
			case 'b':
				g_arbitration.arbitration_burst = val;
				break;
			case 'h':
				g_arbitration.high_priority_weight = val - 1;
				break;
			case 'm':
				g_arbitration.medium_priority_weight = val - 1;
				break;
			case 'l':
				g_arbitration.low_priority_weight = val - 1;
				break;
			default:
				usage(argv[0]);
				return -EINVAL;
			}
		}
	}

	io_pattern_type = g_arbitration.io_pattern_type;

	if (strcmp(io_pattern_type, "read") &&
	    strcmp(io_pattern_type, "write") &&
	    strcmp(io_pattern_type, "randread") &&
	    strcmp(io_pattern_type, "randwrite") &&
	    strcmp(io_pattern_type, "rw") &&
	    strcmp(io_pattern_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}
    if (!strcmp(io_pattern_type, "read") ||
        !strcmp(io_pattern_type, "write") ||
        !strcmp(io_pattern_type, "rw")) {
        g_arbitration.is_random = 0;
    } else {
        g_arbitration.is_random = 1;
    }

    if (!strcmp(io_pattern_type, "read") ||
        !strcmp(io_pattern_type, "randread") ||
        !strcmp(io_pattern_type, "write") ||
        !strcmp(io_pattern_type, "randwrite")) {
        if (mix_specified) {
            fprintf(stderr, "Ignoring -M option because io pattern type"
                " is not rw or randrw.\n");
        }
    }
	if (!strcmp(io_pattern_type, "read") ||
	    !strcmp(io_pattern_type, "randread")) {
		g_arbitration.rw_percentage = 100;
	}
	if (!strcmp(io_pattern_type, "write") ||
	    !strcmp(io_pattern_type, "randwrite")) {
		g_arbitration.rw_percentage = 0;
	}
	if (!strcmp(io_pattern_type, "rw") ||
	    !strcmp(io_pattern_type, "randrw")) {
		if (g_arbitration.rw_percentage < 0 || g_arbitration.rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 or rw or randrw.\n");
			return 1;
		}
	}

	if (g_arbitration.arbitration_burst >= 7) {
		printf("The arbitration burst is set to bigger than 7 which means unlimited\n");
	}

	if (g_arbitration.high_priority_weight >= 255 ||
		g_arbitration.medium_priority_weight >= 255 ||
		g_arbitration.low_priority_weight >= 255) {
		fprintf(stderr,
			"High/medium/low priority weight must be specified to value from 1 to 256.\n");
		return 1;
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;
	enum spdk_nvme_qprio qprio = SPDK_NVME_QPRIO_URGENT;

	// The environment is initialized with core_mask at main function
	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}
		printf("Allocated worker for %d\n", i);

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		qprio++;
		// Mask for more than four cores
		worker->qprio = qprio & SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK;
		
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_arbitration.num_workers++;
	}

	return 0;
}

static int
register_controllers(void)
{
	printf("Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_arbitration.num_namespaces == 0) {
		fprintf(stderr, "No valid namespaces to continue IO testing\n");
		return 1;
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	// Update arbitration configuration, forced to use WRR
	opts->arb_mechanism = SPDK_NVME_CC_AMS_WRR;
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attached to %s\n", trid->traddr);
	// Update with actual arbitration configuration
	printf("  Weighted Round Robin: %s\n", opts->arb_mechanism == SPDK_NVME_CC_AMS_WRR ?
	       "Supported" : "Not Supported");
	register_ctrlr(ctrlr, opts);
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct ctrlr_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	union spdk_nvme_cap_register cap;

	entry = calloc(1, sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	printf("  Name: %s\n", entry->name);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
			nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}

	// Setup weighted round robin
	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	if (opts->arb_mechanism == SPDK_NVME_CC_AMS_WRR && (cap.bits.ams & SPDK_NVME_CAP_AMS_WRR)) {
		print_arb_feature(ctrlr);
		set_arb_feature(ctrlr);
		print_arb_feature(ctrlr);	
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	// Judge if IO size is valid
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	// IO size is invalid can because of
	// 1. The size of namespace size is smaller than IO size
	// 2. IO size is smaller than sectoer size
	// 3. IO size is not a multiple of sector size 
	if (spdk_nvme_ns_get_size(ns) < g_arbitration.io_size_bytes ||
	    spdk_nvme_ns_get_extended_sector_size(ns) > g_arbitration.io_size_bytes ||
	    g_arbitration.io_size_bytes % spdk_nvme_ns_get_extended_sector_size(ns)) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_extended_sector_size(ns),
		       g_arbitration.io_size_bytes);
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->nvme.ctrlr = ctrlr;
	entry->nvme.ns = ns;
	entry->size_in_ios = spdk_nvme_ns_get_size(ns) / g_arbitration.io_size_bytes;
	entry->io_size_blocks = g_arbitration.io_size_bytes / spdk_nvme_ns_get_sector_size(ns);
	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
	g_arbitration.num_namespaces++;
}

static void
print_arb_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	struct spdk_nvme_cmd cmd = {};

	g_features[SPDK_NVME_FEAT_ARBITRATION].valid = false;

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_ARBITRATION;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, get_feature_completion, &g_features[SPDK_NVME_FEAT_ARBITRATION]);
	if (rc) {
		printf("Get Arbitration Feature: Failed 0x%x\n", rc);
		return;
	}

	// get_feature() is asynchronized. Polling is needed
	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		/* This function is thread safe */
		/* This function can be called at any point while the controller is attached to the SPDK NVMe driver. */
	}

	if (g_features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		// SPDK designed serveral unions to decode the result automatically
		union spdk_nvme_cmd_cdw11 arb;
		arb.feat_arbitration.raw = g_features[SPDK_NVME_FEAT_ARBITRATION].result;

		printf("Current Arbitration Configuration\n");
		printf("===========\n");
		printf("Arbitration Burst:           ");
		if (arb.feat_arbitration.bits.ab == SPDK_NVME_ARBITRATION_BURST_UNLIMITED) {
			printf("no limit\n");
		} else {
			printf("%u\n", 1u << arb.feat_arbitration.bits.ab);
		}
		printf("Low Priority Weight:         %u\n", arb.feat_arbitration.bits.lpw + 1);
		printf("Medium Priority Weight:      %u\n", arb.feat_arbitration.bits.mpw + 1);
		printf("High Priority Weight:        %u\n", arb.feat_arbitration.bits.hpw + 1);
		printf("\n");
	} else {
		printf("Set Arbitration Feature failed\n");
	}
}

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature_entry *feature = cb_arg;
	// Use the offset of feature entry pointer to calculate fid (feature id)
	int fid = feature - g_features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get_feature(0x%02X) failed\n", fid);
	} else {
		feature->result = cpl->cdw0;
		feature->valid = true;
	}

	g_outstanding_commands--;
}

static void
set_arb_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	struct spdk_nvme_cmd cmd = {};

	g_features[SPDK_NVME_FEAT_ARBITRATION].valid = false;

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_ARBITRATION;

	cmd.cdw11_bits.feat_arbitration.bits.ab = g_arbitration.arbitration_burst;
	cmd.cdw11_bits.feat_arbitration.bits.hpw = g_arbitration.high_priority_weight;
	cmd.cdw11_bits.feat_arbitration.bits.mpw = g_arbitration.medium_priority_weight;
	cmd.cdw11_bits.feat_arbitration.bits.lpw = g_arbitration.low_priority_weight;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0,
					    set_feature_completion, &g_features[SPDK_NVME_FEAT_ARBITRATION]);
	if (rc) {
		printf("Set Arbitration Feature: Failed 0x%x\n", rc);
		return;
	}

	// Polling is also needed
	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (!g_features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		printf("Set Arbitration Feature failed and use default configuration\n");
	}
}

static void
set_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature_entry *feature = cb_arg;
	int fid = feature - g_features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("set_feature(0x%02X) failed\n", fid);
	} else {
		printf("Set Arbitration Feature Successfully\n\n");
		feature->valid = true;
	}

	g_outstanding_commands--;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry			*ns_entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	struct worker_ns_ctx	*ns_ctx;
	
	// The functoin is designed to make every worker or every namespace are being used
	// In this experiment, the device has only one namespaces, so all the workers are associated to the same namespace
	int count = g_arbitration.num_namespaces > g_arbitration.num_workers ?
				g_arbitration.num_namespaces : g_arbitration.num_workers;
	
	for (int i = 0; i < count; i++) {
		if (ns_entry == NULL) {
			break;
		}

		// Allocate a context for worker
		ns_ctx = malloc(sizeof(struct worker_ns_ctx));
		if (!ns_ctx) {
			return 1;
		}
		memset(ns_ctx, 0, sizeof(*ns_ctx));

		printf("Associating %s Namespace %u with lcore %d\n", ns_entry->name, 
				spdk_nvme_ns_get_id(ns_entry->nvme.ns), worker->lcore);
		ns_ctx->ns_entry = ns_entry;
		TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		ns_entry = TAILQ_NEXT(ns_entry, link);
		if (ns_entry == NULL) {
			ns_entry = TAILQ_FIRST(&g_namespaces);
		}
	}

	return 0;
}

// TODO

static void
cleanup(uint32_t task_count)
{
	struct worker_thread *worker, *tmp_worker;
	struct worker_ns_ctx *ns_ctx, *tmp_ns_ctx;
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	if (spdk_mempool_count(g_task_pool) != (size_t)task_count) {
		fprintf(stderr, "task_pool count is %zu but should be %u\n", 
				spdk_mempool_count(g_task_pool), task_count);
	}
	spdk_mempool_free(g_task_pool);

	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		/* ns_worker_ctx is a list in the worker */
		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			free(ns_ctx);
		}

		free(worker);
	};

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	};

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}
