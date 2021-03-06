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
 * ds_pool: Pool Service
 *
 * This file contains the server API methods and the RPC handlers that are both
 * related pool metadata.
 */
#define D_LOGFAC	DD_FAC(pool)

#include <daos_srv/pool.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <daos/pool_map.h>
#include <daos/rpc.h>
#include <daos/rsvc.h>
#include <daos_srv/container.h>
#include <daos_srv/daos_mgmt_srv.h>
#include <daos_srv/daos_server.h>
#include <daos_srv/rdb.h>
#include <daos_srv/rebuild.h>
#include <cart/iv.h>
#include "rpc.h"
#include "srv_internal.h"
#include "srv_layout.h"

/* Pool service state in pool_svc::ps_term */
enum pool_svc_state {
	POOL_SVC_UP_EMPTY,	/* up but DB newly-created and empty */
	POOL_SVC_UP,		/* up and ready to serve */
	POOL_SVC_DRAINING,	/* stepping down */
	POOL_SVC_DOWN		/* down */
};

/* Pool service */
struct pool_svc {
	d_list_t		ps_entry;
	uuid_t			ps_uuid;	/* pool UUID */
	int			ps_ref;
	ABT_rwlock		ps_lock;	/* for DB data */
	struct rdb	       *ps_db;
	rdb_path_t		ps_root;	/* root KVS */
	rdb_path_t		ps_handles;	/* pool handle KVS */
	rdb_path_t		ps_user;	/* pool user attributes KVS */
	struct cont_svc	       *ps_cont_svc;	/* one combined svc for now */
	ABT_mutex		ps_mutex;	/* for POOL_CREATE */
	bool			ps_stop;
	uint64_t		ps_term;
	enum pool_svc_state	ps_state;
	ABT_cond		ps_state_cv;
	int			ps_leader_ref;	/* to leader members below */
	ABT_cond		ps_leader_ref_cv;
	struct ds_pool	       *ps_pool;
};

static int
write_map_buf(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_buf *buf,
	      uint32_t version)
{
	daos_iov_t	value;
	int		rc;

	D_DEBUG(DF_DSMS, "version=%u ntargets=%u ndomains=%u\n", version,
		buf->pb_target_nr, buf->pb_domain_nr);

	/* Write the version. */
	daos_iov_set(&value, &version, sizeof(version));
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_map_version, &value);
	if (rc != 0)
		return rc;

	/* Write the buffer. */
	daos_iov_set(&value, buf, pool_buf_size(buf->pb_nr));
	return rdb_tx_update(tx, kvs, &ds_pool_attr_map_buffer, &value);
}

/*
 * Retrieve the pool map buffer address in persistent memory and the pool map
 * version into "map_buf" and "map_version", respectively.
 */
static int
read_map_buf(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_buf **buf,
	     uint32_t *version)
{
	uint32_t	ver;
	daos_iov_t	value;
	int		rc;

	/* Read the version. */
	daos_iov_set(&value, &ver, sizeof(ver));
	rc = rdb_tx_lookup(tx, kvs, &ds_pool_attr_map_version, &value);
	if (rc != 0)
		return rc;

	/* Look up the buffer address. */
	daos_iov_set(&value, NULL /* buf */, 0 /* size */);
	rc = rdb_tx_lookup(tx, kvs, &ds_pool_attr_map_buffer, &value);
	if (rc != 0)
		return rc;

	*buf = value.iov_buf;
	*version = ver;
	D_DEBUG(DF_DSMS, "version=%u ntargets=%u ndomains=%u\n", *version,
		(*buf)->pb_target_nr, (*buf)->pb_domain_nr);
	return 0;
}

/* Callers are responsible for destroying the object via pool_map_decref(). */
static int
read_map(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_map **map)
{
	struct pool_buf	       *buf;
	uint32_t		version;
	int			rc;

	rc = read_map_buf(tx, kvs, &buf, &version);
	if (rc != 0)
		return rc;

	return pool_map_create(buf, version, map);
}

/* Store uuid in file path. */
static int
uuid_store(const char *path, const uuid_t uuid)
{
	int	fd;
	int	rc;

	/* Create and open the UUID file. */
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		D_ERROR(DF_UUID": failed to create uuid file %s: %d\n",
			DP_UUID(uuid), path, errno);
		rc = daos_errno2der(errno);
		goto out;
	}

	/* Write the UUID. */
	rc = write(fd, uuid, sizeof(uuid_t));
	if (rc != sizeof(uuid_t)) {
		if (rc != -1)
			errno = EIO;
		D_ERROR(DF_UUID": failed to write uuid into %s: %d %d\n",
			DP_UUID(uuid), path, rc, errno);
		rc = daos_errno2der(errno);
		goto out_fd;
	}

	/* Persist the UUID. */
	rc = fsync(fd);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to fsync %s: %d\n", DP_UUID(uuid),
			path, errno);
		rc = daos_errno2der(errno);
	}

	/* Free the resource and remove the file on errors. */
out_fd:
	close(fd);
	if (rc != 0)
		remove(path);
out:
	return rc;
}

/* Load uuid from file path. */
static int
uuid_load(const char *path, uuid_t uuid)
{
	int	fd;
	int	rc;

	/* Open the UUID file. */
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			D_DEBUG(DB_MD, "failed to open uuid file %s: %d\n",
				path, errno);
		else
			D_ERROR("failed to open uuid file %s: %d\n", path,
				errno);
		rc = daos_errno2der(errno);
		goto out;
	}

	/* Read the UUID. */
	rc = read(fd, uuid, sizeof(uuid_t));
	if (rc == sizeof(uuid_t)) {
		rc = 0;
	} else {
		if (rc != -1)
			errno = EIO;
		D_ERROR("failed to read %s: %d %d\n", path, rc, errno);
		rc = daos_errno2der(errno);
	}

	close(fd);
out:
	return rc;
}

/*
 * Called by mgmt module on every storage node belonging to this pool.
 * "path" is the directory under which the VOS and metadata files shall be.
 * "target_uuid" returns the UUID generated for the target on this storage node.
 */
int
ds_pool_create(const uuid_t pool_uuid, const char *path, uuid_t target_uuid)
{
	char   *fpath;
	int	rc;

	uuid_generate(target_uuid);

	/* Store target_uuid in DSM_META_FILE. */
	rc = asprintf(&fpath, "%s/%s", path, DSM_META_FILE);
	if (rc < 0)
		return -DER_NOMEM;
	rc = uuid_store(fpath, target_uuid);
	D_FREE(fpath);

	return rc;
}

static int
uuid_compare_cb(const void *a, const void *b)
{
	uuid_t *ua = (uuid_t *)a;
	uuid_t *ub = (uuid_t *)b;

	return uuid_compare(*ua, *ub);
}

static int
init_pool_metadata(struct rdb_tx *tx, const rdb_path_t *kvs, uint32_t uid,
		   uint32_t gid, uint32_t mode,
		   uint32_t nnodes, uuid_t target_uuids[], const char *group,
		   const d_rank_list_t *target_addrs, uint32_t ndomains,
		   const int *domains)
{
	struct pool_buf	       *map_buf;
	struct pool_component	map_comp;
	uint32_t		map_version = 1;
	uint32_t		nhandles = 0;
	uuid_t		       *uuids;
	daos_iov_t		value;
	struct rdb_kvs_attr	attr;
	int			ntargets = nnodes * dss_nxstreams;
	int			rc;
	int			i;

	/* Prepare the pool map attribute buffers. */
	map_buf = pool_buf_alloc(ndomains + nnodes + ntargets);
	if (map_buf == NULL)
		return -DER_NOMEM;
	/*
	 * Make a sorted target UUID array to determine target IDs. See the
	 * bsearch() call below.
	 */
	D_ALLOC_ARRAY(uuids, nnodes);
	if (uuids == NULL)
		D_GOTO(out_map_buf, rc = -DER_NOMEM);
	memcpy(uuids, target_uuids, sizeof(uuid_t) * nnodes);
	qsort(uuids, nnodes, sizeof(uuid_t), uuid_compare_cb);

	/* Fill the pool_buf out. */
	/* fill domains */
	for (i = 0; i < ndomains; i++) {
		map_comp.co_type = PO_COMP_TP_RACK;	/* TODO */
		map_comp.co_status = PO_COMP_ST_UP;
		map_comp.co_index = i;
		map_comp.co_id = i;
		map_comp.co_rank = 0;
		map_comp.co_ver = map_version;
		map_comp.co_fseq = 1;
		map_comp.co_nr = domains[i];

		rc = pool_buf_attach(map_buf, &map_comp, 1 /* comp_nr */);
		if (rc != 0)
			D_GOTO(out_uuids, rc);
	}

	/* fill nodes */
	for (i = 0; i < nnodes; i++) {
		uuid_t *p = bsearch(target_uuids[i], uuids, nnodes,
				    sizeof(uuid_t), uuid_compare_cb);

		map_comp.co_type = PO_COMP_TP_NODE;
		map_comp.co_status = PO_COMP_ST_UP;
		map_comp.co_index = i;
		map_comp.co_id = p - uuids;
		map_comp.co_rank = target_addrs->rl_ranks[i];
		map_comp.co_ver = map_version;
		map_comp.co_fseq = 1;
		map_comp.co_nr = dss_nxstreams;

		rc = pool_buf_attach(map_buf, &map_comp, 1 /* comp_nr */);
		if (rc != 0)
			D_GOTO(out_uuids, rc);
	}

	/* fill targets */
	for (i = 0; i < nnodes; i++) {
		int j;

		for (j = 0; j < dss_nxstreams; j++) {
			map_comp.co_type = PO_COMP_TP_TARGET;
			map_comp.co_status = PO_COMP_ST_UP;
			map_comp.co_index = j;
			map_comp.co_id = i * dss_nxstreams + j;
			map_comp.co_rank = target_addrs->rl_ranks[i];
			map_comp.co_ver = map_version;
			map_comp.co_fseq = 1;
			map_comp.co_nr = 1;

			rc = pool_buf_attach(map_buf, &map_comp, 1);
			if (rc != 0)
				D_GOTO(out_uuids, rc);
		}
	}

	/* Initialize the UID, GID, and mode attributes. */
	daos_iov_set(&value, &uid, sizeof(uid));
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_uid, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	daos_iov_set(&value, &gid, sizeof(gid));
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_gid, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	daos_iov_set(&value, &mode, sizeof(mode));
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_mode, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Initialize the pool map attributes. */
	rc = write_map_buf(tx, kvs, map_buf, map_version);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	daos_iov_set(&value, uuids, sizeof(uuid_t) * nnodes);
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_map_uuids, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Write the handle attributes. */
	daos_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(tx, kvs, &ds_pool_attr_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	attr.dsa_class = RDB_KVS_GENERIC;
	attr.dsa_order = 16;
	rc = rdb_tx_create_kvs(tx, kvs, &ds_pool_attr_handles, &attr);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Create pool user attributes KVS */
	rc = rdb_tx_create_kvs(tx, kvs, &ds_pool_attr_user, &attr);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

out_uuids:
	D_FREE(uuids);
out_map_buf:
	pool_buf_free(map_buf);
	return rc;
}

/*
 * nreplicas inputs how many replicas are wanted, while ranks->rl_nr
 * outputs how many replicas are actually selected, which may be less than
 * nreplicas. If successful, callers are responsible for calling
 * daos_rank_list_free(*ranksp).
 */
static int
select_svc_ranks(int nreplicas, const d_rank_list_t *target_addrs,
		 int ndomains, const int *domains, d_rank_list_t **ranksp)
{
	int			i_rank_zero = -1;
	int			selectable;
	d_rank_list_t       *ranks;
	int			i;
	int			j;

	if (nreplicas <= 0)
		return -DER_INVAL;

	/* Determine the number of selectable targets. */
	selectable = target_addrs->rl_nr;
	if (daos_rank_list_find((d_rank_list_t *)target_addrs, 0 /* rank */,
				&i_rank_zero)) {
		/*
		 * Unless it is the only target available, we don't select rank
		 * 0 for now to avoid losing orterun stdout.
		 */
		if (selectable > 1)
			selectable -= 1 /* rank 0 */;
	}

	if (nreplicas > selectable)
		nreplicas = selectable;
	ranks = daos_rank_list_alloc(nreplicas);
	if (ranks == NULL)
		return -DER_NOMEM;

	/* TODO: Choose ranks according to failure domains. */
	j = 0;
	for (i = 0; i < target_addrs->rl_nr; i++) {
		if (j == ranks->rl_nr)
			break;
		if (i == i_rank_zero && selectable > 1)
			/* This is rank 0 and it's not the only rank. */
			continue;
		D_DEBUG(DB_MD, "ranks[%d]: %u\n", j, target_addrs->rl_ranks[i]);
		ranks->rl_ranks[j] = target_addrs->rl_ranks[i];
		j++;
	}
	D_ASSERTF(j == ranks->rl_nr, "%d == %u\n", j, ranks->rl_nr);

	*ranksp = ranks;
	return 0;
}

static size_t
get_md_cap(void)
{
	const size_t	size_default = 1 << 27 /* 128 MB */;
	char	       *v;
	int		n;

	v = getenv("DAOS_MD_CAP"); /* in MB */
	if (v == NULL)
		return size_default;
	n = atoi(v);
	if (n < size_default >> 20) {
		D_ERROR("metadata capacity too low; using %zu MB\n",
			size_default >> 20);
		return size_default;
	}
	return (size_t)n << 20;
}

/**
 * Create a (combined) pool(/container) service. This method shall be called on
 * a single storage node in the pool. "target_uuids" shall be an array of the
 * target UUIDs returned by the ds_pool_create() calls.
 *
 * \param[in]		pool_uuid	pool UUID
 * \param[in]		uid		pool UID
 * \param[in]		gid		pool GID
 * \param[in]		mode		pool mode
 * \param[in]		ntargets	number of targets in the pool
 * \param[in]		target_uuids	array of \a ntargets target UUIDs
 * \param[in]		group		crt group ID (unused now)
 * \param[in]		target_addrs	list of \a ntargets target ranks
 * \param[in]		ndomains	number of domains the pool spans over
 * \param[in]		domains		serialized domain tree
 * \param[in,out]	svc_addrs	\a svc_addrs.rl_nr inputs how many
 *					replicas shall be created; returns the
 *					list of pool service replica ranks
 */
int
ds_pool_svc_create(const uuid_t pool_uuid, unsigned int uid, unsigned int gid,
		   unsigned int mode, int ntargets, uuid_t target_uuids[],
		   const char *group, const d_rank_list_t *target_addrs,
		   int ndomains, const int *domains,
		   d_rank_list_t *svc_addrs)
{
	d_rank_list_t	       *ranks;
	uuid_t			rdb_uuid;
	struct rsvc_client	client;
	struct dss_module_info *info = dss_get_module_info();
	crt_endpoint_t		ep;
	crt_rpc_t	       *rpc;
	struct pool_create_in  *in;
	struct pool_create_out *out;
	int			rc;

	D_ASSERTF(ntargets == target_addrs->rl_nr, "ntargets=%u num=%u\n",
		  ntargets, target_addrs->rl_nr);

	rc = select_svc_ranks(svc_addrs->rl_nr, target_addrs, ndomains,
			      domains, &ranks);
	if (rc != 0)
		D_GOTO(out, rc);

	uuid_generate(rdb_uuid);
	rc = ds_pool_rdb_dist_start(rdb_uuid, pool_uuid, ranks,
				    true /* create */, true /* bootstrap */,
				    get_md_cap());
	if (rc != 0)
		D_GOTO(out_ranks, rc);

	rc = rsvc_client_init(&client, ranks);
	if (rc != 0)
		D_GOTO(out_creation, rc);

rechoose:
	/* Create a POOL_CREATE request. */
	ep.ep_grp = NULL;
	rsvc_client_choose(&client, &ep);
	rc = pool_req_create(info->dmi_ctx, &ep, POOL_CREATE, &rpc);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create POOL_CREATE RPC: %d\n",
			DP_UUID(pool_uuid), rc);
		D_GOTO(out_client, rc);
	}
	in = crt_req_get(rpc);
	uuid_copy(in->pri_op.pi_uuid, pool_uuid);
	uuid_clear(in->pri_op.pi_hdl);
	in->pri_uid = uid;
	in->pri_gid = gid;
	in->pri_mode = mode;
	in->pri_ntgts = ntargets;
	in->pri_tgt_uuids.ca_count = ntargets;
	in->pri_tgt_uuids.ca_arrays = target_uuids;
	in->pri_tgt_ranks = (d_rank_list_t *)target_addrs;
	in->pri_ndomains = ndomains;
	in->pri_domains.ca_count = ndomains;
	in->pri_domains.ca_arrays = (int *)domains;

	/* Send the POOL_CREATE request. */
	rc = dss_rpc_send(rpc);
	out = crt_reply_get(rpc);
	D_ASSERT(out != NULL);
	rc = rsvc_client_complete_rpc(&client, &ep, rc,
				      rc == 0 ? out->pro_op.po_rc : -DER_IO,
				      rc == 0 ? &out->pro_op.po_hint : NULL);
	if (rc == RSVC_CLIENT_RECHOOSE) {
		crt_req_decref(rpc);
		dss_sleep(1000 /* ms */);
		D_GOTO(rechoose, rc);
	}
	rc = out->pro_op.po_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create pool: %d\n",
			DP_UUID(pool_uuid), rc);
		D_GOTO(out_rpc, rc);
	}

	rc = daos_rank_list_copy(svc_addrs, ranks);
	D_ASSERTF(rc == 0, "daos_rank_list_copy: %d\n", rc);
out_rpc:
	crt_req_decref(rpc);
out_client:
	rsvc_client_fini(&client);
out_creation:
	if (rc != 0)
		ds_pool_rdb_dist_stop(pool_uuid, ranks, true /* destroy */);
out_ranks:
	daos_rank_list_free(ranks);
out:
	return rc;
}

int
ds_pool_svc_destroy(const uuid_t pool_uuid)
{
	char		id[DAOS_UUID_STR_SIZE];
	crt_group_t    *group;
	int		rc;

	ds_rebuild_leader_stop(pool_uuid, -1);
	rc = ds_pool_rdb_dist_stop(pool_uuid, NULL /* ranks */,
				   true /* destroy */);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to destroy pool service: %d\n",
			DP_UUID(pool_uuid), rc);
		return rc;
	}

	uuid_unparse_lower(pool_uuid, id);
	group = crt_group_lookup(id);
	if (group != NULL) {
		D_DEBUG(DB_MD, DF_UUID": destroying pool group\n",
			DP_UUID(pool_uuid));
		rc = dss_group_destroy(group);
		if (rc != 0) {
			D_ERROR(DF_UUID": failed to destroy pool group: %d\n",
				DP_UUID(pool_uuid), rc);
			return rc;
		}
	}

	return 0;
}

static int
pool_svc_create_group(struct pool_svc *svc, struct pool_map *map)
{
	char		id[DAOS_UUID_STR_SIZE];
	crt_group_t    *group;
	int		rc;

	/* Check if the pool group exists locally. */
	uuid_unparse_lower(svc->ps_uuid, id);
	group = crt_group_lookup(id);
	if (group != NULL)
		return 0;

	/* Attempt to create the pool group. */
	rc = ds_pool_group_create(svc->ps_uuid, map, &group);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create pool group: %d\n",
			 DP_UUID(svc->ps_uuid), rc);
		return rc;
	}

	return 0;
}

/* If the DB is new, +DER_UNINIT is returned. */
static int
pool_svc_step_up(struct pool_svc *svc)
{
	struct rdb_tx			tx;
	struct ds_pool		       *pool;
	d_rank_list_t		       *replicas = NULL;
	struct pool_map		       *map = NULL;
	uint32_t			map_version;
	struct ds_pool_create_arg	arg;
	d_rank_t			rank;
	int				rc;

	D_ASSERT(svc->ps_state != POOL_SVC_UP);
	D_DEBUG(DB_MD, DF_UUID": stepping up to "DF_U64"\n",
		DP_UUID(svc->ps_uuid), svc->ps_term);

	/* Read the pool map into map and map_version. */
	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		goto out;
	ABT_rwlock_rdlock(svc->ps_lock);
	rc = read_map(&tx, &svc->ps_root, &map);
	if (rc == 0)
		rc = rdb_get_ranks(svc->ps_db, &replicas);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0) {
		if (rc == -DER_NONEXIST) {
			D_DEBUG(DF_DSMS, DF_UUID": new db\n",
				DP_UUID(svc->ps_uuid));
			rc = DER_UNINIT;
		} else {
			D_ERROR(DF_UUID": failed to get %s: %d\n",
				DP_UUID(svc->ps_uuid),
				map == NULL ? "pool map" : "replica ranks", rc);
		}
		goto out;
	}
	map_version = pool_map_get_version(map);

	/* Create the pool group. */
	rc = pool_svc_create_group(svc, map);
	if (rc != 0)
		goto out;

	/* Create or revalidate svc->ps_pool with map and map_version. */
	D_ASSERT(svc->ps_pool == NULL);
	arg.pca_map = map;
	arg.pca_map_version = map_version;
	arg.pca_need_group = 1;
	rc = ds_pool_lookup_create(svc->ps_uuid, &arg, &svc->ps_pool);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to get ds_pool: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		goto out;
	}
	pool = svc->ps_pool;
	ABT_rwlock_wrlock(pool->sp_lock);
	if (pool->sp_map != map) {
		/* An existing ds_pool; map not used yet. */
		D_ASSERTF(pool->sp_map_version <= map_version, "%u <= %u\n",
			  pool->sp_map_version, map_version);
		D_ASSERTF(pool->sp_map == NULL ||
			  pool_map_get_version(pool->sp_map) <= map_version,
			  "%u <= %u\n", pool_map_get_version(pool->sp_map),
			  map_version);
		if (pool->sp_map == NULL ||
		    pool_map_get_version(pool->sp_map) < map_version) {
			struct pool_map *tmp;

			/* Need to update pool->sp_map. Swap with map. */
			pool->sp_map_version = map_version;
			tmp = pool->sp_map;
			pool->sp_map = map;
			map = tmp;
		}
	} else {
		map = NULL; /* taken over by pool */
	}
	ABT_rwlock_unlock(pool->sp_lock);

	ds_cont_svc_step_up(svc->ps_cont_svc);

	rc = ds_rebuild_regenerate_task(pool, replicas);
	if (rc != 0) {
		ds_cont_svc_step_down(svc->ps_cont_svc);
		ds_pool_put(svc->ps_pool);
		svc->ps_pool = NULL;
		goto out;
	}

	rc = crt_group_rank(NULL, &rank);
	D_ASSERTF(rc == 0, "%d\n", rc);
	D_PRINT(DF_UUID": rank %u became pool service leader "DF_U64"\n",
		DP_UUID(svc->ps_uuid), rank, svc->ps_term);
out:
	if (map != NULL)
		pool_map_decref(map);
	if (replicas)
		daos_rank_list_free(replicas);
	return rc;
}

static void
pool_svc_step_down(struct pool_svc *svc)
{
	d_rank_t	rank;
	int		rc;

	D_ASSERT(svc->ps_state != POOL_SVC_DOWN);
	D_DEBUG(DB_MD, DF_UUID": stepping down from "DF_U64"\n",
		DP_UUID(svc->ps_uuid), svc->ps_term);

	/* Stop accepting new leader references. */
	svc->ps_state = POOL_SVC_DRAINING;

	/*
	 * TODO: Abort all in-flight RPCs we sent, after aborting bcasts is
	 * implemented.
	 */

	ds_rebuild_leader_stop(svc->ps_uuid, -1);
	/* Wait for all leader references to be released. */
	for (;;) {
		if (svc->ps_leader_ref == 0)
			break;
		D_DEBUG(DB_MD, DF_UUID": waiting for %d references\n",
			DP_UUID(svc->ps_uuid), svc->ps_leader_ref);
		ABT_cond_wait(svc->ps_leader_ref_cv, svc->ps_mutex);
	}

	ds_cont_svc_step_down(svc->ps_cont_svc);
	D_ASSERT(svc->ps_pool != NULL);
	ds_pool_put(svc->ps_pool);
	svc->ps_pool = NULL;

	rc = crt_group_rank(NULL, &rank);
	D_ASSERTF(rc == 0, "%d\n", rc);
	D_PRINT(DF_UUID": rank %u no longer pool service leader "DF_U64"\n",
		DP_UUID(svc->ps_uuid), rank, svc->ps_term);
}

static int
pool_svc_step_up_cb(struct rdb *db, uint64_t term, void *arg)
{
	struct pool_svc	       *svc = arg;
	int			rc;

	ABT_mutex_lock(svc->ps_mutex);
	if (svc->ps_stop) {
		D_DEBUG(DB_MD, DF_UUID": skip term "DF_U64" due to stopping\n",
			DP_UUID(svc->ps_uuid), term);
		D_GOTO(out_mutex, rc = 0);
	}
	D_ASSERTF(svc->ps_state == POOL_SVC_DOWN, "%d\n", svc->ps_state);
	svc->ps_term = term;

	rc = pool_svc_step_up(svc);
	if (rc == DER_UNINIT) {
		svc->ps_state = POOL_SVC_UP_EMPTY;
		D_GOTO(out_mutex, rc = 0);
	} else if (rc != 0) {
		D_ERROR(DF_UUID": failed to step up as leader "DF_U64": %d\n",
			DP_UUID(svc->ps_uuid), term, rc);
		D_GOTO(out_mutex, rc);
	}

	svc->ps_state = POOL_SVC_UP;
out_mutex:
	ABT_mutex_unlock(svc->ps_mutex);
	return rc;
}

static void
pool_svc_step_down_cb(struct rdb *db, uint64_t term, void *arg)
{
	struct pool_svc *svc = arg;

	ABT_mutex_lock(svc->ps_mutex);
	D_ASSERTF(svc->ps_term == term, DF_U64" == "DF_U64"\n", svc->ps_term,
		  term);
	D_ASSERT(svc->ps_state != POOL_SVC_DOWN);

	if (svc->ps_state == POOL_SVC_UP)
		pool_svc_step_down(svc);

	svc->ps_state = POOL_SVC_DOWN;
	ABT_cond_broadcast(svc->ps_state_cv);
	ABT_mutex_unlock(svc->ps_mutex);
}

static void pool_svc_get(struct pool_svc *svc);
static void pool_svc_put(struct pool_svc *svc);
static void pool_svc_stop(struct pool_svc *svc);

static void
pool_svc_stopper(void *arg)
{
	struct pool_svc *svc = arg;

	pool_svc_stop(svc);
	pool_svc_put(svc);
}

static void
pool_svc_stop_cb(struct rdb *db, int err, void *arg)
{
	struct pool_svc	       *svc = arg;
	int			rc;

	pool_svc_get(svc);
	rc = dss_ult_create(pool_svc_stopper, svc, -1, 0, NULL);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create pool service stopper: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		pool_svc_put(svc);
	}
}

static struct rdb_cbs pool_svc_rdb_cbs = {
	.dc_step_up	= pool_svc_step_up_cb,
	.dc_step_down	= pool_svc_step_down_cb,
	.dc_stop	= pool_svc_stop_cb
};

static char *
pool_svc_rdb_path_common(const uuid_t pool_uuid, const char *suffix)
{
	char   *name;
	char   *path;
	int	rc;

	rc = asprintf(&name, RDB_FILE"pool%s", suffix);
	if (rc < 0)
		return NULL;
	rc = ds_mgmt_tgt_file(pool_uuid, name, NULL /* idx */, &path);
	D_FREE(name);
	if (rc != 0)
		return NULL;
	return path;
}

/* Return a pool service RDB path. */
char *
ds_pool_svc_rdb_path(const uuid_t pool_uuid)
{
	return pool_svc_rdb_path_common(pool_uuid, "");
}

/* Return a pool service RDB UUID file path. This file stores the RDB UUID. */
static char *
pool_svc_rdb_uuid_path(const uuid_t pool_uuid)
{
	return pool_svc_rdb_path_common(pool_uuid, "-uuid");
}

int
ds_pool_svc_rdb_uuid_store(const uuid_t pool_uuid, const uuid_t uuid)
{
	char   *path;
	int	rc;

	path = pool_svc_rdb_uuid_path(pool_uuid);
	if (path == NULL)
		return -DER_NOMEM;
	rc = uuid_store(path, uuid);
	D_FREE(path);
	return rc;
}

int
ds_pool_svc_rdb_uuid_load(const uuid_t pool_uuid, uuid_t uuid)
{
	char   *path;
	int	rc;

	path = pool_svc_rdb_uuid_path(pool_uuid);
	if (path == NULL)
		return -DER_NOMEM;
	rc = uuid_load(path, uuid);
	D_FREE(path);
	return rc;
}

int
ds_pool_svc_rdb_uuid_remove(const uuid_t pool_uuid)
{
	char   *path;
	int	rc;

	path = pool_svc_rdb_uuid_path(pool_uuid);
	if (path == NULL)
		return -DER_NOMEM;
	rc = remove(path);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to remove %s: %d\n",
			DP_UUID(pool_uuid), path, errno);
		rc = daos_errno2der(errno);
	}
	D_FREE(path);
	return rc;
}

static int
pool_svc_init(struct pool_svc *svc, const uuid_t uuid)
{
	char   *path;
	uuid_t	rdb_uuid;
	int	rc;

	uuid_copy(svc->ps_uuid, uuid);
	svc->ps_ref = 1;
	svc->ps_stop = false;
	svc->ps_state = POOL_SVC_DOWN;

	rc = ABT_rwlock_create(&svc->ps_lock);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create ps_lock: %d\n", rc);
		D_GOTO(err, rc = dss_abterr2der(rc));
	}

	rc = ABT_mutex_create(&svc->ps_mutex);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create ps_mutex: %d\n", rc);
		D_GOTO(err_lock, rc = dss_abterr2der(rc));
	}

	rc = ABT_cond_create(&svc->ps_state_cv);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create ps_state_cv: %d\n", rc);
		D_GOTO(err_mutex, rc = dss_abterr2der(rc));
	}

	rc = ABT_cond_create(&svc->ps_leader_ref_cv);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create ps_leader_ref_cv: %d\n", rc);
		D_GOTO(err_state_cv, rc = dss_abterr2der(rc));
	}

	rc = rdb_path_init(&svc->ps_root);
	if (rc != 0)
		D_GOTO(err_leader_ref_cv, rc);
	rc = rdb_path_push(&svc->ps_root, &rdb_path_root_key);
	if (rc != 0)
		D_GOTO(err_root, rc);

	rc = rdb_path_clone(&svc->ps_root, &svc->ps_handles);
	if (rc != 0)
		D_GOTO(err_root, rc);
	rc = rdb_path_push(&svc->ps_handles, &ds_pool_attr_handles);
	if (rc != 0)
		D_GOTO(err_handles, rc);

	rc = rdb_path_clone(&svc->ps_root, &svc->ps_user);
	if (rc != 0)
		D_GOTO(err_handles, rc);
	rc = rdb_path_push(&svc->ps_user, &ds_pool_attr_user);
	if (rc != 0)
		D_GOTO(err_user, rc);

	/* Start the RDB with rdb_uuid at path. */
	rc = ds_pool_svc_rdb_uuid_load(uuid, rdb_uuid);
	if (rc != 0)
		goto err_handles;
	path = ds_pool_svc_rdb_path(uuid);
	if (path == NULL) {
		rc = -DER_NOMEM;
		goto err_handles;
	}
	rc = rdb_start(path, rdb_uuid, &pool_svc_rdb_cbs, svc, &svc->ps_db);
	D_FREE(path);
	if (rc != 0)
		D_GOTO(err_user, rc);

	rc = ds_cont_svc_init(&svc->ps_cont_svc, uuid, 0 /* id */, svc->ps_db);
	if (rc != 0)
		D_GOTO(err_db, rc);

	return 0;

err_db:
	rdb_stop(svc->ps_db);
err_user:
	rdb_path_fini(&svc->ps_user);
err_handles:
	rdb_path_fini(&svc->ps_handles);
err_root:
	rdb_path_fini(&svc->ps_root);
err_leader_ref_cv:
	ABT_cond_free(&svc->ps_leader_ref_cv);
err_state_cv:
	ABT_cond_free(&svc->ps_state_cv);
err_mutex:
	ABT_mutex_free(&svc->ps_mutex);
err_lock:
	ABT_rwlock_free(&svc->ps_lock);
err:
	return rc;
}

static void
pool_svc_fini(struct pool_svc *svc)
{
	ds_cont_svc_fini(&svc->ps_cont_svc);
	rdb_stop(svc->ps_db);
	rdb_path_fini(&svc->ps_user);
	rdb_path_fini(&svc->ps_handles);
	rdb_path_fini(&svc->ps_root);
	ABT_cond_free(&svc->ps_leader_ref_cv);
	ABT_cond_free(&svc->ps_state_cv);
	ABT_mutex_free(&svc->ps_mutex);
	ABT_rwlock_free(&svc->ps_lock);
}

static inline struct pool_svc *
pool_svc_obj(d_list_t *rlink)
{
	return container_of(rlink, struct pool_svc, ps_entry);
}

static bool
pool_svc_key_cmp(struct d_hash_table *htable, d_list_t *rlink,
		 const void *key, unsigned int ksize)
{
	struct pool_svc *svc = pool_svc_obj(rlink);

	D_ASSERTF(ksize == sizeof(uuid_t), "%u\n", ksize);
	return uuid_compare(svc->ps_uuid, key) == 0;
}

static void
pool_svc_rec_addref(struct d_hash_table *htable, d_list_t *rlink)
{
	pool_svc_obj(rlink)->ps_ref++;
}

static bool
pool_svc_rec_decref(struct d_hash_table *htable, d_list_t *rlink)
{
	struct pool_svc *svc = pool_svc_obj(rlink);

	D_ASSERTF(svc->ps_ref > 0, "%d\n", svc->ps_ref);
	svc->ps_ref--;
	return svc->ps_ref == 0;
}

static void
pool_svc_rec_free(struct d_hash_table *htable, d_list_t *rlink)
{
	struct pool_svc *svc = pool_svc_obj(rlink);

	D_DEBUG(DF_DSMS, DF_UUID": freeing\n", DP_UUID(svc->ps_uuid));
	D_ASSERT(d_hash_rec_unlinked(&svc->ps_entry));
	D_ASSERTF(svc->ps_ref == 0, "%d\n", svc->ps_ref);
	pool_svc_fini(svc);
	D_FREE(svc);
}

static d_hash_table_ops_t pool_svc_hash_ops = {
	.hop_key_cmp	= pool_svc_key_cmp,
	.hop_rec_addref	= pool_svc_rec_addref,
	.hop_rec_decref	= pool_svc_rec_decref,
	.hop_rec_free	= pool_svc_rec_free
};

static struct d_hash_table	pool_svc_hash;
static ABT_mutex		pool_svc_hash_lock;

int
ds_pool_svc_hash_init(void)
{
	int rc;

	rc = ABT_mutex_create(&pool_svc_hash_lock);
	if (rc != ABT_SUCCESS)
		return dss_abterr2der(rc);
	rc = d_hash_table_create_inplace(D_HASH_FT_NOLOCK, 4 /* bits */,
					 NULL /* priv */, &pool_svc_hash_ops,
					 &pool_svc_hash);
	if (rc != 0)
		ABT_mutex_free(&pool_svc_hash_lock);
	return rc;
}

void
ds_pool_svc_hash_fini(void)
{
	d_hash_table_destroy_inplace(&pool_svc_hash, true /* force */);
	ABT_mutex_free(&pool_svc_hash_lock);
}

static int
pool_svc_lookup(const uuid_t uuid, struct pool_svc **svcp)
{
	d_list_t	*entry;
	bool		nonexist = false;

	ABT_mutex_lock(pool_svc_hash_lock);
	entry = d_hash_rec_find(&pool_svc_hash, uuid, sizeof(uuid_t));
	if (entry == NULL) {
		char	       *path;
		struct stat	buf;
		int		rc;

		/*
		 * See if the DB exists. If an error prevents us from find that
		 * out, return -DER_NOTLEADER so that the client tries other
		 * replicas.
		 */
		path = ds_pool_svc_rdb_path(uuid);
		if (path == NULL) {
			D_ERROR(DF_UUID": failed to get rdb path\n",
				DP_UUID(uuid));
			D_GOTO(out_lock, -DER_NOMEM);
		}
		rc = stat(path, &buf);
		D_FREE(path);
		if (rc != 0) {
			if (errno == ENOENT)
				nonexist = true;
			else
				D_ERROR(DF_UUID": failed to stat rdb: %d\n",
					DP_UUID(uuid), errno);
			D_GOTO(out_lock, daos_errno2der(errno));
		}
	}
out_lock:
	ABT_mutex_unlock(pool_svc_hash_lock);
	if (nonexist)
		return -DER_NONEXIST;
	if (entry == NULL)
		return -DER_NOTLEADER;
	*svcp = pool_svc_obj(entry);
	return 0;
}

static void
pool_svc_get(struct pool_svc *svc)
{
	ABT_mutex_lock(pool_svc_hash_lock);
	d_hash_rec_addref(&pool_svc_hash, &svc->ps_entry);
	ABT_mutex_unlock(pool_svc_hash_lock);
}

static void
pool_svc_put(struct pool_svc *svc)
{
	ABT_mutex_lock(pool_svc_hash_lock);
	d_hash_rec_decref(&pool_svc_hash, &svc->ps_entry);
	ABT_mutex_unlock(pool_svc_hash_lock);
}

/*
 * Is svc up (i.e., ready to accept RPCs)? If not, the caller may always report
 * -DER_NOTLEADER, even if svc->ps_db is in leader state, in which case the
 * client will retry the RPC.
 */
static inline bool
pool_svc_up(struct pool_svc *svc)
{
	return !svc->ps_stop && svc->ps_state == POOL_SVC_UP;
}

/*
 * As a convenient helper for general pool service RPCs, look up the pool
 * service for uuid, check that it is up, and take a reference to the leader
 * members (e.g., the cached pool map). svcp is filled only if zero is
 * returned. If the pool service is not up, hint is filled.
 */
static int
pool_svc_lookup_leader(const uuid_t uuid, struct pool_svc **svcp,
		     struct rsvc_hint *hint)
{
	struct pool_svc	       *svc;
	int			rc;

	rc = pool_svc_lookup(uuid, &svc);
	if (rc != 0)
		return rc;
	if (!pool_svc_up(svc)) {
		if (hint != NULL)
			ds_pool_set_hint(svc->ps_db, hint);
		pool_svc_put(svc);
		return -DER_NOTLEADER;
	}
	svc->ps_leader_ref++;
	*svcp = svc;
	return 0;
}

/*
 * As a convenient helper for general pool service RPCs, put svc obtained from
 * a pool_svc_lookup_leader() call.
 */
static void
pool_svc_put_leader(struct pool_svc *svc)
{
	D_ASSERTF(svc->ps_leader_ref > 0, "%d\n", svc->ps_leader_ref);
	svc->ps_leader_ref--;
	if (svc->ps_leader_ref == 0)
		ABT_cond_broadcast(svc->ps_leader_ref_cv);
	pool_svc_put(svc);
}

/**
 * Look up container service \a pool_uuid. We have to return the address of
 * ps_cont_svc via a pointer... :(
 */
int
ds_pool_cont_svc_lookup_leader(const uuid_t pool_uuid, struct cont_svc ***svcpp,
			       struct rsvc_hint *hint)
{
	struct pool_svc	       *pool_svc;
	int			rc;

	rc = pool_svc_lookup_leader(pool_uuid, &pool_svc, hint);
	if (rc != 0)
		return rc;
	*svcpp = &pool_svc->ps_cont_svc;
	return 0;
}

/**
 * Put container service *\a svcp.
 */
void
ds_pool_cont_svc_put_leader(struct cont_svc **svcp)
{
	struct pool_svc *pool_svc;

	pool_svc = container_of(svcp, struct pool_svc, ps_cont_svc);
	pool_svc_put_leader(pool_svc);
}

/**
 * Return the container service term.
 */
uint64_t
ds_pool_cont_svc_term(struct cont_svc **svcp)
{
	struct pool_svc *pool_svc;

	pool_svc = container_of(svcp, struct pool_svc, ps_cont_svc);
	return pool_svc->ps_term;
}

int
ds_pool_svc_start(const uuid_t uuid)
{
	d_list_t	       *entry;
	struct pool_svc	       *svc;
	int			rc;

	ABT_mutex_lock(pool_svc_hash_lock);

	entry = d_hash_rec_find(&pool_svc_hash, uuid, sizeof(uuid_t));
	if (entry != NULL) {
		svc = pool_svc_obj(entry);
		D_GOTO(out_ref, rc = 0);
	}

	D_ALLOC_PTR(svc);
	if (svc == NULL)
		D_GOTO(err_lock, rc = -DER_NOMEM);

	rc = pool_svc_init(svc, uuid);
	if (rc != 0)
		D_GOTO(err_svc, rc);

	rc = d_hash_rec_insert(&pool_svc_hash, uuid, sizeof(uuid_t),
			       &svc->ps_entry, true /* exclusive */);
	if (rc != 0)
		D_GOTO(err_svc_init, rc);

out_ref:
	d_hash_rec_decref(&pool_svc_hash, &svc->ps_entry);
	ABT_mutex_unlock(pool_svc_hash_lock);
	D_DEBUG(DF_DSMS, DF_UUID": started pool service\n", DP_UUID(uuid));
	return 0;

err_svc_init:
	pool_svc_fini(svc);
err_svc:
	D_FREE(svc);
err_lock:
	ABT_mutex_unlock(pool_svc_hash_lock);
	D_ERROR(DF_UUID": failed to start pool service\n", DP_UUID(uuid));
	return rc;
}

static void
pool_svc_stop(struct pool_svc *svc)
{
	ABT_mutex_lock(svc->ps_mutex);

	if (svc->ps_stop) {
		D_DEBUG(DF_DSMS, DF_UUID": already stopping\n",
			 DP_UUID(svc->ps_uuid));
		ABT_mutex_unlock(svc->ps_mutex);
		return;
	}
	D_DEBUG(DF_DSMS, DF_UUID": stopping pool service\n",
		 DP_UUID(svc->ps_uuid));
	svc->ps_stop = true;

	if (svc->ps_state == POOL_SVC_UP ||
	    svc->ps_state == POOL_SVC_UP_EMPTY)
		/*
		 * The service has stepped up. If it is still the leader of
		 * svc->ps_term, the following rdb_resign() call will trigger
		 * the matching pool_svc_step_down_cb() callback in
		 * svc->ps_term; otherwise, the callback must already be
		 * pending. Either way, the service shall eventually enter the
		 * POOL_SVC_DOWN state.
		 */
		rdb_resign(svc->ps_db, svc->ps_term);
	while (svc->ps_state != POOL_SVC_DOWN)
		ABT_cond_wait(svc->ps_state_cv, svc->ps_mutex);

	ABT_mutex_unlock(svc->ps_mutex);

	ABT_mutex_lock(pool_svc_hash_lock);
	d_hash_rec_delete_at(&pool_svc_hash, &svc->ps_entry);
	ABT_mutex_unlock(pool_svc_hash_lock);
}

void
ds_pool_svc_stop(const uuid_t uuid)
{
	struct pool_svc	       *svc;
	int			rc;

	rc = pool_svc_lookup(uuid, &svc);
	if (rc != 0)
		return;
	pool_svc_stop(svc);
	pool_svc_put(svc);
}

/*
 * Try to start a pool's pool service if its RDB exists. Continue the iteration
 * upon errors as other pools may still be able to work.
 */
static int
start_one(const uuid_t uuid, void *arg)
{
	char   *path;
	int	rc;

	/*
	 * Check if an RDB file exists and we can access it, to avoid
	 * unnecessary error messages from the ds_pool_svc_start() call.
	 */
	path = ds_pool_svc_rdb_path(uuid);
	if (path == NULL) {
		D_ERROR(DF_UUID": failed allocate rdb path\n", DP_UUID(uuid));
		return 0;
	}
	rc = access(path, R_OK | W_OK);
	D_FREE(path);
	if (rc != 0) {
		D_DEBUG(DB_MD, DF_UUID": cannot find or access rdb: %d\n",
			DP_UUID(uuid), errno);
		return 0;
	}

	rc = ds_pool_svc_start(uuid);
	if (rc != 0) {
		D_ERROR("not starting pool service "DF_UUID": %d\n",
			DP_UUID(uuid), rc);
		return 0;
	}

	D_DEBUG(DB_MD, "started pool service "DF_UUID"\n", DP_UUID(uuid));
	return 0;
}

void
pool_svc_start_all(void *arg)
{
	int rc;

	/* Scan the storage and start all pool services. */
	rc = ds_mgmt_tgt_pool_iterate(start_one, NULL /* arg */);
	if (rc != 0)
		D_ERROR("failed to scan all pool services: %d\n", rc);
}

/* Note that this function is currently called from the main xstream. */
int
ds_pool_svc_start_all(void)
{
	ABT_thread	thread;
	int		rc;

	/* Create a ULT to call ds_pool_svc_start() in xstream 0. */
	rc = dss_ult_create(pool_svc_start_all, NULL, 0, 0, &thread);
	if (rc != 0) {
		D_ERROR("failed to create pool service start ULT: %d\n", rc);
		return rc;
	}
	ABT_thread_join(thread);
	ABT_thread_free(&thread);
	return 0;
}

struct ult {
	d_list_t	u_entry;
	ABT_thread	u_thread;
};

static int
stop_one(d_list_t *entry, void *arg)
{
	struct pool_svc	       *svc = pool_svc_obj(entry);
	d_list_t	       *list = arg;
	struct ult	       *ult;
	int			rc;

	D_ALLOC_PTR(ult);
	if (ult == NULL)
		return -DER_NOMEM;

	d_hash_rec_addref(&pool_svc_hash, &svc->ps_entry);
	rc = dss_ult_create(pool_svc_stopper, svc, 0, 0, &ult->u_thread);
	if (rc != 0) {
		d_hash_rec_decref(&pool_svc_hash, &svc->ps_entry);
		D_FREE(ult);
		return rc;
	}

	d_list_add(&ult->u_entry, list);
	return 0;
}

/*
 * Note that this function is currently called from the main xstream to save
 * one ULT creation.
 */
int
ds_pool_svc_stop_all(void)
{
	d_list_t	list = D_LIST_HEAD_INIT(list);
	struct ult     *ult;
	struct ult     *ult_tmp;
	int		rc;

	/* Create a stopper ULT for each pool service. */
	ABT_mutex_lock(pool_svc_hash_lock);
	rc = d_hash_table_traverse(&pool_svc_hash, stop_one, &list);
	ABT_mutex_unlock(pool_svc_hash_lock);

	/* Wait for the stopper ULTs to return. */
	d_list_for_each_entry_safe(ult, ult_tmp, &list, u_entry) {
		d_list_del_init(&ult->u_entry);
		ABT_thread_join(ult->u_thread);
		ABT_thread_free(&ult->u_thread);
		D_FREE(ult);
	}

	if (rc != 0)
		D_ERROR("failed to stop all pool services: %d\n", rc);
	return rc;
}

static int
bcast_create(crt_context_t ctx, struct pool_svc *svc, crt_opcode_t opcode,
	     crt_rpc_t **rpc)
{
	return ds_pool_bcast_create(ctx, svc->ps_pool, DAOS_POOL_MODULE, opcode,
				    rpc, NULL, NULL);
}

/**
 * Retrieve the latest leader hint from \a db and fill it into \a hint.
 *
 * \param[in]	db	database
 * \param[out]	hint	rsvc hint
 */
void
ds_pool_set_hint(struct rdb *db, struct rsvc_hint *hint)
{
	int rc;

	rc = rdb_get_leader(db, &hint->sh_term, &hint->sh_rank);
	if (rc != 0)
		return;
	hint->sh_flags |= RSVC_HINT_VALID;
}

struct pool_attr {
	uint32_t	pa_uid;
	uint32_t	pa_gid;
	uint32_t	pa_mode;
};

static int
pool_attr_read(struct rdb_tx *tx, const struct pool_svc *svc,
	       struct pool_attr *attr)
{
	daos_iov_t	value;
	int		rc;

	daos_iov_set(&value, &attr->pa_uid, sizeof(attr->pa_uid));
	rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_attr_uid, &value);
	if (rc != 0)
		return rc;

	daos_iov_set(&value, &attr->pa_gid, sizeof(attr->pa_gid));
	rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_attr_gid, &value);
	if (rc != 0)
		return rc;

	daos_iov_set(&value, &attr->pa_mode, sizeof(attr->pa_mode));
	rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_attr_mode, &value);
	if (rc != 0)
		return rc;

	D_DEBUG(DF_DSMS, "uid=%u gid=%u mode=%u\n", attr->pa_uid, attr->pa_gid,
		attr->pa_mode);
	return 0;
}

/*
 * We use this RPC to not only create the pool metadata but also initialize the
 * pool/container service DB.
 */
void
ds_pool_create_handler(crt_rpc_t *rpc)
{
	struct pool_create_in  *in = crt_req_get(rpc);
	struct pool_create_out *out = crt_reply_get(rpc);
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	daos_iov_t		value;
	struct rdb_kvs_attr	attr;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->pri_op.pi_uuid), rpc);

	if (in->pri_ntgts != in->pri_tgt_uuids.ca_count ||
	    in->pri_ntgts != in->pri_tgt_ranks->rl_nr)
		D_GOTO(out, rc = -DER_PROTO);
	if (in->pri_ndomains != in->pri_domains.ca_count)
		D_GOTO(out, rc = -DER_PROTO);

	/* This RPC doesn't care about pool_svc_up(). */
	rc = pool_svc_lookup(in->pri_op.pi_uuid, &svc);
	if (rc != 0)
		D_GOTO(out, rc);

	/*
	 * Simply serialize this whole RPC with pool_svc_step_{up,down}_cb()
	 * and pool_svc_stop().
	 */
	ABT_mutex_lock(svc->ps_mutex);

	if (svc->ps_stop) {
		D_DEBUG(DB_MD, DF_UUID": pool service already stopping\n",
			DP_UUID(svc->ps_uuid));
		D_GOTO(out_mutex, rc = -DER_CANCELED);
	}

	rc = rdb_tx_begin(svc->ps_db, RDB_NIL_TERM, &tx);
	if (rc != 0)
		D_GOTO(out_mutex, rc);
	ABT_rwlock_wrlock(svc->ps_lock);
	ds_cont_wrlock_metadata(svc->ps_cont_svc);

	/* See if the DB has already been initialized. */
	daos_iov_set(&value, NULL /* buf */, 0 /* size */);
	rc = rdb_tx_lookup(&tx, &svc->ps_root, &ds_pool_attr_map_buffer,
			   &value);
	if (rc != -DER_NONEXIST) {
		if (rc == 0)
			D_DEBUG(DF_DSMS, DF_UUID": db already initialized\n",
				DP_UUID(svc->ps_uuid));
		else
			D_ERROR(DF_UUID": failed to look up pool map: %d\n",
				DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_tx, rc);
	}

	/* Initialize the DB and the metadata for this pool. */
	attr.dsa_class = RDB_KVS_GENERIC;
	attr.dsa_order = 8;
	rc = rdb_tx_create_root(&tx, &attr);
	if (rc != 0)
		D_GOTO(out_tx, rc);
	rc = init_pool_metadata(&tx, &svc->ps_root, in->pri_uid, in->pri_gid,
				in->pri_mode, in->pri_ntgts,
				in->pri_tgt_uuids.ca_arrays, NULL /* group */,
				in->pri_tgt_ranks, in->pri_ndomains,
				in->pri_domains.ca_arrays);
	if (rc != 0)
		D_GOTO(out_tx, rc);
	rc = ds_cont_init_metadata(&tx, &svc->ps_root, in->pri_op.pi_uuid);
	if (rc != 0)
		D_GOTO(out_tx, rc);

	rc = rdb_tx_commit(&tx);
	if (rc != 0)
		D_GOTO(out_tx, rc);

out_tx:
	ds_cont_unlock_metadata(svc->ps_cont_svc);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	if (svc->ps_state == POOL_SVC_UP_EMPTY) {
		/*
		 * The DB is no longer empty. Since the previous
		 * pool_svc_step_up_cb() call didn't finish stepping up due to
		 * an empty DB, and there hasn't been a pool_svc_step_down_cb()
		 * call yet, we should call pool_svc_step_up() to finish
		 * stepping up.
		 */
		D_DEBUG(DF_DSMS, DF_UUID": trying to finish stepping up\n",
			DP_UUID(in->pri_op.pi_uuid));
		rc = pool_svc_step_up(svc);
		if (rc != 0) {
			D_ASSERT(rc != DER_UNINIT);
			/* TODO: Ask rdb to step down. */
			D_GOTO(out_svc, rc);
		}
		svc->ps_state = POOL_SVC_UP;
	}

out_mutex:
	ABT_mutex_unlock(svc->ps_mutex);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->pro_op.po_hint);
	pool_svc_put(svc);
out:
	out->pro_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pri_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
permitted(const struct pool_attr *attr, uint32_t uid, uint32_t gid,
	  uint64_t capas)
{
	int		shift;
	uint32_t	capas_permitted;

	/*
	 * Determine which set of capability bits applies. See also the
	 * comment/diagram for ds_pool_attr_mode in src/pool/srv_layout.h.
	 */
	if (uid == attr->pa_uid)
		shift = DAOS_PC_NBITS * 2;	/* user */
	else if (gid == attr->pa_gid)
		shift = DAOS_PC_NBITS;		/* group */
	else
		shift = 0;			/* other */

	/* Extract the applicable set of capability bits. */
	capas_permitted = (attr->pa_mode >> shift) & DAOS_PC_MASK;

	/* Only if all requested capability bits are permitted... */
	return (capas & capas_permitted) == capas;
}

static int
pool_connect_bcast(crt_context_t ctx, struct pool_svc *svc,
		   const uuid_t pool_hdl, uint64_t capas,
		   daos_iov_t *global_ns)
{
	struct pool_tgt_connect_in     *in;
	struct pool_tgt_connect_out    *out;
	d_rank_t		       rank;
	crt_rpc_t		       *rpc;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": bcasting\n", DP_UUID(svc->ps_uuid));

	rc = crt_group_rank(svc->ps_pool->sp_group, &rank);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = bcast_create(ctx, svc, POOL_TGT_CONNECT, &rpc);
	if (rc != 0)
		D_GOTO(out, rc);

	in = crt_req_get(rpc);
	uuid_copy(in->tci_uuid, svc->ps_uuid);
	uuid_copy(in->tci_hdl, pool_hdl);
	in->tci_capas = capas;
	in->tci_map_version = pool_map_get_version(svc->ps_pool->sp_map);
	in->tci_iv_ns_id = ds_iv_ns_id_get(svc->ps_pool->sp_iv_ns);
	in->tci_iv_ctxt.iov_buf = global_ns->iov_buf;
	in->tci_iv_ctxt.iov_buf_len = global_ns->iov_buf_len;
	in->tci_iv_ctxt.iov_len = global_ns->iov_len;
	in->tci_master_rank = rank;

	rc = dss_rpc_send(rpc);
	if (rc != 0)
		D_GOTO(out_rpc, rc);

	out = crt_reply_get(rpc);
	rc = out->tco_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to connect to %d targets\n",
			DP_UUID(svc->ps_uuid), rc);
		rc = -DER_IO;
	}

out_rpc:
	crt_req_decref(rpc);
out:
	D_DEBUG(DF_DSMS, DF_UUID": bcasted: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

static int
bulk_cb(const struct crt_bulk_cb_info *cb_info)
{
	ABT_eventual *eventual = cb_info->bci_arg;

	ABT_eventual_set(*eventual, (void *)&cb_info->bci_rc,
			 sizeof(cb_info->bci_rc));
	return 0;
}

/*
 * Transfer the pool map to "remote_bulk". If the remote bulk buffer is too
 * small, then return -DER_TRUNC and set "required_buf_size" to the local pool
 * map buffer size.
 */
static int
transfer_map_buf(struct rdb_tx *tx, struct pool_svc *svc, crt_rpc_t *rpc,
		 crt_bulk_t remote_bulk, uint32_t *required_buf_size)
{
	struct pool_buf	       *map_buf;
	size_t			map_buf_size;
	uint32_t		map_version;
	daos_size_t		remote_bulk_size;
	daos_iov_t		map_iov;
	daos_sg_list_t		map_sgl;
	crt_bulk_t		bulk;
	struct crt_bulk_desc	map_desc;
	crt_bulk_opid_t		map_opid;
	ABT_eventual		eventual;
	int		       *status;
	int			rc;

	rc = read_map_buf(tx, &svc->ps_root, &map_buf, &map_version);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to read pool map: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out, rc);
	}

	if (map_version != pool_map_get_version(svc->ps_pool->sp_map)) {
		D_ERROR(DF_UUID": found different cached and persistent pool "
			"map versions: cached=%u persistent=%u\n",
			DP_UUID(svc->ps_uuid),
			pool_map_get_version(svc->ps_pool->sp_map),
			map_version);
		D_GOTO(out, rc = -DER_IO);
	}

	map_buf_size = pool_buf_size(map_buf->pb_nr);

	/* Check if the client bulk buffer is large enough. */
	rc = crt_bulk_get_len(remote_bulk, &remote_bulk_size);
	if (rc != 0)
		D_GOTO(out, rc);
	if (remote_bulk_size < map_buf_size) {
		D_ERROR(DF_UUID": remote pool map buffer ("DF_U64") < required "
			"(%lu)\n", DP_UUID(svc->ps_uuid), remote_bulk_size,
			map_buf_size);
		*required_buf_size = map_buf_size;
		D_GOTO(out, rc = -DER_TRUNC);
	}

	daos_iov_set(&map_iov, map_buf, map_buf_size);
	map_sgl.sg_nr = 1;
	map_sgl.sg_nr_out = 0;
	map_sgl.sg_iovs = &map_iov;

	rc = crt_bulk_create(rpc->cr_ctx, daos2crt_sg(&map_sgl),
			     CRT_BULK_RO, &bulk);
	if (rc != 0)
		D_GOTO(out, rc);

	/* Prepare "map_desc" for crt_bulk_transfer(). */
	map_desc.bd_rpc = rpc;
	map_desc.bd_bulk_op = CRT_BULK_PUT;
	map_desc.bd_remote_hdl = remote_bulk;
	map_desc.bd_remote_off = 0;
	map_desc.bd_local_hdl = bulk;
	map_desc.bd_local_off = 0;
	map_desc.bd_len = map_iov.iov_len;

	rc = ABT_eventual_create(sizeof(*status), &eventual);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_bulk, rc = dss_abterr2der(rc));

	rc = crt_bulk_transfer(&map_desc, bulk_cb, &eventual, &map_opid);
	if (rc != 0)
		D_GOTO(out_eventual, rc);

	rc = ABT_eventual_wait(eventual, (void **)&status);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_eventual, rc = dss_abterr2der(rc));

	if (*status != 0)
		D_GOTO(out_eventual, rc = *status);

out_eventual:
	ABT_eventual_free(&eventual);
out_bulk:
	crt_bulk_free(bulk);
out:
	return rc;
}

void
ds_pool_connect_handler(crt_rpc_t *rpc)
{
	struct pool_connect_in	       *in = crt_req_get(rpc);
	struct pool_connect_out	       *out = crt_reply_get(rpc);
	struct pool_svc		       *svc;
	struct rdb_tx			tx;
	daos_iov_t			key;
	daos_iov_t			value;
	struct pool_attr		attr;
	struct pool_hdl			hdl;
	daos_iov_t			iv_iov;
	unsigned int			iv_ns_id;
	uint32_t			nhandles;
	int				skip_update = 0;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pci_op.pi_uuid), rpc, DP_UUID(in->pci_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pci_op.pi_uuid, &svc,
				    &out->pco_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	/* sp_iv_ns will be destroyed when pool is destroyed,
	 * see pool_free_ref()
	 */
	D_ASSERT(svc->ps_pool != NULL);
	if (svc->ps_pool->sp_iv_ns == NULL) {
		rc = ds_iv_ns_create(rpc->cr_ctx, NULL,
				     &iv_ns_id, &iv_iov,
				     &svc->ps_pool->sp_iv_ns);
		if (rc)
			D_GOTO(out_svc, rc);
	} else {
		rc = ds_iv_global_ns_get(svc->ps_pool->sp_iv_ns, &iv_iov);
		if (rc)
			D_GOTO(out_svc, rc);
	}

	rc = ds_rebuild_query(in->pci_op.pi_uuid, &out->pco_rebuild_st);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	/* Check existing pool handles. */
	daos_iov_set(&key, in->pci_op.pi_hdl, sizeof(uuid_t));
	daos_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
	if (rc == 0) {
		if (hdl.ph_capas == in->pci_capas) {
			/*
			 * The handle already exists; only do the pool map
			 * transfer.
			 */
			skip_update = 1;
		} else {
			/* The existing one does not match the new one. */
			D_ERROR(DF_UUID": found conflicting pool handle\n",
				DP_UUID(in->pci_op.pi_uuid));
			D_GOTO(out_lock, rc = -DER_EXIST);
		}
	} else if (rc != -DER_NONEXIST) {
		D_GOTO(out_lock, rc);
	}

	rc = pool_attr_read(&tx, svc, &attr);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	if (!permitted(&attr, in->pci_uid, in->pci_gid, in->pci_capas)) {
		D_ERROR(DF_UUID": refusing connect attempt for uid %u gid %u "
			DF_X64"\n", DP_UUID(in->pci_op.pi_uuid), in->pci_uid,
			in->pci_gid, in->pci_capas);
		D_GOTO(out_map_version, rc = -DER_NO_PERM);
	}

	out->pco_uid = attr.pa_uid;
	out->pco_gid = attr.pa_gid;
	out->pco_mode = attr.pa_mode;

	/*
	 * Transfer the pool map to the client before adding the pool handle,
	 * so that we don't need to worry about rolling back the transaction
	 * when the tranfer fails. The client has already been authenticated
	 * and authorized at this point. If an error occurs after the transfer
	 * completes, then we simply return the error and the client will throw
	 * its pool_buf away.
	 */
	rc = transfer_map_buf(&tx, svc, rpc, in->pci_map_bulk,
			      &out->pco_map_buf_size);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	if (skip_update)
		D_GOTO(out_map_version, rc = 0);

	daos_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_lookup(&tx, &svc->ps_root, &ds_pool_attr_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	/* Take care of exclusive handles. */
	if (nhandles != 0) {
		if (in->pci_capas & DAOS_PC_EX) {
			D_DEBUG(DF_DSMS, DF_UUID": others already connected\n",
				DP_UUID(in->pci_op.pi_uuid));
			D_GOTO(out_map_version, rc = -DER_BUSY);
		} else {
			/*
			 * If there is a non-exclusive handle, then all handles
			 * are non-exclusive.
			 */
			daos_iov_set(&value, &hdl, sizeof(hdl));
			rc = rdb_tx_fetch(&tx, &svc->ps_handles,
					  RDB_PROBE_FIRST, NULL /* key_in */,
					  NULL /* key_out */, &value);
			if (rc != 0)
				D_GOTO(out_map_version, rc);
			if (hdl.ph_capas & DAOS_PC_EX)
				D_GOTO(out_map_version, rc = -DER_BUSY);
		}
	}

	rc = pool_connect_bcast(rpc->cr_ctx, svc, in->pci_op.pi_hdl,
				in->pci_capas, &iv_iov);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to connect to targets: %d\n",
			DP_UUID(in->pci_op.pi_uuid), rc);
		D_GOTO(out_map_version, rc);
	}

	hdl.ph_capas = in->pci_capas;
	nhandles++;

	daos_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(&tx, &svc->ps_root, &ds_pool_attr_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	daos_iov_set(&key, in->pci_op.pi_hdl, sizeof(uuid_t));
	daos_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_update(&tx, &svc->ps_handles, &key, &value);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	rc = rdb_tx_commit(&tx);
out_map_version:
	out->pco_op.po_map_version = pool_map_get_version(svc->ps_pool->sp_map);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->pco_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->pco_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pci_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
pool_disconnect_bcast(crt_context_t ctx, struct pool_svc *svc,
		      uuid_t *pool_hdls, int n_pool_hdls)
{
	struct pool_tgt_disconnect_in  *in;
	struct pool_tgt_disconnect_out *out;
	crt_rpc_t		       *rpc;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": bcasting\n", DP_UUID(svc->ps_uuid));

	rc = bcast_create(ctx, svc, POOL_TGT_DISCONNECT, &rpc);
	if (rc != 0)
		D_GOTO(out, rc);

	in = crt_req_get(rpc);
	uuid_copy(in->tdi_uuid, svc->ps_uuid);
	in->tdi_hdls.ca_arrays = pool_hdls;
	in->tdi_hdls.ca_count = n_pool_hdls;
	rc = dss_rpc_send(rpc);
	if (rc != 0)
		D_GOTO(out_rpc, rc);

	out = crt_reply_get(rpc);
	rc = out->tdo_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to disconnect from %d targets\n",
			DP_UUID(svc->ps_uuid), rc);
		rc = -DER_IO;
	}

out_rpc:
	crt_req_decref(rpc);
out:
	D_DEBUG(DF_DSMS, DF_UUID": bcasted: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

static int
pool_disconnect_hdls(struct rdb_tx *tx, struct pool_svc *svc, uuid_t *hdl_uuids,
		     int n_hdl_uuids, crt_context_t ctx)
{
	daos_iov_t	value;
	uint32_t	nhandles;
	int		i;
	int		rc;

	D_ASSERTF(n_hdl_uuids > 0, "%d\n", n_hdl_uuids);

	D_DEBUG(DF_DSMS, DF_UUID": disconnecting %d hdls: hdl_uuids[0]="DF_UUID
		"\n", DP_UUID(svc->ps_uuid), n_hdl_uuids,
		DP_UUID(hdl_uuids[0]));

	/*
	 * TODO: Send POOL_TGT_CLOSE_CONTS and somehow retry until every
	 * container service has responded (through ds_pool).
	 */
	rc = ds_cont_close_by_pool_hdls(svc->ps_uuid, hdl_uuids, n_hdl_uuids,
					ctx);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = pool_disconnect_bcast(ctx, svc, hdl_uuids, n_hdl_uuids);
	if (rc != 0)
		D_GOTO(out, rc);

	daos_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_attr_nhandles, &value);
	if (rc != 0)
		D_GOTO(out, rc);

	nhandles -= n_hdl_uuids;

	for (i = 0; i < n_hdl_uuids; i++) {
		daos_iov_t key;

		daos_iov_set(&key, hdl_uuids[i], sizeof(uuid_t));
		rc = rdb_tx_delete(tx, &svc->ps_handles, &key);
		if (rc != 0)
			D_GOTO(out, rc);
	}

	daos_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(tx, &svc->ps_root, &ds_pool_attr_nhandles, &value);
	if (rc != 0)
		D_GOTO(out, rc);

out:
	D_DEBUG(DF_DSMS, DF_UUID": leaving: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

void
ds_pool_disconnect_handler(crt_rpc_t *rpc)
{
	struct pool_disconnect_in      *pdi = crt_req_get(rpc);
	struct pool_disconnect_out     *pdo = crt_reply_get(rpc);
	struct pool_svc		       *svc;
	struct rdb_tx			tx;
	daos_iov_t			key;
	daos_iov_t			value;
	struct pool_hdl			hdl;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(pdi->pdi_op.pi_uuid), rpc, DP_UUID(pdi->pdi_op.pi_hdl));

	rc = pool_svc_lookup_leader(pdi->pdi_op.pi_uuid, &svc,
				    &pdo->pdo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	daos_iov_set(&key, pdi->pdi_op.pi_hdl, sizeof(uuid_t));
	daos_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
	if (rc != 0) {
		if (rc == -DER_NONEXIST)
			rc = 0;
		D_GOTO(out_lock, rc);
	}

	rc = pool_disconnect_hdls(&tx, svc, &pdi->pdi_op.pi_hdl,
				  1 /* n_hdl_uuids */, rpc->cr_ctx);
	if (rc != 0)
		D_GOTO(out_lock, rc);

	rc = rdb_tx_commit(&tx);
	/* No need to set pdo->pdo_op.po_map_version. */
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &pdo->pdo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	pdo->pdo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(pdi->pdi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_query_handler(crt_rpc_t *rpc)
{
	struct pool_query_in   *in = crt_req_get(rpc);
	struct pool_query_out  *out = crt_reply_get(rpc);
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	daos_iov_t		key;
	daos_iov_t		value;
	struct pool_hdl		hdl;
	struct pool_attr	attr;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pqi_op.pi_uuid), rpc, DP_UUID(in->pqi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pqi_op.pi_uuid, &svc,
				    &out->pqo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = ds_rebuild_query(in->pqi_op.pi_uuid, &out->pqo_rebuild_st);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);

	/* Verify the pool handle. Note: since rebuild will not
	 * connect the pool, so we only verify the non-rebuild
	 * pool.
	 */
	if (!is_rebuild_pool(in->pqi_op.pi_uuid, in->pqi_op.pi_hdl)) {
		daos_iov_set(&key, in->pqi_op.pi_hdl, sizeof(uuid_t));
		daos_iov_set(&value, &hdl, sizeof(hdl));
		rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
		if (rc != 0) {
			if (rc == -DER_NONEXIST)
				rc = -DER_NO_HDL;
			D_GOTO(out_lock, rc);
		}
	}

	rc = pool_attr_read(&tx, svc, &attr);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	out->pqo_uid = attr.pa_uid;
	out->pqo_gid = attr.pa_gid;
	out->pqo_mode = attr.pa_mode;

	rc = transfer_map_buf(&tx, svc, rpc, in->pqi_map_bulk,
			      &out->pqo_map_buf_size);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

out_map_version:
	out->pqo_op.po_map_version = pool_map_get_version(svc->ps_pool->sp_map);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->pqo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->pqo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pqi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
pool_map_update(crt_context_t ctx, struct pool_svc *svc,
		uint32_t map_version, struct pool_buf *buf)
{
	struct pool_iv_entry	*iv_entry;
	uint32_t		size;
	int			rc;

	/* If iv_ns is NULL, it means the pool is not connected,
	 * then we do not need distribute pool map to all other
	 * servers. NB: rebuild will redistribute the pool map
	 * by itself anyway.
	 */
	if (svc->ps_pool->sp_iv_ns == NULL)
		return 0;

	D_DEBUG(DF_DSMS, DF_UUID": update ver %d pb_nr %d\n",
		 DP_UUID(svc->ps_uuid), map_version, buf->pb_nr);

	size = pool_iv_ent_size(buf->pb_nr);
	D_ALLOC(iv_entry, size);
	if (iv_entry == NULL)
		return -DER_NOMEM;

	crt_group_rank(svc->ps_pool->sp_group, &iv_entry->piv_master_rank);
	uuid_copy(iv_entry->piv_pool_uuid, svc->ps_uuid);
	iv_entry->piv_pool_map_ver = map_version;
	memcpy(&iv_entry->piv_pool_buf, buf, pool_buf_size(buf->pb_nr));
	rc = pool_iv_update(svc->ps_pool->sp_iv_ns, iv_entry,
			    CRT_IV_SHORTCUT_NONE, CRT_IV_SYNC_LAZY);

	/* Some nodes ivns does not exist, might because of the disconnection,
	 * let's ignore it
	 */
	if (rc == -DER_NONEXIST)
		rc = 0;

	D_FREE(iv_entry);

	return rc;
}

/* Callers are responsible for daos_rank_list_free(*replicasp). */
static int
ds_pool_update_internal(uuid_t pool_uuid, struct pool_target_id_list *tgts,
			unsigned int opc,
			struct pool_op_out *pto_op, bool *p_updated,
			d_rank_list_t **replicasp)
{
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	struct pool_map	       *map;
	uint32_t		map_version_before;
	uint32_t		map_version = 0;
	struct pool_buf	       *map_buf = NULL;
	struct pool_map	       *map_tmp;
	bool			updated = false;
	struct dss_module_info *info = dss_get_module_info();
	int			rc;

	rc = pool_svc_lookup_leader(pool_uuid, &svc,
				    pto_op == NULL ? NULL : &pto_op->po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);
	ABT_rwlock_wrlock(svc->ps_lock);

	if (replicasp != NULL) {
		rc = rdb_get_ranks(svc->ps_db, replicasp);
		if (rc != 0)
			D_GOTO(out_map_version, rc);
	}

	/* Create a temporary pool map based on the last committed version. */
	rc = read_map(&tx, &svc->ps_root, &map);
	if (rc != 0)
		D_GOTO(out_replicas, rc);

	/*
	 * Attempt to modify the temporary pool map and save its versions
	 * before and after. If the version hasn't changed, we are done.
	 */
	map_version_before = pool_map_get_version(map);
	rc = ds_pool_map_tgts_update(map, tgts, opc);
	if (rc != 0)
		D_GOTO(out_map, rc);
	map_version = pool_map_get_version(map);

	D_DEBUG(DF_DSMS, DF_UUID": version=%u->%u\n",
		DP_UUID(svc->ps_uuid), map_version_before, map_version);
	if (map_version == map_version_before)
		D_GOTO(out_map, rc = 0);

	/* Write the new pool map. */
	rc = pool_buf_extract(map, &map_buf);
	if (rc != 0)
		D_GOTO(out_map, rc);
	rc = write_map_buf(&tx, &svc->ps_root, map_buf, map_version);
	if (rc != 0)
		D_GOTO(out_map, rc);

	rc = rdb_tx_commit(&tx);
	if (rc != 0) {
		D_DEBUG(DB_MD, DF_UUID": failed to commit: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_map, rc);
	}

	updated = true;

	/*
	 * The new pool map is now committed and can be publicized. Swap the
	 * new pool map with the old one in the cache.
	 */
	ABT_rwlock_wrlock(svc->ps_pool->sp_lock);
	map_tmp = svc->ps_pool->sp_map;
	svc->ps_pool->sp_map = map;
	map = map_tmp;
	svc->ps_pool->sp_map_version = map_version;
	ABT_rwlock_unlock(svc->ps_pool->sp_lock);

out_map:
	pool_map_decref(map);
out_replicas:
	if (rc) {
		daos_rank_list_free(*replicasp);
		*replicasp = NULL;
	}
out_map_version:
	if (pto_op != NULL)
		pto_op->po_map_version =
			pool_map_get_version(svc->ps_pool->sp_map);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);

	/*
	 * Distribute pool map to other targets, and ignore the return code
	 * as we are more about committing a pool map change than its
	 * dissemination.
	 */
	if (updated)
		pool_map_update(info->dmi_ctx, svc, map_version, map_buf);

	if (map_buf != NULL)
		pool_buf_free(map_buf);
out_svc:
	if (pto_op != NULL)
		ds_pool_set_hint(svc->ps_db, &pto_op->po_hint);
	pool_svc_put_leader(svc);
out:
	if (p_updated)
		*p_updated = updated;
	return rc;
}

static int
pool_find_all_targets_by_addr(uuid_t pool_uuid,
			struct pool_target_addr_list *list,
			struct pool_target_id_list *tgt_list,
			struct pool_target_addr_list *out_list)
{
	struct pool_svc	*svc;
	struct rdb_tx	tx;
	struct pool_map *map = NULL;
	int		i;
	int		rc;

	rc = pool_svc_lookup_leader(pool_uuid, &svc, NULL);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);
	ABT_rwlock_rdlock(svc->ps_lock);

	/* Create a temporary pool map based on the last committed version. */
	rc = read_map(&tx, &svc->ps_root, &map);

	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	for (i = 0; i < list->pta_number; i++) {
		struct pool_target *tgt;
		int tgt_nr;
		int j;
		int ret;

		tgt_nr = pool_map_find_target_by_rank_idx(map,
				list->pta_addrs[i].pta_rank,
				list->pta_addrs[i].pta_target, &tgt);
		if (tgt_nr <= 0) {
			/* Can not locate the target in pool map, let's
			 * add it to the output list
			 */
			ret = pool_target_addr_list_append(out_list,
						&list->pta_addrs[i]);
			if (ret) {
				rc = ret;
				break;
			}
		}

		for (j = 0; j < tgt_nr; j++) {
			struct pool_target_id tid;

			tid.pti_id = tgt[j].ta_comp.co_id;
			ret = pool_target_id_list_append(tgt_list, &tid);
			if (ret) {
				rc = ret;
				break;
			}
		}
	}
out_svc:
	pool_svc_put_leader(svc);
out:
	if (map != NULL)
		pool_map_decref(map);
	return rc;
}

int
ds_pool_tgt_exclude_out(uuid_t pool_uuid, struct pool_target_id_list *list)
{
	return ds_pool_update_internal(pool_uuid, list, POOL_EXCLUDE_OUT,
				       NULL, NULL, NULL);
}

int
ds_pool_tgt_exclude(uuid_t pool_uuid, struct pool_target_id_list *list)
{
	return ds_pool_update_internal(pool_uuid, list, POOL_EXCLUDE,
				       NULL, NULL, NULL);
}

void
ds_pool_update_handler(crt_rpc_t *rpc)
{
	struct pool_tgt_update_in	*in = crt_req_get(rpc);
	struct pool_tgt_update_out	*out = crt_reply_get(rpc);
	struct pool_target_addr_list	list = { 0 };
	struct pool_target_addr_list	out_list = { 0 };
	struct pool_target_id_list	target_list = { 0 };
	d_rank_list_t			*replicas = NULL;
	bool				updated;
	int				rc;

	if (in->pti_addr_list.ca_arrays == NULL ||
	    in->pti_addr_list.ca_count == 0)
		D_GOTO(out, rc = -DER_INVAL);

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: ntargets=%zu\n",
		DP_UUID(in->pti_op.pi_uuid), rpc, in->pti_addr_list.ca_count);

	/* Convert target address list to target id list */
	list.pta_number = in->pti_addr_list.ca_count;
	list.pta_addrs = in->pti_addr_list.ca_arrays;
	rc = pool_find_all_targets_by_addr(in->pti_op.pi_uuid, &list,
					   &target_list, &out_list);
	if (rc)
		D_GOTO(out, rc);

	/* Update target by target id */
	rc = ds_pool_update_internal(in->pti_op.pi_uuid, &target_list,
				     opc_get(rpc->cr_opc),
				     &out->pto_op, &updated, &replicas);
	if (rc)
		D_GOTO(out, rc);

	out->pto_addr_list.ca_arrays = out_list.pta_addrs;
	out->pto_addr_list.ca_count = out_list.pta_number;

out:
	out->pto_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pti_op.pi_uuid), rpc, rc);
	rc = crt_reply_send(rpc);

	if (out->pto_op.po_rc == 0 && updated &&
	    opc_get(rpc->cr_opc) == POOL_EXCLUDE) {
		char	*env;
		int	 ret;

		env = getenv(REBUILD_ENV);
		if ((env && !strcasecmp(env, REBUILD_ENV_DISABLED)) ||
		    daos_fail_check(DAOS_REBUILD_DISABLE)) {
			D_DEBUG(DB_TRACE, "Rebuild is disabled\n");
		} else { /* enabled by default */
			D_ASSERT(replicas != NULL);
			ret = ds_rebuild_schedule(in->pti_op.pi_uuid,
				out->pto_op.po_map_version,
				&target_list, replicas);
			if (ret != 0) {
				D_ERROR("rebuild fails rc %d\n", ret);
				if (rc == 0)
					rc = ret;
			}
		}
	}

	pool_target_addr_list_free(&out_list);
	pool_target_id_list_free(&target_list);
	if (replicas != NULL)
		daos_rank_list_free(replicas);
}

struct evict_iter_arg {
	uuid_t *eia_hdl_uuids;
	size_t	eia_hdl_uuids_size;
	int	eia_n_hdl_uuids;
};

static int
evict_iter_cb(daos_handle_t ih, daos_iov_t *key, daos_iov_t *val, void *varg)
{
	struct evict_iter_arg  *arg = varg;

	D_ASSERT(arg->eia_hdl_uuids != NULL);
	D_ASSERT(arg->eia_hdl_uuids_size > sizeof(uuid_t));

	if (key->iov_len != sizeof(uuid_t) ||
	    val->iov_len != sizeof(struct pool_hdl)) {
		D_ERROR("invalid key/value size: key="DF_U64" value="DF_U64"\n",
			key->iov_len, val->iov_len);
		return -DER_IO;
	}

	/*
	 * Make sure arg->eia_hdl_uuids[arg->eia_hdl_uuids_size] have enough
	 * space for this handle.
	 */
	if (sizeof(uuid_t) * (arg->eia_n_hdl_uuids + 1) >
	    arg->eia_hdl_uuids_size) {
		uuid_t *hdl_uuids_tmp;
		size_t	hdl_uuids_size_tmp;

		hdl_uuids_size_tmp = arg->eia_hdl_uuids_size * 2;
		D_ALLOC(hdl_uuids_tmp, hdl_uuids_size_tmp);
		if (hdl_uuids_tmp == NULL)
			return -DER_NOMEM;
		memcpy(hdl_uuids_tmp, arg->eia_hdl_uuids,
		       arg->eia_hdl_uuids_size);
		D_FREE(arg->eia_hdl_uuids);
		arg->eia_hdl_uuids = hdl_uuids_tmp;
		arg->eia_hdl_uuids_size = hdl_uuids_size_tmp;
	}

	uuid_copy(arg->eia_hdl_uuids[arg->eia_n_hdl_uuids], key->iov_buf);
	arg->eia_n_hdl_uuids++;
	return 0;
}

/*
 * Callers are responsible for freeing *hdl_uuids if this function returns zero.
 */
static int
find_hdls_to_evict(struct rdb_tx *tx, struct pool_svc *svc, uuid_t **hdl_uuids,
		   size_t *hdl_uuids_size, int *n_hdl_uuids)
{
	struct evict_iter_arg	arg;
	int			rc;

	arg.eia_hdl_uuids_size = sizeof(uuid_t) * 4;
	D_ALLOC(arg.eia_hdl_uuids, arg.eia_hdl_uuids_size);
	if (arg.eia_hdl_uuids == NULL)
		return -DER_NOMEM;
	arg.eia_n_hdl_uuids = 0;

	rc = rdb_tx_iterate(tx, &svc->ps_handles, false /* backward */,
			    evict_iter_cb, &arg);
	if (rc != 0) {
		D_FREE(arg.eia_hdl_uuids);
		return rc;
	}

	*hdl_uuids = arg.eia_hdl_uuids;
	*hdl_uuids_size = arg.eia_hdl_uuids_size;
	*n_hdl_uuids = arg.eia_n_hdl_uuids;
	return 0;
}

void
ds_pool_evict_handler(crt_rpc_t *rpc)
{
	struct pool_evict_in   *in = crt_req_get(rpc);
	struct pool_evict_out  *out = crt_reply_get(rpc);
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	uuid_t		       *hdl_uuids;
	size_t			hdl_uuids_size;
	int			n_hdl_uuids;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->pvi_op.pi_uuid), rpc);

	rc = pool_svc_lookup_leader(in->pvi_op.pi_uuid, &svc,
				    &out->pvo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	rc = find_hdls_to_evict(&tx, svc, &hdl_uuids, &hdl_uuids_size,
				&n_hdl_uuids);
	if (rc != 0)
		D_GOTO(out_lock, rc);

	if (n_hdl_uuids > 0)
		rc = pool_disconnect_hdls(&tx, svc, hdl_uuids, n_hdl_uuids,
					  rpc->cr_ctx);

	rc = rdb_tx_commit(&tx);
	/* No need to set out->pvo_op.po_map_version. */
	D_FREE(hdl_uuids);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->pvo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->pvo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pvi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_svc_stop_handler(crt_rpc_t *rpc)
{
	struct pool_svc_stop_in	       *in = crt_req_get(rpc);
	struct pool_svc_stop_out       *out = crt_reply_get(rpc);
	struct pool_svc		       *svc;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->psi_op.pi_uuid), rpc);

	rc = pool_svc_lookup(in->psi_op.pi_uuid, &svc);
	if (rc != 0)
		D_GOTO(out, rc);
	if (!pool_svc_up(svc))
		D_GOTO(out_svc, rc = -DER_NOTLEADER);

	pool_svc_stop(svc);

out_svc:
	ds_pool_set_hint(svc->ps_db, &out->pso_op.po_hint);
	pool_svc_put(svc);
out:
	out->pso_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->psi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

/**
 * update pool map to all servers.
 **/
int
ds_pool_map_buf_get(const uuid_t uuid, d_iov_t *iov,
		    uint32_t *map_version)
{
	struct pool_svc	*svc;
	struct rdb_tx	tx;
	struct pool_buf	*map_buf;
	int		rc;

	rc = pool_svc_lookup_leader(uuid, &svc, NULL /* hint */);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);
	rc = read_map_buf(&tx, &svc->ps_root, &map_buf, map_version);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to read pool map: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_lock, rc);
	}
	D_ASSERT(map_buf != NULL);
	iov->iov_buf = map_buf;
	iov->iov_len = pool_buf_size(map_buf->pb_nr);
	iov->iov_buf_len = pool_buf_size(map_buf->pb_nr);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	pool_svc_put_leader(svc);
out:
	return rc;
}

/* Try to create iv namespace for the pool */
int
ds_pool_iv_ns_update(struct ds_pool *pool, unsigned int master_rank,
		     d_iov_t *iv_iov, unsigned int iv_ns_id)
{
	struct ds_iv_ns	*ns;
	int		rc;

	if (pool->sp_iv_ns != NULL &&
	    pool->sp_iv_ns->iv_master_rank != master_rank) {
		/* If root has been changed, let's destroy the
		 * previous IV ns
		 */
		ds_iv_ns_destroy(pool->sp_iv_ns);
		pool->sp_iv_ns = NULL;
	}

	if (pool->sp_iv_ns != NULL)
		return 0;

	if (iv_iov == NULL) {
		d_iov_t tmp;

		/* master node */
		rc = ds_iv_ns_create(dss_get_module_info()->dmi_ctx,
				     pool->sp_group, &iv_ns_id, &tmp, &ns);
	} else {
		/* other node */
		rc = ds_iv_ns_attach(dss_get_module_info()->dmi_ctx,
				     iv_ns_id, master_rank, iv_iov, &ns);
	}

	if (rc) {
		D_ERROR("pool "DF_UUID" iv ns create failed %d\n",
			 DP_UUID(pool->sp_uuid), rc);
		return rc;
	}

	pool->sp_iv_ns = ns;

	return rc;
}

int
ds_pool_svc_term_get(const uuid_t uuid, uint64_t *term)
{
	struct pool_svc	*svc;
	int		rc;

	rc = pool_svc_lookup_leader(uuid, &svc, NULL /* hint */);
	if (rc != 0)
		return rc;

	*term = svc->ps_term;

	pool_svc_put_leader(svc);
	return 0;
}

static int
attr_bulk_transfer(crt_rpc_t *rpc, crt_bulk_op_t op,
		   crt_bulk_t local_bulk, crt_bulk_t remote_bulk,
		   off_t local_off, off_t remote_off, size_t length)
{
	ABT_eventual		 eventual;
	int			*status;
	int			 rc;
	struct crt_bulk_desc	 bulk_desc = {
				.bd_rpc		= rpc,
				.bd_bulk_op	= op,
				.bd_local_hdl	= local_bulk,
				.bd_local_off	= local_off,
				.bd_remote_hdl	= remote_bulk,
				.bd_remote_off	= remote_off,
				.bd_len		= length,
			};

	rc = ABT_eventual_create(sizeof(*status), &eventual);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	rc = crt_bulk_transfer(&bulk_desc, bulk_cb, &eventual, NULL);
	if (rc != 0)
		D_GOTO(out_eventual, rc);

	rc = ABT_eventual_wait(eventual, (void **)&status);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_eventual, rc = dss_abterr2der(rc));
	if (*status != 0)
		D_GOTO(out_eventual, rc = *status);

out_eventual:
	ABT_eventual_free(&eventual);
out:
	return rc;
}

void
ds_pool_attr_set_handler(crt_rpc_t *rpc)
{
	struct pool_attr_set_in  *in = crt_req_get(rpc);
	struct pool_op_out	 *out = crt_reply_get(rpc);
	struct pool_svc		 *svc;
	struct rdb_tx		  tx;
	crt_bulk_t		  local_bulk;
	daos_size_t		  bulk_size;
	daos_iov_t		  iov;
	daos_sg_list_t		  sgl;
	void			 *data;
	char			 *names;
	char			 *values;
	size_t			 *sizes;
	int			  rc;
	int			  i;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pasi_op.pi_uuid), rpc, DP_UUID(in->pasi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pasi_op.pi_uuid, &svc, &out->po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	rc = crt_bulk_get_len(in->pasi_bulk, &bulk_size);
	if (rc != 0)
		D_GOTO(out_lock, rc);
	D_DEBUG(DF_DSMS, DF_UUID": count=%lu, size=%lu\n",
		DP_UUID(in->pasi_op.pi_uuid), in->pasi_count, bulk_size);

	D_ALLOC(data, bulk_size);
	if (data == NULL)
		D_GOTO(out_lock, rc = -DER_NOMEM);

	sgl.sg_nr = 1;
	sgl.sg_nr_out = sgl.sg_nr;
	sgl.sg_iovs = &iov;
	daos_iov_set(&iov, data, bulk_size);
	rc = crt_bulk_create(rpc->cr_ctx, daos2crt_sg(&sgl),
			     CRT_BULK_RW, &local_bulk);
	if (rc != 0)
		D_GOTO(out_mem, rc);

	rc = attr_bulk_transfer(rpc, CRT_BULK_GET, local_bulk,
				in->pasi_bulk, 0, 0, bulk_size);
	if (rc != 0)
		D_GOTO(out_bulk, rc);

	names = data;
	/* go to the end of names array */
	for (values = names, i = 0; i < in->pasi_count; ++values)
		if (*values == '\0')
			++i;
	sizes = (size_t *)values;
	values = (char *)(sizes + in->pasi_count);

	for (i = 0; i < in->pasi_count; i++) {
		size_t len;
		daos_iov_t key;
		daos_iov_t value;

		len = strlen(names) /* trailing '\0' */ + 1;
		daos_iov_set(&key, names, len);
		names += len;
		daos_iov_set(&value, values, sizes[i]);
		values += sizes[i];

		rc = rdb_tx_update(&tx, &svc->ps_user, &key, &value);
		if (rc != 0) {
			D_ERROR(DF_UUID": failed to update attribute "
				 "'%s': %d\n", DP_UUID(svc->ps_uuid),
				 (char *) key.iov_buf, rc);
			D_GOTO(out_bulk, rc);
		}
	}
	rc = rdb_tx_commit(&tx);

out_bulk:
	crt_bulk_free(local_bulk);
out_mem:
	D_FREE(data);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->po_hint);
	pool_svc_put_leader(svc);
out:
	out->po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pasi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_attr_get_handler(crt_rpc_t *rpc)
{
	struct pool_attr_get_in  *in = crt_req_get(rpc);
	struct pool_op_out	 *out = crt_reply_get(rpc);
	struct pool_svc		 *svc;
	struct rdb_tx		  tx;
	crt_bulk_t		  local_bulk;
	daos_size_t		  bulk_size;
	daos_size_t		  input_size;
	daos_iov_t		 *iovs;
	daos_sg_list_t		  sgl;
	void			 *data;
	char			 *names;
	size_t			 *sizes;
	int			  rc;
	int			  i;
	int			  j;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pagi_op.pi_uuid), rpc, DP_UUID(in->pagi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pagi_op.pi_uuid, &svc, &out->po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);


	rc = crt_bulk_get_len(in->pagi_bulk, &bulk_size);
	if (rc != 0)
		D_GOTO(out_lock, rc);
	D_DEBUG(DF_DSMS, DF_UUID": count=%lu, key_length=%lu, size=%lu\n",
		DP_UUID(in->pagi_op.pi_uuid),
		in->pagi_count, in->pagi_key_length, bulk_size);

	input_size = in->pagi_key_length + in->pagi_count * sizeof(*sizes);
	D_ASSERT(input_size <= bulk_size);

	D_ALLOC(data, input_size);
	if (data == NULL)
		D_GOTO(out_lock, rc = -DER_NOMEM);

	/* for output sizes */
	D_ALLOC_ARRAY(iovs, (int)(1 + in->pagi_count));
	if (iovs == NULL)
		D_GOTO(out_data, rc = -DER_NOMEM);

	sgl.sg_nr = 1;
	sgl.sg_nr_out = sgl.sg_nr;
	sgl.sg_iovs = &iovs[0];
	daos_iov_set(&iovs[0], data, input_size);
	rc = crt_bulk_create(rpc->cr_ctx, daos2crt_sg(&sgl),
			     CRT_BULK_RW, &local_bulk);
	if (rc != 0)
		D_GOTO(out_iovs, rc);

	rc = attr_bulk_transfer(rpc, CRT_BULK_GET, local_bulk,
				in->pagi_bulk, 0, 0, input_size);
	crt_bulk_free(local_bulk);
	if (rc != 0)
		D_GOTO(out_iovs, rc);

	names = data;
	sizes = (size_t *)(names + in->pagi_key_length);
	daos_iov_set(&iovs[0], (void *)sizes,
		     in->pagi_count * sizeof(*sizes));

	for (i = 0, j = 1; i < in->pagi_count; ++i) {
		size_t len;
		daos_iov_t key;

		len = strlen(names) + /* trailing '\0' */ 1;
		daos_iov_set(&key, names, len);
		names += len;
		daos_iov_set(&iovs[j], NULL, 0);

		rc = rdb_tx_lookup(&tx, &svc->ps_user, &key, &iovs[j]);

		if (rc != 0) {
			D_ERROR(DF_UUID": failed to lookup attribute "
				 "'%s': %d\n", DP_UUID(svc->ps_uuid),
				 (char *) key.iov_buf, rc);
			D_GOTO(out_iovs, rc);
		}
		iovs[j].iov_buf_len = sizes[i];
		sizes[i] = iovs[j].iov_len;

		/* If buffer length is zero, send only size */
		if (iovs[j].iov_buf_len > 0)
			++j;
	}

	sgl.sg_nr = j;
	sgl.sg_nr_out = sgl.sg_nr;
	sgl.sg_iovs = iovs;
	rc = crt_bulk_create(rpc->cr_ctx, daos2crt_sg(&sgl),
			     CRT_BULK_RO, &local_bulk);
	if (rc != 0)
		D_GOTO(out_iovs, rc);

	rc = attr_bulk_transfer(rpc, CRT_BULK_PUT, local_bulk,
				in->pagi_bulk, 0, in->pagi_key_length,
				bulk_size - in->pagi_key_length);
	crt_bulk_free(local_bulk);
	if (rc != 0)
		D_GOTO(out_iovs, rc);

out_iovs:
	D_FREE(iovs);
out_data:
	D_FREE(data);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->po_hint);
	pool_svc_put_leader(svc);
out:
	out->po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pagi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);

}

struct attr_list_iter_args {
	size_t		 alia_available; /* Remaining client buffer space */
	size_t		 alia_length; /* Aggregate length of attribute names */
	size_t		 alia_iov_index;
	size_t		 alia_iov_count;
	daos_iov_t	*alia_iovs;
};

static int
attr_list_iter_cb(daos_handle_t ih,
		  daos_iov_t *key, daos_iov_t *val, void *arg)
{
	struct attr_list_iter_args *i_args = arg;

	i_args->alia_length += key->iov_len;

	if (i_args->alia_available > key->iov_len && key->iov_len > 0) {
		/*
		 * Exponentially grow the array of IOVs if insufficient.
		 * Considering the pathological case where each key is just
		 * a single character, with one additional trailing '\0',
		 * if the client buffer is 'N' bytes, it can hold at the most
		 * N/2 keys, which requires that many IOVs to be allocated.
		 * Thus, the upper limit on the space required for IOVs is:
		 * sizeof(daos_iov_t) * N/2 = 24 * N/2 = 12*N bytes.
		 */
		if (i_args->alia_iov_index == i_args->alia_iov_count) {
			void *ptr;

			D_REALLOC(ptr, i_args->alia_iovs,
				  i_args->alia_iov_count *
				  2 * sizeof(daos_iov_t));
			/*
			 * TODO: Fail or continue transferring
			 *	 iteratively using available memory?
			 */
			if (ptr == NULL)
				return -DER_NOMEM;
			i_args->alia_iovs = ptr;
			i_args->alia_iov_count *= 2;
		}

		memcpy(&i_args->alia_iovs[i_args->alia_iov_index],
		       key, sizeof(daos_iov_t));
		i_args->alia_iovs[i_args->alia_iov_index]
			.iov_buf_len = key->iov_len;
		i_args->alia_available -= key->iov_len;
		++i_args->alia_iov_index;
	}
	return 0;
}

void
ds_pool_attr_list_handler(crt_rpc_t *rpc)
{
	struct pool_attr_list_in	*in	    = crt_req_get(rpc);
	struct pool_attr_list_out	*out	    = crt_reply_get(rpc);
	struct pool_svc			*svc;
	struct rdb_tx			 tx;
	crt_bulk_t			 local_bulk;
	daos_size_t			 bulk_size;
	int				 rc;
	struct attr_list_iter_args	 iter_args;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pali_op.pi_uuid), rpc, DP_UUID(in->pali_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pali_op.pi_uuid, &svc,
				    &out->palo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_db, svc->ps_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);

	/*
	 * If remote bulk handle does not exist, only aggregate size is sent.
	 */
	if (in->pali_bulk) {
		rc = crt_bulk_get_len(in->pali_bulk, &bulk_size);
		if (rc != 0)
			D_GOTO(out_lock, rc);
		D_DEBUG(DF_DSMS, DF_UUID": bulk_size=%lu\n",
			DP_UUID(in->pali_op.pi_uuid), bulk_size);

		/* Start with 1 and grow as needed */
		D_ALLOC_PTR(iter_args.alia_iovs);
		if (iter_args.alia_iovs == NULL)
			D_GOTO(out_lock, rc = -DER_NOMEM);
		iter_args.alia_iov_count = 1;
	} else {
		bulk_size = 0;
		iter_args.alia_iovs = NULL;
		iter_args.alia_iov_count = 0;
	}
	iter_args.alia_iov_index = 0;
	iter_args.alia_length	 = 0;
	iter_args.alia_available = bulk_size;
	rc = rdb_tx_iterate(&tx, &svc->ps_user, false /* !backward */,
			    attr_list_iter_cb, &iter_args);
	out->palo_size = iter_args.alia_length;
	if (rc != 0)
		D_GOTO(out_mem, rc);

	if (iter_args.alia_iov_index > 0) {
		daos_sg_list_t	 sgl = {
			.sg_nr_out = iter_args.alia_iov_index,
			.sg_nr	   = iter_args.alia_iov_index,
			.sg_iovs   = iter_args.alia_iovs
		};
		rc = crt_bulk_create(rpc->cr_ctx, daos2crt_sg(&sgl),
				     CRT_BULK_RW, &local_bulk);
		if (rc != 0)
			D_GOTO(out_mem, rc);

		rc = attr_bulk_transfer(rpc, CRT_BULK_PUT, local_bulk,
					in->pali_bulk, 0, 0,
					bulk_size - iter_args.alia_available);
		crt_bulk_free(local_bulk);
	}

out_mem:
	D_FREE(iter_args.alia_iovs);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_pool_set_hint(svc->ps_db, &out->palo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->palo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pali_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_replicas_update_handler(crt_rpc_t *rpc)
{
	struct pool_membership_in	*in = crt_req_get(rpc);
	struct pool_membership_out	*out = crt_reply_get(rpc);
	crt_opcode_t			 opc = opc_get(rpc->cr_opc);
	struct pool_svc			*svc;
	struct rdb			*db;
	d_rank_list_t			*ranks;
	uuid_t				 dbid;
	uuid_t				 psid;
	int				 rc;

	D_DEBUG(DB_MD, DF_UUID": Replica Rank: %u\n", DP_UUID(in->pmi_uuid),
				 in->pmi_targets->rl_ranks[0]);

	rc = daos_rank_list_dup(&ranks, in->pmi_targets);
	if (rc != 0)
		D_GOTO(out, rc);

	/*
	 * Do this locally and release immediately; otherwise if we try to
	 * remove the leader replica, the call never returns since the service
	 * won't stop until all references have been released
	 */
	rc = pool_svc_lookup_leader(in->pmi_uuid, &svc, &out->pmo_hint);
	if (rc != 0)
		D_GOTO(out, rc);
	/* TODO: Use rdb_get() to track references? */
	db = svc->ps_db;
	rdb_get_uuid(db, dbid);
	uuid_copy(psid, svc->ps_uuid);
	pool_svc_put_leader(svc);

	switch (opc) {
	case POOL_REPLICAS_ADD:
		rc = ds_pool_rdb_dist_start(dbid, psid, in->pmi_targets,
					    true /* create */,
					    false /* bootstrap */,
					    get_md_cap());
		if (rc != 0)
			break;
		rc = rdb_add_replicas(db, ranks);
		break;

	case POOL_REPLICAS_REMOVE:
		rc = rdb_remove_replicas(db, ranks);
		if (rc != 0)
			break;
		/* ignore return code */
		ds_pool_rdb_dist_stop(psid, in->pmi_targets, true /*destroy*/);
		break;

	default:
		D_ASSERT(0);
	}

	ds_pool_set_hint(db, &out->pmo_hint);
out:
	out->pmo_failed = ranks;
	out->pmo_rc = rc;
	crt_reply_send(rpc);
}
