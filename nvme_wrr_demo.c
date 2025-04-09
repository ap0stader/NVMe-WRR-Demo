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
	printf("\t[-t time in seconds]\n");
}

int main(int argc, char **argv) {
    int rc;
    struct spdk_env_opts opts;
    
    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);

    rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}
    
    opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
    opts.name = "nvme_wrr_demo";
    opts.core_mask = CORE_MASK;

    // Initialize the SPDK environment
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

exit:
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

	while ((op = getopt(argc, argv, "d:p:s:t:M:")) != -1) {
		switch (op) {
		case 'p':
			g_arbitration.io_pattern_type = optarg;
			break;
        case 'h':
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

	return 0;
}
