/**
 * (C) Copyright 2016-2018 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of the DAOS server. It implements the startup/shutdown
 * routines for the daos_server.
 */
#define D_LOGFAC	DD_FAC(server)

#include <signal.h>
#include <abt.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <execinfo.h>

#include <daos/btree_class.h>
#include <daos/common.h>
#include "srv_internal.h"

#include <daos.h> /* for daos_init() */

#define MAX_MODULE_OPTIONS	64
#define MODULE_LIST		"vos,rdb,mgmt,pool,cont,obj,rebuild"

/** List of modules to load */
static char		modules[MAX_MODULE_OPTIONS + 1];

/**
 * Number of threads the user would like to start
 * 0 means default value, which is one thread per core
 */
static unsigned int	nr_threads;

/** Server crt group ID */
static char	       *server_group_id = DAOS_DEFAULT_GROUP_ID;

/** Storage path (hack) */
const char	       *dss_storage_path = "/mnt/daos";

/** NVMe config file */
const char	       *dss_nvme_conf = "/etc/daos_nvme.conf";

/** Socket Directory */
const char	       *dss_socket_dir = "/var/run/daos_server";

/** attach_info path to support singleton client */
static bool	        save_attach_info;
const char	       *attach_info_path;

/** HW topology */
hwloc_topology_t	dss_topo;

/** Module facility bitmask */
static uint64_t		dss_mod_facs;

/** System map path */
static const char      *sys_map_path;

/** Self rank */
static d_rank_t		self_rank = -1;

/*
 * Register the dbtree classes used by native server-side modules (e.g.,
 * ds_pool, ds_cont, etc.). Unregistering is currently not supported.
 */
static int
register_dbtree_classes(void)
{
	int rc;

	rc = dbtree_class_register(DBTREE_CLASS_KV, 0 /* feats */,
				   &dbtree_kv_ops);
	if (rc != 0) {
		D_ERROR("failed to register DBTREE_CLASS_KV: %d\n", rc);
		return rc;
	}

	rc = dbtree_class_register(DBTREE_CLASS_IV,
				   BTR_FEAT_UINT_KEY /* feats */,
				   &dbtree_iv_ops);
	if (rc != 0) {
		D_ERROR("failed to register DBTREE_CLASS_IV: %d\n", rc);
		return rc;
	}

	rc = dbtree_class_register(DBTREE_CLASS_NV, 0 /* feats */,
				   &dbtree_nv_ops);
	if (rc != 0) {
		D_ERROR("failed to register DBTREE_CLASS_NV: %d\n", rc);
		return rc;
	}

	rc = dbtree_class_register(DBTREE_CLASS_UV, 0 /* feats */,
				   &dbtree_uv_ops);
	if (rc != 0) {
		D_ERROR("failed to register DBTREE_CLASS_UV: %d\n", rc);
		return rc;
	}

	rc = dbtree_class_register(DBTREE_CLASS_EC,
				   BTR_FEAT_UINT_KEY /* feats */,
				   &dbtree_ec_ops);
	if (rc != 0) {
		D_ERROR("failed to register DBTREE_CLASS_EC: %d\n", rc);
		return rc;
	}

	rc = dbtree_class_register(DBTREE_CLASS_RECX, BTR_FEAT_DIRECT_KEY,
				   &dbtree_recx_ops);
	/* DBTREE_CLASS_RECX possibly be registered by client-stack also */
	if (rc == -DER_EXIST)
		rc = 0;
	if (rc != 0)
		D_ERROR("failed to register DBTREE_CLASS_RECX: %d\n", rc);

	return rc;
}

static int
modules_load(uint64_t *facs)
{
	char		*mod;
	char		*sep;
	char		*run;
	uint64_t	 mod_facs;
	int		 rc = 0;

	sep = strdup(modules);
	if (sep == NULL)
		return -DER_NOMEM;
	run = sep;

	mod = strsep(&run, ",");
	while (mod != NULL) {
		if (strcmp(mod, "object") == 0)
			mod = "obj";
		else if (strcmp(mod, "po") == 0)
			mod = "pool";
		else if (strcmp(mod, "container") == 0 ||
			 strcmp(mod, "co") == 0)
			mod = "cont";
		else if (strcmp(mod, "management") == 0)
			mod = "mgmt";
		else if (strcmp(mod, "vos") == 0)
			mod = "vos_srv";

		mod_facs = 0;
		rc = dss_module_load(mod, &mod_facs);
		if (rc != 0) {
			D_ERROR("Failed to load module %s: %d\n",
				mod, rc);
			break;
		}

		if (facs != NULL)
			*facs |= mod_facs;

		mod = strsep(&run, ",");
	}

	D_FREE(sep);
	return rc;
}

static int
server_init()
{
	int		rc;
	uint32_t	flags = CRT_FLAG_BIT_SERVER | CRT_FLAG_BIT_LM_DISABLE;
	d_rank_t	rank = -1;
	uint32_t	size = -1;

	rc = daos_debug_init(NULL);
	if (rc != 0)
		return rc;

	rc = register_dbtree_classes();
	if (rc != 0)
		D_GOTO(exit_debug_init, rc);

	/** initialize server topology data */
	hwloc_topology_init(&dss_topo);
	hwloc_topology_load(dss_topo);

	/* initialize the modular interface */
	rc = dss_module_init();
	if (rc)
		D_GOTO(exit_debug_init, rc);

	D_INFO("Module interface successfully initialized\n");

	/* initialize the network layer */
	if (sys_map_path != NULL)
		flags |= CRT_FLAG_BIT_PMIX_DISABLE;
	rc = crt_init(server_group_id, flags);
	if (rc)
		D_GOTO(exit_mod_init, rc);
	if (sys_map_path != NULL) {
		if (self_rank == -1) {
			D_ERROR("self rank required\n");
			D_GOTO(exit_crt_init, rc = -DER_INVAL);
		}
		rc = crt_rank_self_set(self_rank);
		if (rc != 0)
			D_ERROR("failed to set self rank %u: %d\n", self_rank,
				rc);
		rc = dss_sys_map_load(sys_map_path, server_group_id, self_rank,
				      nr_threads);
		if (rc) {
			D_ERROR("failed to load %s: %d\n", sys_map_path, rc);
			D_GOTO(exit_crt_init, rc);
		}
	}
	D_INFO("Network successfully initialized\n");

	rc = crt_group_rank(NULL, &rank);
	D_ASSERTF(rc == 0, "%d\n", rc);
	if (sys_map_path != NULL)
		D_ASSERTF(rank == self_rank, "%u == %u\n", rank, self_rank);
	rc = crt_group_size(NULL, &size);
	D_ASSERTF(rc == 0, "%d\n", rc);

	/* rank 0 save attach info for singleton client if needed */
	if (save_attach_info && rank == 0) {
		if (attach_info_path != NULL) {
			rc = crt_group_config_path_set(attach_info_path);
			if (rc != 0) {
				D_ERROR("crt_group_config_path_set(path %s) "
					"failed, rc: %d.\n", attach_info_path,
					rc);
				D_GOTO(exit_mod_init, rc);
			}
		}
		rc = crt_group_config_save(NULL, true);
		if (rc)
			D_GOTO(exit_mod_init, rc);
		D_INFO("server group attach info saved\n");
	}

	rc = ds_iv_init();
	if (rc)
		D_GOTO(exit_crt_init, rc);

	/* load modules */
	rc = modules_load(&dss_mod_facs);
	if (rc)
		/* Some modules may have been loaded successfully. */
		D_GOTO(exit_mod_loaded, rc);
	D_INFO("Module %s successfully loaded\n", modules);

	/* start up service */
	rc = dss_srv_init(nr_threads);
	if (rc) {
		D_ERROR("DAOS cannot be initialized using the configured "
			"path (%s).   Please ensure it is on a PMDK compatible "
			"file system and writeable by the current user\n",
			dss_storage_path);
		D_GOTO(exit_mod_loaded, rc);
	}
	D_INFO("Service is now running\n");

	if (dss_mod_facs & DSS_FAC_LOAD_CLI) {
		rc = daos_init();
		if (rc) {
			D_ERROR("daos_init (client) failed, rc: %d.\n", rc);
			D_GOTO(exit_srv_init, rc);
		}
		D_INFO("Client stack enabled\n");
	} else {
		rc = daos_hhash_init();
		if (rc) {
			D_ERROR("daos_hhash_init failed, rc: %d.\n", rc);
			D_GOTO(exit_srv_init, rc);
		}
		D_INFO("daos handle hash-table initialized\n");
	}
	/* server-side uses D_HTYPE_PTR handle */
	d_hhash_set_ptrtype(daos_ht.dht_hhash);

	rc = dss_module_setup_all();
	if (rc != 0)
		D_GOTO(exit_daos_fini, rc);
	D_INFO("Modules successfully set up\n");

	D_PRINT("DAOS I/O server (v%s) process %u started on rank %u "
		"(out of %u) with %u xstream(s)\n", DAOS_VERSION, getpid(),
		rank, size, dss_nxstreams);

	return 0;

exit_daos_fini:
	if (dss_mod_facs & DSS_FAC_LOAD_CLI)
		daos_fini();
	else
		daos_hhash_fini();
exit_srv_init:
	dss_srv_fini(true);
exit_mod_loaded:
	dss_module_unload_all();
	ds_iv_fini();
exit_crt_init:
	crt_finalize();
exit_mod_init:
	dss_module_fini(true);
exit_debug_init:
	daos_debug_fini();
	return rc;
}

static void
server_fini(bool force)
{
	D_INFO("Service is shutting down\n");
	dss_module_cleanup_all();
	if (dss_mod_facs & DSS_FAC_LOAD_CLI)
		daos_fini();
	else
		daos_hhash_fini();
	dss_srv_fini(force);
	dss_module_unload_all();
	ds_iv_fini();
	crt_finalize();
	dss_module_fini(force);
	daos_debug_fini();
}

static void
usage(char *prog, FILE *out)
{
	fprintf(out, "\
Usage:\n\
  %s -h\n\
  %s [-m modules] [-c ncores] [-g group] [-s path]\n\
Options:\n\
  --modules=modules, -m modules\n\
      List of server modules to load (default \"%s\")\n\
  --cores=ncores, -c ncores\n\
      Number of cores to use (default all)\n\
  --group=group, -g group\n\
      Server group name (default \"%s\")\n\
  --storage=path, -s path\n\
      Storage path (default \"%s\")\n\
  --socket_dir=socket_dir, -d socket_dir\n\
      Directory where daos_server sockets are located (default \"%s\")\n\
  --nvme=config, -n config\n\
      NVMe config file (default \"%s\")\n\
  --attach_info=path, -apath\n\
      Attach info patch (to support non-PMIx client, default \"/tmp\")\n\
  --map=path, -y path\n\
      [Temporary] System map configuration file (default none)\n\
  --rank=rank, -r rank\n\
      [Temporary] Self rank (default none; ignored if no --map|-y)\n\
  --help, -h\n\
      Print this description\n",
		prog, prog, modules, server_group_id, dss_storage_path,
		dss_socket_dir, dss_nvme_conf);
}

static int
parse(int argc, char **argv)
{
	struct	option opts[] = {
		{ "modules",		required_argument,	NULL,	'm' },
		{ "cores",		required_argument,	NULL,	'c' },
		{ "group",		required_argument,	NULL,	'g' },
		{ "storage",		required_argument,	NULL,	's' },
		{ "socket_dir",		required_argument,	NULL,	'd' },
		{ "nvme",		required_argument,	NULL,	'n' },
		{ "attach_info",	required_argument,	NULL,	'a' },
		{ "map",		required_argument,	NULL,	'y' },
		{ "rank",		required_argument,	NULL,	'r' },
		{ "help",		no_argument,		NULL,	'h' },
		{ NULL,			0,			NULL,	0}
	};
	int	rc = 0;
	int	c;

	/* load all of modules by default */
	sprintf(modules, "%s", MODULE_LIST);
	while ((c = getopt_long(argc, argv, "c:m:g:s:d:n:a:y:r:h",
			opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (strlen(optarg) > MAX_MODULE_OPTIONS) {
				rc = -DER_INVAL;
				usage(argv[0], stderr);
				break;
			}
			snprintf(modules, sizeof(modules), "%s", optarg);
			break;
		case 'c': {
			unsigned int	 nr;
			char		*end;

			nr = strtoul(optarg, &end, 10);
			if (end == optarg || nr == ULONG_MAX) {
				rc = -DER_INVAL;
				break;
			}
			nr_threads = nr;
			break;
		}
		case 'g':
			server_group_id = optarg;
			break;
		case 's':
			dss_storage_path = optarg;
			break;
		case 'd':
			dss_socket_dir = optarg;
			break;
		case 'n':
			dss_nvme_conf = optarg;
			break;
		case 'h':
			usage(argv[0], stdout);
			break;
		case 'a':
			save_attach_info = true;
			attach_info_path = optarg;
			break;
		case 'y':
			sys_map_path = optarg;
			break;
		case 'r':
			self_rank = atoi(optarg);
			break;
		default:
			usage(argv[0], stderr);
			rc = -DER_INVAL;
		}
		if (rc < 0)
			return rc;
	}

	return 0;
}

struct sigaction old_handlers[_NSIG];

static int
daos_register_sighand(int signo, void (*handler) (int, siginfo_t *, void *))
{
	struct sigaction	act;
	int			rc;

	if ((signo < 0) || (signo >= _NSIG)) {
		D_ERROR("invalid signo %d to register\n", signo);
		return -DER_INVAL;
	}

	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = handler;

	/* register new and save old handler */
	rc = sigaction(signo, &act, &old_handlers[signo]);
	if (rc != 0) {
		D_ERROR("sigaction() failure registering new and reading "
			"old %d signal handler\n", signo);
		return rc;
	}
	return 0;
}

static void
print_backtrace(int signo, siginfo_t *info, void *p)
{
	void	*bt[128];
	int	 bt_size, i, rc;

	fprintf(stderr, "*** Process %d received signal %d ***\n", getpid(),
		signo);

	if (info != NULL) {
		fprintf(stderr, "Associated errno: %s (%d)\n",
			strerror(info->si_errno), info->si_errno);

		/* XXX we could get more signal/fault specific details from
		 * info->si_code decode
		 */

		switch (signo) {
		case SIGILL:
		case SIGFPE:
			fprintf(stderr, "Failing at address: %p\n",
				info->si_addr);
			break;
		case SIGSEGV:
		case SIGBUS:
			fprintf(stderr, "Failing for address: %p\n",
				info->si_addr);
			break;
		}
	} else {
		fprintf(stderr, "siginfo is NULL, additional information "
			"unavailable\n");
	}

	bt_size = backtrace(bt, 128);
	if (bt_size >= 128)
		fprintf(stderr, "backtrace may have been truncated\n");

	/* start at 1 to forget about me! */
	for (i = 1; i < bt_size; i++)
		backtrace_symbols_fd(&bt[i], 1, fileno(stderr));

	/* re-register old handler */
	rc = sigaction(signo, &old_handlers[signo], NULL);
	if (rc != 0) {
		D_ERROR("sigaction() failure registering new and reading old "
			"%d signal handler\n", signo);
		/* XXX it is weird, we may end-up in a loop handling same
		 * signal with this handler if we return
		 */
		exit(EXIT_FAILURE);
	}

	/* XXX we may choose to forget about old handler and simply register
	 * signal again as SIG_DFL and raise it for corefile creation
	 */
	if (old_handlers[signo].sa_sigaction != NULL ||
	    old_handlers[signo].sa_handler != SIG_IGN) {
		/* XXX will old handler get accurate siginfo_t/ucontext_t ?
		 * we may prefer to call it with the same params we got ?
		 */
		raise(signo);
	}

	memset(&old_handlers[signo], 0, sizeof(struct sigaction));
}

int
main(int argc, char **argv)
{
	sigset_t	set;
	int		sig;
	int		rc;

	/** parse command line arguments */
	rc = parse(argc, argv);
	if (rc)
		exit(EXIT_FAILURE);

	/** block all possible signals but faults */
	sigfillset(&set);
	sigdelset(&set, SIGILL);
	sigdelset(&set, SIGFPE);
	sigdelset(&set, SIGBUS);
	sigdelset(&set, SIGSEGV);
	/** also allow abort()/assert() to trigger */
	sigdelset(&set, SIGABRT);

	rc = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (rc) {
		perror("failed to mask signals");
		exit(EXIT_FAILURE);
	}

	/* register our own handler for faults and abort()/assert() */
	/* errors are harmless */
	daos_register_sighand(SIGILL, print_backtrace);
	daos_register_sighand(SIGFPE, print_backtrace);
	daos_register_sighand(SIGBUS, print_backtrace);
	daos_register_sighand(SIGSEGV, print_backtrace);
	daos_register_sighand(SIGABRT, print_backtrace);

	rc = ABT_init(argc, argv);
	if (rc != 0) {
		D_ERROR("failed to init ABT: %d\n", rc);
		exit(EXIT_FAILURE);
	}
	/** server initialization */
	rc = server_init();
	if (rc)
		exit(EXIT_FAILURE);

	/** wait for shutdown signal */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	while (1) {
		rc = sigwait(&set, &sig);
		if (rc) {
			D_ERROR("failed to wait for signals: %d\n", rc);
			break;
		}

		/* use this iosrv main thread's context to dump Argobot internal
		 * infos upon SIGUSR1
		 */
		if (sig == SIGUSR1) {
			dss_dump_ABT_state();
			continue;
		}

		/* SIGINT/SIGTERM/SIGUSR2 cause server shutdown */
		break;
	}

	/** shutdown */
	server_fini(true);

	ABT_finalize();
	exit(EXIT_SUCCESS);
}
