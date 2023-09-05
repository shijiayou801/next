#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"


struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
};

static struct ctrlr_entry *g_controller;
static struct ns_entry *g_namespace;

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = (struct ns_entry *)malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;

	assert(g_namespace == NULL);
	g_namespace = entry;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool probe_cb(
                void *cb_ctx,
                const struct spdk_nvme_transport_id *trid,
                struct spdk_nvme_ctrlr_opts *opts)
{
        printf("attaching to\n");
	printf("traddr:%s\n", 
	       trid->traddr);
	printf("trstring:%s\n",
	       trid->trstring);

        return true;
}

static void attach_cb(
                void *cb_ctx,
                const struct spdk_nvme_transport_id *trid,
                struct spdk_nvme_ctrlr *ctrlr,
                const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = (struct ctrlr_entry *)malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;

	assert(g_controller == NULL);
	g_controller = entry;

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}

		printf("register namespace nsid:%d\n", nsid);
		register_ns(ctrlr, ns);
	}
	
	
	printf("Attached to %s\n", trid->traddr);
}


struct nxt_io {
	struct ns_entry *ns_entry;
	char *buf;
	int is_completed;
	int using_cmb_io;
};

static void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nxt_io *io = (struct nxt_io *)arg;

	/* Assume the I/O was successful */
	io->is_completed = 1;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(io->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		io->is_completed = 2;
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the io as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	printf("%s", io->buf);
	spdk_free(io->buf);
}


static void write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nxt_io			*io = (struct nxt_io *)arg;
	struct ns_entry			*ns_entry = io->ns_entry;
	int				rc;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(io->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		io->is_completed = 2;
		exit(1);
	}

	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	if (io->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(io->buf);
	}
	io->buf = (char *)spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, io->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_complete, (void *)io, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}

void hello_world()
{
	struct ns_entry		*ns_entry;
	struct nxt_io		io;
	size_t			sz;
	int 			rc;

	ns_entry = g_namespace;

	ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
	if (ns_entry->qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return;
	}

	io.using_cmb_io = 1;
	io.buf = (char *)spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
	io.is_completed = 0;
	io.ns_entry = ns_entry;

	if (io.buf == NULL || sz < 0x1000) {
		io.using_cmb_io = 0;
		io.buf = (char *)spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}
	if (io.buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}
	if (io.using_cmb_io) {
		printf("INFO: using controller memory buffer for IO\n");
	} else {
		printf("INFO: using host memory buffer for IO\n");
	}


	assert(spdk_nvme_ns_get_csi(ns_entry->ns) != SPDK_NVME_CSI_ZNS);

	snprintf(io.buf, 0x1000, "%s", "Hello world!\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, io.buf,
			0, /* LBA start */
			1, /* number of LBAs */
			write_complete, &io, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	while (!io.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
}


int main()
{
        struct spdk_env_opts            opts;
        struct spdk_nvme_transport_id *trid;
        struct spdk_pci_addr pci_addr;
        const char *path = "traddr:0000:00:0e.0";

	spdk_env_opts_init(&opts);
        opts.name = "hello_world";
        opts.shm_id = 0;

        if (spdk_env_init(&opts) < 0) {
                fprintf(stderr, "failed to initialize spdk env\n");
                return -1;
        }

        if (spdk_vmd_init()) {
                fprintf(stderr, "failed to init VMD\nb");
                return -1;
        }

	trid = (struct spdk_nvme_transport_id *)calloc(1, sizeof(struct spdk_nvme_transport_id));
        trid->trtype = SPDK_NVME_TRANSPORT_PCIE;

        if (spdk_nvme_transport_id_parse(trid, path)) {
                fprintf(stderr, "invalid transport id format %s\n", path);
                return -1;
        }

        if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
                fprintf(stderr, "invalid traddr %s\n", trid->traddr);
                return -1;
        }

        spdk_pci_addr_fmt(trid->traddr, sizeof(trid->traddr), &pci_addr);

	if (spdk_nvme_probe(trid, NULL, probe_cb, attach_cb, NULL) < 0) {
                fprintf(stderr, "failed to spdk_nvme_probe\n");
                return -1;
        }

	hello_world();

        return 0;
}
