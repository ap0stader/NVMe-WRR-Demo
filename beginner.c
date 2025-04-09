#include "beginner.h"

#define DATA_BUFFER_STRING "NVMe Demo Started!"

struct nvme_demo_sequence {
	struct ns_entry	*ns_entry;
	char			*buf;
	int				is_completed;
};

static void
nvme_demo(void);

int
main(int argc, char **argv)
{	
	// Return value
	int rc;

	// Initialize the SPDK environment options structure
	struct spdk_env_opts opts;
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);

	opts.name = "beginner";

	// Initialize the SPDK environment
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	// Initialize NVMe Transport ID, using PCIe
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	// SPDK NVMe enumeration process, cb == callback
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	// Check if all the preparations are done
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}
	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}

	printf("Initialization complete.\n");

	nvme_demo();
	
exit:
	fflush(stdout);
	cleanup();
	// Cleanup the SPDK environment
	spdk_env_fini();
	return rc;
}

static void
nvme_demo(void)
{
	struct ns_entry				*ns_entry;
	struct nvme_demo_sequence	sequence;
	int							rc;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		// Any I/O qpair allocated for a controller can submit I/O to any namespace on that controller.
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}

		// The device in this experiment does not support CMB (Controller Memory Buffer). Skip trying spdk_nvme_ctrlr_map_cmb().

		// Use spdk_dma_zmalloc to allocate a 4KB zeroed buffer. This memory will be pinned.
		sequence.ns_entry = ns_entry;

		printf("INFO: using host memory buffer for IO\n");
		sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		snprintf(sequence.buf, 0x1000, "%s", DATA_BUFFER_STRING);

		sequence.is_completed = 0;

		// The active namespace of the device is not a Zoned Namespace. Skip reset_zone_and_wait_for_completion().

		// Write the data buffer to LBA 0 of this namespace
		// Because of nvme_demo is polling before the write I/O is completed, so passing &sequence (a local variable) is legal.
		rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf,
					    0, /* LBA start */
					    1, /* number of LBAs */
					    write_complete, &sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}

		// Poll for completions
		// The SPDK NVMe driver will only check for completions when the application calls spdk_nvme_qpair_process_completions().
		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0 /* Process all available completions */);
		}

		// It is the responsibility of the caller to ensure all pending I/O are completed before trying to free the qpair.
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}

// probe_cb will be called for each NVMe controller found
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr_opts *opts)
{
	// Attach every probed controller
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

// attach_cb will be called for each controller after the SPDK NVMe driver
// has completed initializing the controller chose to attach.
static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct ctrlr_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	
	int nsid;
	struct spdk_nvme_ns *ns;
	
	printf("Attached to %s ", trid->traddr);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	// Use an NVMe admin command to read detailed information on the controller
	// Specification 5.15.2.2 Identify Controller data structure
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	// cdata->mn == Model Number  cdata->sn == Serial Number
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	printf("%s\n", entry->name);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	// In NVMe, namespace IDs start at 1
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}
	entry->ctrlr = ctrlr;
	entry->ns = ns;

	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_demo_sequence	*sequence = arg;
	struct ns_entry				*ns_entry = sequence->ns_entry;
	int							rc;

	if (spdk_nvme_cpl_is_error(completion)) {
		// spdk_nvme_qpair_print_completion() is a debug tool function
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	
	// Free the buffer associated withthe write I/O 
	spdk_free(sequence->buf);
	
	// And allocate a new zeroed buffer for reading the data back from the NVMe namespace.
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);

	// Read the data in LBA 0 of this namespace
	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_complete, (void *)sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_demo_sequence *sequence = arg;

	// Assume the I/O was successful
	sequence->is_completed = 1;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	// Compare and check if the I/O is correct
	if (strcmp(sequence->buf, DATA_BUFFER_STRING)) {
		fprintf(stderr, "Read data doesn't match write data\n");
		exit(1);
	}

	printf("%s\n", sequence->buf);
	spdk_free(sequence->buf);
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		// detach_ctx tracks detachment of multiple controllers
		// An new context is allocated if this call is the first successful start of detachment in a sequence.
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	// detach_ctx 
	if (detach_ctx) {
		// polling the detachment
		spdk_nvme_detach_poll(detach_ctx);
	}
}
