#include "spdk/stdinc.h"
#include "spdk/env.h"

#include "spdk/nvme.h"

static struct spdk_nvme_transport_id g_trid = {};

struct ctrlr_entry {
	// spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe controller
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char						name[1024];
};
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns		*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr_opts *opts);

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts);

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns);

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion);

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion);

static void
cleanup(void);
