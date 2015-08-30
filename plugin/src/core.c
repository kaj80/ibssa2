/*
 * Copyright (c) 2012-2015 Mellanox Technologies LTD. All rights reserved.
 * Copyright (c) 2012-2013 Intel Corporation. All rights reserved.
 * Copyright (c) 2012 Lawrence Livermore National Securities.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <limits.h>
#include <syslog.h>
#include <sys/timerfd.h>
#include <infiniband/osm_headers.h>
#include <search.h>
#include <common.h>
#include <infiniband/ssa_mad.h>
#include <infiniband/ssa_extract.h>
#include <infiniband/ssa_comparison.h>
#include <ssa_ctrl.h>
#include <ssa_log.h>
#include <infiniband/ssa_db_helper.h>
#include <ssa_admin.h>

#define SSA_CORE_OPTS_FILE SSA_FILE_PREFIX "_core" SSA_OPTS_FILE_SUFFIX
#define EXTRACT_TIMER_FD_SLOT		2
#define TREE_BALANCE_TIMER_FD_SLOT	3
#define FIRST_DOWNSTREAM_FD_SLOT	4

#ifndef CORE_BALANCE_TIMEOUT
#define CORE_BALANCE_TIMEOUT 300	/* 5 minutes in seconds */
#endif

/*
 * Service options - may be set through ibssa_opts.cfg file.
 */
static char *opts_file = RDMA_CONF_DIR "/" SSA_CORE_OPTS_FILE;
static int node_type = SSA_NODE_CORE;
int smdb_deltas = 0;
static char log_file[128] = "/var/log/ibssa.log";
static char lock_file[128] = "/var/run/ibssa.pid";
char addr_data_file[128] = RDMA_CONF_DIR "/" SSA_HOSTS_FILE;
int addr_preload = 0;
#if defined(SIM_SUPPORT) || defined(SIM_SUPPORT_SMDB)
static char *smdb_lock_file = "ibssa_smdb.lock";
static int smdb_lock_fd = -1;
#endif
static int first_extraction = 1;

enum {
	SSA_DTREE_DEFAULT	= 0,
	SSA_DTREE_CORE		= 1 << 0,
	SSA_DTREE_DISTRIB	= 1 << 1,
	SSA_DTREE_ACCESS	= 1 << 2,
	SSA_DTREE_CONSUMER	= 1 << 3
};

#ifndef SIM_SUPPORT
static int distrib_tree_level = SSA_DTREE_DEFAULT;
static uint64_t dtree_epoch_cur = 0;
static uint64_t dtree_epoch_prev = 0;
static time_t join_timeout = 30; /* timeout for joining to original parent node in seconds */
#endif

extern int log_flush;
extern int accum_log_file;
extern int smdb_dump;
extern int err_smdb_dump;
extern int prdb_dump;
extern char smdb_dump_dir[128];
extern char prdb_dump_dir[128];
extern short smdb_port;
extern short prdb_port;
extern short admin_port;
extern int keepalive;
extern int sock_accessextract[2];
#ifdef SIM_SUPPORT_FAKE_ACM
extern int fake_acm_num;
#endif

/* Used for primary/secondary state to properly maintain number of children */
enum {
	SSA_CHILD_IDLE		= 0,
	SSA_CHILD_PARENTED	= (1 << 0)
};

struct ssa_member {
	struct ssa_member_record	rec;
	struct ssa_member		*primary;	/* parent */
	struct ssa_member		*secondary;	/* parent */
	int				primary_state;
	int				secondary_state;
	time_t				join_start_time;
	uint16_t			lid;
	uint8_t				sl;
	atomic_t			child_num;
	atomic_t			access_child_num; /* used when combined or access node type */
	DLIST_ENTRY			child_list;
	DLIST_ENTRY			access_child_list; /* used when combined or access node type */
	DLIST_ENTRY			entry;
	DLIST_ENTRY			access_entry;
};

struct ssa_core {
	struct ssa_svc			svc;
	void				*member_map;
	pthread_mutex_t			list_lock; /* should be taken when accessing one of the member lists */
	DLIST_ENTRY			orphan_list;
	DLIST_ENTRY			core_list;
	DLIST_ENTRY			distrib_list;
	DLIST_ENTRY			access_list;
};

struct ssa_extract_data {
	void				*opensm;
	int				num_svcs;
	struct ssa_svc			**svcs;
};

enum core_tree_action {
	CORE_TREE_NODE_TYPE_COUNT,
	CORE_TREE_NODE_PARENT_TEST
};

struct core_tree_context {
	struct ssa_core		*core;
	enum core_tree_action	action;
	int			node_type;
	void			*priv;
};

static struct ssa_class ssa;
struct ssa_database *ssa_db;
static struct ssa_db_diff *ssa_db_diff = NULL;
struct ssa_db *ipdb = NULL;
pthread_mutex_t ssa_db_diff_lock;
pthread_t ctrl_thread, extract_thread;
static osm_opensm_t *osm;
static struct ssa_extract_data extract_data;
#ifdef SIM_SUPPORT_SMDB
static struct ssa_db *p_ref_smdb = NULL;
#endif

static int sock_coreextract[2];
static const union ibv_gid zero_gid = { {0} };

/* Forward declarations */
#ifdef SIM_SUPPORT_SMDB
static int ssa_extract_process(osm_opensm_t *p_osm, struct ssa_db *p_ref_smdb,
			       int *outstanding_count);
#endif

#ifndef SIM_SUPPORT
static void core_free_member(void *gid);

static int is_gid_not_zero(union ibv_gid *gid)
{
	int ret = 0;

	if (ssa_compare_gid(&zero_gid, gid))
		ret = 1;

	return ret;
}

/* Should the following two DList routines go into a new dlist.c in shared ? */
static DLIST_ENTRY *DListFind(DLIST_ENTRY *entry, DLIST_ENTRY *list)
{
	DLIST_ENTRY *cur_entry;

	for (cur_entry = list->Next; cur_entry != list;
	     cur_entry = cur_entry->Next) {
		if (cur_entry == entry)
			return entry;
	}
	return NULL;
}

static int DListCount(DLIST_ENTRY *list)
{
	DLIST_ENTRY *entry;
	int count = 0;

	for (entry = list->Next; entry != list; entry = entry->Next)
		count++;
	return count;
}

/*
 * Current algorithm for find_best_parent is to merely balance
 * the number of children when a new join arrives at the
 * core.
 *
 * There is join order dependency in the current algorithm.
 * The current assumption is that distribution tree (core,
 * distribution, and access nodes) come up prior to compute
 * nodes. This is so-call "structured" bringup. In the
 * future, as part of resiliency work, unstructured bringup
 * will be supported.
 *
 * Note also that there is currently no rebalancing. Balancing
 * only occurs on join to subnet and not on leaves from subnet.
 * This will be further investigated when fault/error handling
 * is added. Also, there is no way currently for the core
 * to request that a downstream node switchover to new parent.
 * Also, a way for downstream node to request a reparent may
 * also be needed.
 *
 * For now, if the child is an access node and there is no
 * distribution node, the parent will be the core node. This
 * may change depending on how reconnection ends up working.
 *
 * Also, for now, if child is consumer node and there is no
 * access node, this is an error and the child needs to
 * rety the join.
 *
 * Subsequent version may be based on some maximum number of hops
 * allowed between child and parent but this requires similar
 * expensive calculations like routing.
 *
 * A simpler approach would be to support a configuration file for this.
 *
 * Another mechanism to influence the algorithm is weighting for
 * combined nodes so these handler fewer access nodes than a "pure"
 * access node when subnet has a mix of such nodes.
 */
static union ibv_gid *find_best_parent(struct ssa_core *core,
				       struct ssa_member *child,
				       time_t join_time_passed)
{
	struct ssa_svc *svc;
	DLIST_ENTRY *list = NULL, *entry;
	struct ssa_member *member;
	union ibv_gid *parentgid = NULL;
	int least_child_num;
	uint8_t node_type;

	if (child->primary && !child->rec.bad_parent)
		return (union ibv_gid *) child->primary->rec.port_gid;

	svc = &core->svc;
	node_type = child->rec.node_type;

	switch (node_type) {
	case SSA_NODE_CORE:
	case SSA_NODE_DISTRIBUTION:
	case (SSA_NODE_CORE | SSA_NODE_ACCESS):
	case (SSA_NODE_DISTRIBUTION | SSA_NODE_ACCESS):
		list = NULL;
		parentgid = &svc->port->gid;
		break;
	case SSA_NODE_ACCESS:
		if (is_gid_not_zero((union ibv_gid *) child->rec.parent_gid) &&
		    !child->rec.bad_parent) {
			if (join_time_passed < join_timeout) {
				/* Try to preserve previous tree formation */
				list = NULL;
				if (tfind(child->rec.parent_gid, &core->member_map, ssa_compare_gid))
					parentgid = (union ibv_gid *) child->rec.parent_gid;
				break;
			}
		}

		/* If no distribution nodes yet, parent is core */
		if (DListCount(&core->distrib_list))
			list = &core->distrib_list;
		else {
			list = NULL;
			parentgid = &svc->port->gid;
		}
		break;
	case SSA_NODE_CONSUMER:
		/* If child is consumer, parent is access */
		list = &core->access_list;
		break;
	}

	if (list) {
		least_child_num = INT_MAX;
		parentgid = NULL;
		for (entry = list->Next; entry != list; entry = entry->Next) {
			if (node_type == SSA_NODE_CONSUMER) {
				member = container_of(entry, struct ssa_member,
						      access_entry);
				if (child->rec.bad_parent &&
				    !memcmp(child->rec.parent_gid, member->rec.port_gid, 16))
						continue;
				if (atomic_get(&member->access_child_num) < least_child_num) {
					parentgid = (union ibv_gid *) member->rec.port_gid;
					least_child_num = atomic_get(&member->access_child_num);
					if (!least_child_num)
						break;
				}
			} else {
				member = container_of(entry, struct ssa_member,
						      entry);
				if (child->rec.bad_parent &&
				    !memcmp(child->rec.parent_gid, member->rec.port_gid, 16))
						continue;
				if (atomic_get(&member->child_num) < least_child_num) {
					parentgid = (union ibv_gid *) member->rec.port_gid;
					least_child_num = atomic_get(&member->child_num);
					if (!least_child_num)
						break;
				}
			}
		}
	}
	return parentgid;
}

static void core_clean_tree(struct ssa_svc *svc)
{
	struct ssa_core *core = container_of(svc, struct ssa_core, svc);

	ssa_log_func(SSA_LOG_CTRL);

	if (core->member_map)
		tdestroy(core->member_map, core_free_member);
	core->member_map = NULL;

	DListInit(&core->orphan_list);
	DListInit(&core->core_list);
	DListInit(&core->distrib_list);
	DListInit(&core->access_list);
}

static void core_update_children_counter(struct ssa_core *core, union ibv_gid *parentgid,
					 union ibv_gid *childgid, int increment)
{
	struct ssa_member_record *rec;
	struct ssa_member *parent, *child;
	uint8_t **member;
	atomic_t *children_num = NULL;

	member = tfind(parentgid, &core->member_map, ssa_compare_gid);
	if (!member) {
		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
				sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) parentgid, sizeof(*parentgid));
		ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			    "couldn't find parent with GID %s\n", log_data);
		return;
	} else {
		rec = container_of(*member, struct ssa_member_record, port_gid);
		parent = container_of(rec, struct ssa_member, rec);

		member = tfind(childgid, &core->member_map, ssa_compare_gid);
		if (!member) {
			ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
					sizeof log_data, SSA_ADDR_GID,
					(uint8_t *) childgid, sizeof(*childgid));
			ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				    "couldn't find child with GID %s\n", log_data);
			return;
		 } else {
			rec = container_of(*member, struct ssa_member_record, port_gid);
			child = container_of(rec, struct ssa_member, rec);

			if (child->rec.node_type == SSA_NODE_CONSUMER)
				children_num = &parent->access_child_num;
			else if ((child->rec.node_type & SSA_NODE_CORE) != SSA_NODE_CORE)
				children_num = &parent->child_num;

			if (children_num) {
				if (increment)
					atomic_inc(children_num);
				else
					atomic_dec(children_num);
			}
		}
	}
}

static int core_build_tree(struct ssa_core *core, struct ssa_member *child,
			   union ibv_gid *parentgid)
{
	struct ssa_svc *svc = &core->svc;
	union ibv_gid *gid = (union ibv_gid *) child->rec.port_gid;
	int ret = -1;
	uint8_t node_type = child->rec.node_type;

	switch (node_type) {
	case SSA_NODE_DISTRIBUTION:
	case (SSA_NODE_DISTRIBUTION | SSA_NODE_ACCESS):
		if (parentgid)
			ret = ssa_svc_query_path(svc, parentgid, gid);
		if (parentgid && !ret) {
			if (!DListFind(&child->entry, &core->distrib_list))
				DListInsertBefore(&child->entry,
						  &core->distrib_list);
			if (node_type & SSA_NODE_ACCESS) {
				if (!DListFind(&child->access_entry,
					       &core->access_list))
					DListInsertBefore(&child->access_entry,
							  &core->access_list);
			}
		}
		break;
	case SSA_NODE_ACCESS:
		if (parentgid)
			ret = ssa_svc_query_path(svc, parentgid, gid);
		if (parentgid && !ret &&
		    !DListFind(&child->access_entry, &core->access_list))
			DListInsertBefore(&child->access_entry,
					  &core->access_list);
		break;
	case (SSA_NODE_CORE | SSA_NODE_ACCESS):
	case SSA_NODE_CORE:
		/* TODO: Handle standby SM nodes */
		if (parentgid)
			ret = ssa_svc_query_path(svc, parentgid, gid);
		if (parentgid && !ret) {
			if (!DListFind(&child->entry, &core->core_list))
				DListInsertBefore(&child->entry, &core->core_list);
			if ((node_type & SSA_NODE_ACCESS) &&
			    (!DListFind(&child->access_entry,
					&core->access_list))) {
				DListInsertBefore(&child->access_entry,
						  &core->access_list);
			}
		}
		break;
	case SSA_NODE_CONSUMER:
		if (parentgid)
			ret = ssa_svc_query_path(svc, parentgid, gid);
		else
			ssa_log_err(SSA_LOG_CTRL,
				    "no access node joined as yet\n");
		break;
	}

	if (parentgid && !ret)
		core_update_children_counter(core, parentgid, gid, 1);

	return ret;
}

static void core_update_tree(struct ssa_core *core, struct ssa_member *child,
			     union ibv_gid *gid)
{
	struct ssa_member_record *rec;
	struct ssa_member *parent;
	uint8_t **tgid;
	uint8_t node_type;

	if (!child)
		return;

	/*
	 * Find parent of child being removed from tree and
	 * update the number of children.
	 *
	 * No way to force children to reconnect currently.
	 */
	node_type = child->rec.node_type;
	if (node_type & SSA_NODE_CORE)
		return;
	if (!child->primary) {
		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
				sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) &child->rec.port_gid,
				sizeof child->rec.port_gid);
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"ERROR - no parent for GID %s\n", log_data);
		return;
	}

	/* Should something else be done with children whose parent goes away ? */
	tgid = tfind(&child->primary->rec.port_gid, &core->member_map, ssa_compare_gid);
	if (!tgid) {
		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
				sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) &child->primary->rec.port_gid,
				sizeof child->primary->rec.port_gid);
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"ERROR - couldn't find parent GID %s\n", log_data);
		return;
	}
	rec = container_of(*tgid, struct ssa_member_record, port_gid);
	parent = container_of(rec, struct ssa_member, rec);
	if (node_type & SSA_NODE_CONSUMER)
		atomic_dec(&parent->access_child_num);
	else if ((node_type & SSA_NODE_CORE) != SSA_NODE_CORE)
		atomic_dec(&parent->child_num);
	child->primary = NULL;
	child->primary_state = SSA_CHILD_IDLE;
}

static int
ssa_sprint_member(char *buf, size_t buf_size, struct ssa_member *member, int level)
{
	struct ssa_member_record *member_rec = &member->rec;
	char addr[INET6_ADDRSTRLEN];
	char parent[64] = { 0 }, children[64] = { 0 };
	int ret = 0;
	uint16_t parent_lid = 0;

	/*
	 * Currently only primary parent is supported in
	 * distribution tree formation algorithm.
	 */
	if (member->primary_state & SSA_CHILD_PARENTED)
		parent_lid = member->primary->lid;
	else if (member->secondary_state & SSA_CHILD_PARENTED)
		parent_lid = member->secondary->lid;

	if (parent_lid)
		snprintf(parent, sizeof parent, "parent LID %u", parent_lid);
	else /* member is orphan */
		snprintf(parent, sizeof parent, "no parent");

	if (((level & SSA_DTREE_CORE) &&
	     (member_rec->node_type & SSA_NODE_CORE)) ||
	    ((level & SSA_DTREE_DISTRIB) &&
	     (member_rec->node_type & SSA_NODE_DISTRIBUTION)))
		snprintf(children, sizeof children,
			 " [ children %ld ]", atomic_get(&member->child_num));
	else if ((level & SSA_DTREE_ACCESS) &&
		 (member_rec->node_type & SSA_NODE_ACCESS))
		snprintf(children, sizeof children,
			 " [ children %ld ]", atomic_get(&member->access_child_num));
	else if ((level & SSA_DTREE_CONSUMER) &&
		 (member_rec->node_type & SSA_NODE_CONSUMER))
		snprintf(children, sizeof children, "[ no children ]");

	ssa_sprint_addr(SSA_LOG_DEFAULT, addr, sizeof addr, SSA_ADDR_GID,
			member_rec->port_gid, sizeof member_rec->port_gid);
	ret = snprintf(buf, buf_size, "[ (%s) GID %s LID %u SL %u DB 0x%"
		       PRIx64 " ] [ %s ]%s\n",
		       ssa_node_type_str(member_rec->node_type),
		       addr, member->lid, member->sl,
		       ntohll(member_rec->database_id), parent, children);
	if (ret >= buf_size)
		ssa_log_warn(SSA_LOG_DEFAULT,
			     "output buffer size is not sufficient\n");

	return ret;
}

static void core_dump_tree(struct ssa_core *core, char *svc_name)
{
	DLIST_ENTRY *child_entry, *child_list = NULL;
	DLIST_ENTRY *entry, *list = NULL;
	int core_cnt = 0, access_cnt = 0;
	int distrib_cnt = 0, consumer_cnt = 0;
	struct ssa_member *member, *child;
	size_t buf_size;
	int ssa_nodes = 0;
	unsigned n = 0;
	char *buf;

	if (distrib_tree_level == SSA_DTREE_DEFAULT)
		return;

	list = &core->core_list;
	for (entry = list->Next; entry != list; entry = entry->Next)
		core_cnt++;

	list = &core->distrib_list;
	for (entry = list->Next; entry != list; entry = entry->Next)
		distrib_cnt++;

	list = &core->access_list;
	for (entry = list->Next; entry != list; entry = entry->Next) {
		member = container_of(entry, struct ssa_member, access_entry);
		access_cnt++;
		child_list = &member->access_child_list;
		for (child_entry = child_list->Next; child_entry != child_list;
		     child_entry = child_entry->Next)
			consumer_cnt++;
	}

	if (distrib_tree_level & SSA_DTREE_CORE)
		ssa_nodes += core_cnt;
	if (distrib_tree_level & SSA_DTREE_DISTRIB)
		ssa_nodes += distrib_cnt;
	if ((distrib_tree_level & SSA_DTREE_ACCESS)||
	    (distrib_tree_level & SSA_DTREE_CONSUMER))
		ssa_nodes += access_cnt;
	if (distrib_tree_level & SSA_DTREE_CONSUMER)
		ssa_nodes += consumer_cnt;

	/* 13 is the number of distribution tree meta data log lines */
	buf_size = (ssa_nodes + 13) * 256 * sizeof(*buf);
	buf = calloc(1, buf_size);
	if (!buf) {
		ssa_log_err(SSA_LOG_DEFAULT, "unable to allocate buffer\n");
		return;
	}

	n += snprintf(buf + n, buf_size - n, "General SSA distribution tree info\n");
	n += snprintf(buf + n, buf_size - n, "------------------------------------\n");
	n += snprintf(buf + n, buf_size - n, "| Core nodes:           %10d |\n", core_cnt);
	n += snprintf(buf + n, buf_size - n, "| Distribution nodes:   %10d |\n", distrib_cnt);
	n += snprintf(buf + n, buf_size - n, "| Access nodes:         %10d |\n", access_cnt);
	n += snprintf(buf + n, buf_size - n, "| Consumer (ACM) nodes: %10d |\n", consumer_cnt);
	n += snprintf(buf + n, buf_size - n, "------------------------------------\n\n");

	if (distrib_tree_level & SSA_DTREE_CORE) {
		n += snprintf(buf + n, buf_size - n, "[ Core nodes ]\n");
		list = &core->core_list;
		for (entry = list->Next; entry != list; entry = entry->Next) {
			member = container_of(entry, struct ssa_member, entry);
			n += ssa_sprint_member(buf + n, buf_size - n, member,
					       SSA_DTREE_CORE);
		}
	}

	if (distrib_tree_level & SSA_DTREE_DISTRIB) {
		n += snprintf(buf + n, buf_size - n, "------------------------------------\n\n");
		n += snprintf(buf + n, buf_size - n, "[ Distribution nodes ]\n");
		list = &core->distrib_list;
		for (entry = list->Next; entry != list; entry = entry->Next) {
			member = container_of(entry, struct ssa_member, entry);
			n += ssa_sprint_member(buf + n, buf_size - n, member,
					       SSA_DTREE_DISTRIB);
		}
	}

	if ((distrib_tree_level & SSA_DTREE_ACCESS) ||
	    (distrib_tree_level & SSA_DTREE_CONSUMER)) {
		n += snprintf(buf + n, buf_size - n, "------------------------------------\n\n");
		n += snprintf(buf + n, buf_size - n, "[ Access nodes ]\n");
		list = &core->access_list;
		for (entry = list->Next; entry != list; entry = entry->Next) {
			member = container_of(entry, struct ssa_member, access_entry);
			n += ssa_sprint_member(buf + n, buf_size - n, member,
					       SSA_DTREE_ACCESS);

			if (distrib_tree_level & SSA_DTREE_CONSUMER) {
				child_list = &member->access_child_list;
				for (child_entry = child_list->Next; child_entry != child_list;
				     child_entry = child_entry->Next) {
					child = container_of(child_entry, struct ssa_member, entry);
					n += ssa_sprint_member(buf + n, buf_size - n, child, SSA_DTREE_CONSUMER);
				}
			}
		}
	}
	n += snprintf(buf + n, buf_size - n, "------------------------------------\n\n");

	ssa_log(SSA_LOG_DEFAULT, "%s\n\n%s", svc_name, buf);
	free(buf);
}

/* Caller must hold list lock. */
static void core_adopt_orphans(DLIST_ENTRY *orphan_list, int node_type)
{
	DLIST_ENTRY *entry, tmp;
	struct ssa_core *core =
		container_of(orphan_list, struct ssa_core, orphan_list);
	struct ssa_member *member;
	union ibv_gid *parentgid = NULL;
	time_t join_time_passed = 0;
	int ret, changed = 0;

	if (!DListEmpty(orphan_list)) {
		entry = orphan_list->Next;
		while (entry != orphan_list) {
			member = container_of(entry, struct ssa_member, entry);

			tmp = *entry;
			entry = entry->Next;

			if (!(member->rec.node_type & node_type))
				continue;

			if (member->rec.node_type == SSA_NODE_ACCESS)
				join_time_passed = time(NULL) - member->join_start_time;

			parentgid = find_best_parent(core, member, join_time_passed);
			ret = core_build_tree(core, member, parentgid);
			if (!ret) {
				DListRemove(&tmp);
				changed = 1;
			}
		}
		if (changed)
			dtree_epoch_cur++;
	}
}

static void core_process_orphans(struct ssa_core *core)
{
	pthread_mutex_lock(&core->list_lock);
	core_adopt_orphans(&core->orphan_list, SSA_NODE_CORE);
	core_adopt_orphans(&core->orphan_list, SSA_NODE_DISTRIBUTION);
	core_adopt_orphans(&core->orphan_list, SSA_NODE_ACCESS);
	core_adopt_orphans(&core->orphan_list, SSA_NODE_CONSUMER);
	pthread_mutex_unlock(&core->list_lock);
}

static void core_handle_node(struct ssa_member_record *rec,
			     struct core_tree_context *context)
{
	struct ssa_member *parent = NULL, *child = NULL;
	int *context_num = NULL;

	if (!(context->node_type & rec->node_type))
		return;

	switch (context->action) {
	case CORE_TREE_NODE_TYPE_COUNT:
		context_num = (int *) context->priv;
		*context_num = *context_num + 1;;
		break;
	case CORE_TREE_NODE_PARENT_TEST:
		child = container_of(rec, struct ssa_member, rec);
		if (child->primary_state & SSA_CHILD_PARENTED)
			parent = child->primary;
		else if (child->secondary_state & SSA_CHILD_PARENTED)
			parent = child->secondary;

		if (parent) {
			int parent_children_num = 0;

			context_num = (int *) context->priv;
			if (child->rec.node_type & SSA_NODE_ACCESS) {
				parent_children_num = atomic_get(&parent->child_num);
				if (*context_num < parent_children_num)
					atomic_dec(&parent->child_num);
			} else if (child->rec.node_type & SSA_NODE_CONSUMER) {
				parent_children_num = atomic_get(&parent->access_child_num);
				if (*context_num < parent_children_num)
					atomic_dec(&parent->access_child_num);
			}

			if (*context_num < parent_children_num) {
				child->primary		= NULL;
				child->secondary	= NULL;
				child->primary_state	= SSA_CHILD_IDLE;
				child->secondary_state	= SSA_CHILD_IDLE;
				memset(child->rec.parent_gid, 0, 16);

				DListInsertBefore(&child->entry,
						  &context->core->orphan_list);

				if (child->rec.node_type & SSA_NODE_ACCESS) {
					if (DListFind(&child->access_entry,
						      &context->core->access_list))
						DListRemove(&child->access_entry);
				}
			}
		}
		break;
	default:
		ssa_log_warn(SSA_LOG_DEFAULT,
			     "unknown action %d for node type %d (%s)\n",
			     context->action, context->node_type,
			     ssa_node_type_str(context->node_type));
		break;
	}
}

static void core_tree_callback(const void *nodep, const VISIT which,
			       const void *priv)
{
	struct core_tree_context *context = (struct core_tree_context *) priv;
	struct ssa_member_record *rec = NULL;
	int handle_node = 0;

	switch (which) {
	case preorder:
		break;
	case postorder:
		handle_node = 1;
		break;
	case endorder:
		break;
	case leaf:
		handle_node = 1;
		break;
	}

	if (handle_node) {
		rec = container_of(* (struct ssa_member **) nodep,
				   struct ssa_member_record, port_gid);
		core_handle_node(rec, context);
	}
}

static void core_rebalance_tree_layer(struct ssa_core *core, int max_children,
				      int child_type)
{
	struct core_tree_context context;

	context.core		= core;
	context.node_type	= child_type;
	context.action		= CORE_TREE_NODE_PARENT_TEST;
	context.priv		= &max_children;

	ssa_twalk(core->member_map, core_tree_callback, &context);

	ssa_log(SSA_LOG_DEFAULT,
		"child type %d (%s) max children allowed per parent %d\n",
		child_type, ssa_node_type_str(child_type), max_children);
}

/*
 * Current algorithm for distribution tree rebalancing
 * is to balance the number of children for distribution
 * and access layers.
 *
 * Balanced tree assures that maximum difference in
 * fanout (number of children) between nodes, that belong
 * to the same layer, is not more than 1.
 *
 * The algorithm works as follows:
 *
 * - Maximum fanout is being calculated per layer
 *
 * - For each node whose actual fanout is larger
 *   than the maximum allowed, we take the proper number
 *   of children, reset their member_record parent_gid
 *   field and add them to orphan list.
 *
 * - Orphan list members are being adopted (assigned with
 *   new parents)
 *
 * Algorithm purpose is to keep ssa distribution tree
 * balanced in case of unordered ssa fabric bringup.
 *
 * IMPORTANT NOTE:
 * Current rebalancing mechanism is currently only accurate at
 * initial ssa bring-up phase, because it relies
 * on children counters that are not currently updated when
 * some node leaves the distribution tree.
 *
 */
static void core_rebalance_tree(struct ssa_core *core)
{
	struct core_tree_context context;
	int distrib_num, access_num, consumer_num = 0;
	int distrib_child_max, access_child_max;

	ssa_log_func(SSA_LOG_DEFAULT);

	pthread_mutex_lock(&core->list_lock);

	distrib_num	= DListCount(&core->distrib_list);
	access_num	= DListCount(&core->access_list);

	context.core		= core;
	context.node_type	= SSA_NODE_CONSUMER;
	context.action		= CORE_TREE_NODE_TYPE_COUNT;
	context.priv		= &consumer_num;

	ssa_twalk(core->member_map, core_tree_callback, &context);

	if (distrib_num > 0) {
		distrib_child_max = access_num / distrib_num;
		if (access_num % distrib_num)
			distrib_child_max++;

		if (distrib_child_max)
			core_rebalance_tree_layer(core, distrib_child_max,
						  SSA_NODE_ACCESS);
	}

	if (access_num > 0) {
		access_child_max = consumer_num / access_num;
		if (consumer_num % access_num)
			access_child_max++;

		if (access_child_max)
			core_rebalance_tree_layer(core, access_child_max,
						  SSA_NODE_CONSUMER);
	}

	core_adopt_orphans(&core->orphan_list, SSA_NODE_ACCESS);
	core_adopt_orphans(&core->orphan_list, SSA_NODE_CONSUMER);

	pthread_mutex_unlock(&core->list_lock);
}

/*
 * Process received SSA membership requests.  On errors, we simply drop
 * the request and let the remote node retry.
 */
static void core_process_join(struct ssa_core *core, struct ssa_umad *umad)
{
	struct ssa_member_record *rec, *umad_rec;
	struct ssa_member *member;
	union ibv_gid *parentgid = NULL;
	DLIST_ENTRY *entry;
	uint8_t **tgid, node_type;
	time_t join_time_passed = 0;
	int ret;

	/* TODO: verify ssa_key with core nodes */
	umad_rec = rec = &umad->packet.ssa_mad.member;
	node_type = rec->node_type;
	ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, rec->port_gid, sizeof rec->port_gid);
	if (is_gid_not_zero((union ibv_gid *) rec->parent_gid)) {
		ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				log_data1, sizeof log_data1, SSA_ADDR_GID,
				rec->parent_gid, sizeof rec->parent_gid);
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
			"%s %s node type %d old parent %s %s\n",
			core->svc.name, log_data, node_type, log_data1,
			rec->bad_parent ? "BAD": "OK");
	} else {
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s node type %d\n",
			core->svc.name, log_data, node_type);
	}

	tgid = tfind(rec->port_gid, &core->member_map, ssa_compare_gid);
	if (!tgid) {
		ssa_log(SSA_LOG_CTRL, "adding new member\n");
		member = calloc(1, sizeof *member);
		if (!member)
			return;

		member->rec = *rec;
		member->lid = ntohs(umad->umad.addr.lid);
		member->join_start_time = time(NULL);
		atomic_init(&member->child_num);
		atomic_init(&member->access_child_num);
		DListInit(&member->child_list);
		DListInit(&member->access_child_list);
		if (!tsearch(&member->rec.port_gid, &core->member_map, ssa_compare_gid)) {
			free(member);
			return;
		}
	} else {
		rec = container_of(*tgid, struct ssa_member_record, port_gid);
		member = container_of(rec, struct ssa_member, rec);
		entry = DListFind(&member->entry, &core->orphan_list);
		if (entry) {
			ssa_log(SSA_LOG_CTRL, "removing member in orphan list\n");
			DListRemove(&member->entry);
		}
		member->rec = *umad_rec;
		/* Need to handle child_list/access_child_list */
		/* and other fields in member struct */
	}

	umad->packet.mad_hdr.status = 0;
	if (!first_extraction) {
		if (node_type == SSA_NODE_ACCESS)
			join_time_passed = time(NULL) - member->join_start_time;

		parentgid = find_best_parent(core, member, join_time_passed);
		if (parentgid == NULL) {
			if (!(node_type == SSA_NODE_ACCESS &&
			      join_time_passed < join_timeout) &&
			      is_gid_not_zero((union ibv_gid *) rec->parent_gid))

				/* class specific status */
				umad->packet.mad_hdr.status =
					htons(SSA_STATUS_REQ_DENIED << 8);
		}
	}

	ssa_log(SSA_LOG_CTRL, "sending join response: MAD status 0x%x\n",
		ntohs(umad->packet.mad_hdr.status));
	umad->packet.mad_hdr.method = UMAD_METHOD_GET_RESP;
	umad_send(core->svc.port->mad_portid, core->svc.port->mad_agentid,
		  (void *) umad, sizeof umad->packet, 0, 0);

	if (first_extraction) {
		/* member is orphaned */
		DListInsertBefore(&member->entry, &core->orphan_list);
	} else {
		ret = core_build_tree(core, member, parentgid);
		if (ret) {
			ssa_log(SSA_LOG_CTRL, "core_build_tree failed %d\n", ret);
			/* member is orphaned */
			DListInsertBefore(&member->entry, &core->orphan_list);
		} else {
			if (member->rec.node_type & SSA_NODE_DISTRIBUTION) {
				/* list lock is held in core_process_ssa_mad() */
				core_adopt_orphans(&core->orphan_list, SSA_NODE_ACCESS);
				core_adopt_orphans(&core->orphan_list, SSA_NODE_CONSUMER);
			} else if (member->rec.node_type & SSA_NODE_ACCESS)
				core_adopt_orphans(&core->orphan_list, SSA_NODE_CONSUMER);
			dtree_epoch_cur++;
		}
	}
}

static void core_process_leave(struct ssa_core *core, struct ssa_umad *umad)
{
	struct ssa_member_record *rec;
	struct ssa_member *member;
	uint8_t **tgid;
	DLIST_ENTRY *entry;
	uint8_t node_type;

	rec = &umad->packet.ssa_mad.member;
	ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, rec->port_gid, sizeof rec->port_gid);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", core->svc.name, log_data);

	tgid = tfind(rec->port_gid, &core->member_map, ssa_compare_gid);
	if (tgid) {
		ssa_log(SSA_LOG_CTRL, "removing member\n");
		rec = container_of(*tgid, struct ssa_member_record, port_gid);
		member = container_of(rec, struct ssa_member, rec);
		entry = DListFind(&member->entry, &core->orphan_list);
		if (entry) {
			ssa_log(SSA_LOG_CTRL, "in orphan list\n");
			DListRemove(&member->entry);
		}
		node_type = member->rec.node_type;
		if (node_type & SSA_NODE_CORE) {
			entry = DListFind(&member->entry, &core->core_list);
			if (entry) {
				ssa_log(SSA_LOG_CTRL, "in core list\n");
				DListRemove(&member->entry);	
			}
		}
		if (node_type & SSA_NODE_DISTRIBUTION) {
			entry = DListFind(&member->entry, &core->distrib_list);
			if (entry) {
				ssa_log(SSA_LOG_CTRL, "in distrib list\n");
				DListRemove(&member->entry);
			}
		}
		if (node_type & SSA_NODE_ACCESS) {
			entry = DListFind(&member->access_entry,
					  &core->access_list);
			if (entry) {
				ssa_log(SSA_LOG_CTRL, "in access list\n");
				DListRemove(&member->access_entry);
			}
		}
		core_update_tree(core, member, (union ibv_gid *) rec->port_gid);
		tdelete(rec->port_gid, &core->member_map, ssa_compare_gid);
		free(member);
	}

	ssa_log(SSA_LOG_CTRL, "sending leave response\n");
	umad->packet.mad_hdr.method = SSA_METHOD_DELETE_RESP;
	umad_send(core->svc.port->mad_portid, core->svc.port->mad_agentid,
		  (void *) umad, sizeof umad->packet, 0, 0);
}

void core_init_parent(struct ssa_core *core, struct ssa_mad_packet *mad,
		      struct ssa_member_record *member,
		      struct ssa_member_record *parent,
		      struct ibv_path_record *path)
{
	struct ssa_info_record *rec;

	ssa_init_mad_hdr(&core->svc, &mad->mad_hdr, UMAD_METHOD_SET, SSA_ATTR_INFO_REC);
	mad->ssa_key = 0;	/* TODO: set for real */

	rec = &mad->ssa_mad.info;
	rec->database_id = member->database_id;
	rec->node_type = parent ? parent->node_type : 0;
	rec->path_data.flags = IBV_PATH_FLAG_GMP | IBV_PATH_FLAG_PRIMARY |
			       IBV_PATH_FLAG_BIDIRECTIONAL;
	rec->path_data.path = *path;
}

static void core_process_parent_set(struct ssa_core *core, struct ssa_umad *umad)
{
	/* Ignoring this for now */
}

static void core_process_path_rec(struct ssa_core *core, struct sa_umad *umad)
{
	struct ibv_path_record *path;
	struct uint8_t **childgid, **parentgid;
	struct ssa_member_record *rec;
	struct ssa_member *child, *parent;
	struct ssa_umad umad_sa;
	int ret;

	path = &umad->sa_mad.path_rec.path;
	ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, (uint8_t *) &path->sgid, sizeof path->sgid);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", core->svc.name, log_data);

	/* Joined port GID is SGID in PathRecord */
	childgid = tfind(&path->sgid, &core->member_map, ssa_compare_gid);
	if (!childgid) {
		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
				sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) &path->sgid, sizeof path->sgid);
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"ERROR - couldn't find joined port GID %s\n", log_data);
		return;
	}
	rec = container_of(*childgid, struct ssa_member_record, port_gid);
	child = container_of(rec, struct ssa_member, rec);

	parentgid = tfind(&path->dgid, &core->member_map, ssa_compare_gid);
	if (parentgid) {
		rec = container_of(*parentgid, struct ssa_member_record, port_gid);
		parent = container_of(rec, struct ssa_member, rec);
		child->primary = parent;
		child->primary_state |= SSA_CHILD_PARENTED;

		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				log_data, sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) &path->dgid, sizeof path->dgid); 
		ssa_log(SSA_LOG_DEFAULT,
			"child node type %d parent GID %s children %d access children %d\n",
			child->rec.node_type, log_data, parent->child_num,
			parent->access_child_num);

	} else {
		child->primary = NULL;
		child->primary_state = SSA_CHILD_IDLE;
		ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data,
				sizeof log_data, SSA_ADDR_GID,
				(uint8_t *) &path->dgid, sizeof path->dgid);
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"ERROR - couldn't find parent GID %s\n", log_data);
	}

	/*
	 * TODO: SL should come from another PathRecord between core
	 * and joined client.
	 *
	 * In prototype, since core is coresident with SM, this is SL
	 * from the PathRecord between the client and the parent
	 * since the (only) parent is the core.
	 */
	child->sl = ntohs(path->qosclass_sl) & 0xF;

	memset(&umad_sa, 0, sizeof umad_sa);
	umad_set_addr(&umad_sa.umad, child->lid, 1, child->sl, UMAD_QKEY);
	if (child->primary)
		core_init_parent(core, &umad_sa.packet, &child->rec, &child->primary->rec, path);
	else
		core_init_parent(core, &umad_sa.packet, &child->rec, NULL, path);

	ssa_log(SSA_LOG_CTRL, "sending set parent\n");
	ret = umad_send(core->svc.port->mad_portid, core->svc.port->mad_agentid,
			(void *) &umad_sa, sizeof umad_sa.packet, core->svc.umad_timeout, 0);
	if (ret)
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"ERROR - failed to send set parent\n");
}

static int core_process_sa_mad(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	struct ibv_path_record *path;
	struct ssa_core *core;
	struct sa_umad *umad_sa;

	umad_sa = &msg->data.umad_sa;
	if (umad_sa->umad.status) {
		ssa_log(SSA_LOG_DEFAULT,
			"SA MAD method 0x%x attribute 0x%x received with status 0x%x\n",
			umad_sa->sa_mad.packet.mad_hdr.method,
			ntohs(umad_sa->sa_mad.packet.mad_hdr.attr_id),
			umad_sa->umad.status);

		if (ntohs(umad_sa->sa_mad.packet.mad_hdr.attr_id) ==
		    UMAD_SA_ATTR_PATH_REC) {
			core = container_of(svc, struct ssa_core, svc);
			path = &umad_sa->sa_mad.path_rec.path;
			core_update_children_counter(core, &path->dgid, &path->sgid, 0);
		}

		return 1;
	}

	core = container_of(svc, struct ssa_core, svc);

	switch (umad_sa->sa_mad.packet.mad_hdr.method) {
	case UMAD_METHOD_GET_RESP:
		if (ntohs(umad_sa->sa_mad.packet.mad_hdr.attr_id) ==
		    UMAD_SA_ATTR_PATH_REC) {
			core_process_path_rec(core, umad_sa);
			return 1;
		}
		break;
	default:
		ssa_log(SSA_LOG_DEFAULT,
			"SA MAD method 0x%x attribute 0x%x not expected\n",
			umad_sa->sa_mad.packet.mad_hdr.method,
			ntohs(umad_sa->sa_mad.packet.mad_hdr.attr_id));
		break;
	}

	return 0;
}

static int core_process_ssa_mad(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	struct ssa_core *core;
	struct ssa_umad *umad;

	umad = &msg->data.umad;
	if (umad->umad.status) {
		ssa_log(SSA_LOG_DEFAULT,
			"SSA MAD method 0x%x (%s) attribute 0x%x (%s) received with status 0x%x\n",
			umad->packet.mad_hdr.method,
			ssa_method_str(umad->packet.mad_hdr.method),
			ntohs(umad->packet.mad_hdr.attr_id),
			ssa_attribute_str(umad->packet.mad_hdr.attr_id),
			umad->umad.status);
		return 0;
	}

	core = container_of(svc, struct ssa_core, svc);

	switch (umad->packet.mad_hdr.method) {
	case UMAD_METHOD_SET:
		if (ntohs(umad->packet.mad_hdr.attr_id) == SSA_ATTR_MEMBER_REC) {
			pthread_mutex_lock(&core->list_lock);
			core_process_join(core, umad);
			pthread_mutex_unlock(&core->list_lock);
			return 1;
		}
		break;
	case SSA_METHOD_DELETE:
		if (ntohs(umad->packet.mad_hdr.attr_id) == SSA_ATTR_MEMBER_REC) {
			pthread_mutex_lock(&core->list_lock);
			core_process_leave(core, umad);
			pthread_mutex_unlock(&core->list_lock);
			return 1;
		}
		break;
	case UMAD_METHOD_GET_RESP:
		if (ntohs(umad->packet.mad_hdr.attr_id) == SSA_ATTR_INFO_REC) {
			core_process_parent_set(core, umad);
			return 1;
		}
		break;
	default:
		ssa_log(SSA_LOG_DEFAULT,
			"SSA MAD method 0x%x (%s) attribute 0x%x (%s) not expected\n",
			umad->packet.mad_hdr.method,
			ssa_method_str(umad->packet.mad_hdr.method),
			ntohs(umad->packet.mad_hdr.attr_id),
			ssa_attribute_str(umad->packet.mad_hdr.attr_id));
		break;
	}

	return 0;
}

static int core_process_dev_event(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n",
		svc->name, ibv_event_type_str(msg->data.event));
	switch (msg->data.event) {
	case IBV_EVENT_SM_CHANGE:
		core_clean_tree(svc);
		first_extraction = 1;
		break;
	default:
		break;
	};
	return 0;
}

static int core_process_msg(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	switch(msg->hdr.type) {
	case SSA_CTRL_MAD:
		return core_process_ssa_mad(svc, msg);
	case SSA_SA_MAD:
		return core_process_sa_mad(svc, msg);
	case SSA_CTRL_DEV_EVENT:
		return core_process_dev_event(svc, msg);
	case SSA_CTRL_EXIT:
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL,
			     "ignoring unexpected message type %d\n",
			     msg->hdr.type);
		break;
	}
	return 0;
}

static void *core_ctrl_handler(void *context)
{
	int ret;

	SET_THREAD_NAME(ctrl_thread, "CTRL");

	ret = ssa_ctrl_run(&ssa);
	if (ret)
		ssa_log(SSA_LOG_DEFAULT, "ERROR processing control\n");

	return context;
}

static int core_init_svc(struct ssa_svc *svc)
{
	struct ssa_core *core = container_of(svc, struct ssa_core, svc);
	int ret;

	ret = pthread_mutex_init(&core->list_lock, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable initialize core member lists lock\n");
		return -1;
        }

	DListInit(&core->orphan_list);
	DListInit(&core->core_list);
	DListInit(&core->distrib_list);
	DListInit(&core->access_list);
	return 0;
}

static void core_free_member(void *gid)
{
	struct ssa_member *member;
	struct ssa_member_record *rec;
	rec = container_of(gid, struct ssa_member_record, port_gid);
	member = container_of(rec, struct ssa_member, rec);
	free(member);
}

static void core_destroy_svc(struct ssa_svc *svc)
{
	struct ssa_core *core = container_of(svc, struct ssa_core, svc);
	ssa_log_func(SSA_LOG_CTRL);
	if (core->member_map)
		tdestroy(core->member_map, core_free_member);
	pthread_mutex_destroy(&core->list_lock);
}
#endif

static const char *sm_state_str(int state)
{
	switch (state) {
	case IB_SMINFO_STATE_DISCOVERING:
		return "Discovering";
	case IB_SMINFO_STATE_STANDBY:
		return "Standby";
	case IB_SMINFO_STATE_NOTACTIVE:
		return "Not Active";
	case IB_SMINFO_STATE_MASTER:
		return "Master";
	}
	return "UNKNOWN";
}

static void handle_trap_event(ib_mad_notice_attr_t *p_ntc)
{
	if (ib_notice_is_generic(p_ntc)) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE,
			"Generic trap type %d event %d from LID %u\n",
			ib_notice_get_type(p_ntc),
			ntohs(p_ntc->g_or_v.generic.trap_num),
			ntohs(p_ntc->issuer_lid));
	} else {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE,
			"Vendor trap type %d from LID %u\n",
			ib_notice_get_type(p_ntc),
			ntohs(p_ntc->issuer_lid));
	}
}

#ifndef SIM_SUPPORT
static void ssa_extract_send_db_update_prepare(int fd)
{
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_UPDATE_PREPARE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = NULL;
	msg.db_upd.svc = NULL;
	msg.db_upd.flags = 0;
	msg.db_upd.epoch = DB_EPOCH_INVALID;
	write(fd, (char *) &msg, sizeof(msg));
}

static void ssa_extract_send_db_update(struct ssa_db *db, int fd, int flags)
{
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_UPDATE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = db;
	msg.db_upd.svc = NULL;
	msg.db_upd.flags = flags;
	memset(&msg.db_upd.remote_gid, 0, sizeof(msg.db_upd.remote_gid));
	msg.db_upd.remote_lid = 0;
	msg.db_upd.epoch = ssa_db_get_epoch(db, DB_DEF_TBL_ID);
	write(fd, (char *) &msg, sizeof(msg));
}

static int ssa_extract_db_update_prepare(struct ssa_db *db)
{
	struct ssa_svc *svc;
	struct ssa_port *port;
	int d, p, s, count = 0;

	if (!db)
		return count;

	for (d = 0; d < ssa.dev_cnt; d++) {
		for (p = 1; p <= ssa_dev(&ssa, d)->port_cnt; p++) {
			port = ssa_dev_port(ssa_dev(&ssa, d), p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < port->svc_cnt; s++) {
				svc = port->svc[s];
				ssa_extract_send_db_update_prepare(svc->sock_extractdown[1]);
				count++;
			}
		}
	}

	if (ssa.node_type & SSA_NODE_ACCESS) {
		ssa_extract_send_db_update_prepare(sock_accessextract[0]);
		count++;
	}

	return count;
}

static void ssa_extract_db_update(struct ssa_db *db, int db_changed)
{
	struct ssa_svc *svc;
	struct ssa_port *port;
	int d, p, s;
	enum ssa_db_update_flag flags = 0;

	if (!db)
		return;

	if (db_changed)
		flags |= SSA_DB_UPDATE_CHANGE;

	for (d = 0; d < ssa.dev_cnt; d++) {
		for (p = 1; p <= ssa_dev(&ssa, d)->port_cnt; p++) {
			port = ssa_dev_port(ssa_dev(&ssa, d), p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < port->svc_cnt; s++) {
				svc = port->svc[s];
				ssa_extract_send_db_update(db,
							   svc->sock_extractdown[1], flags);
			}
		}
	}

	if (ssa.node_type & SSA_NODE_ACCESS)
		ssa_extract_send_db_update(db, sock_accessextract[0], flags);

	ssa_db_update_change_stats(db);
}
#endif

#ifdef SIM_SUPPORT_SMDB
static int ssa_extract_load_smdb(osm_opensm_t *p_osm, struct ssa_db *p_ref_smdb,
				 int *outstanding_count, struct timespec *last_mtime)
{
	struct stat smdb_dir_stats;
	int ret;

	if (!smdb_dump)
		return 0;

	ret = lockf(smdb_lock_fd, F_TLOCK, 0);
	if (ret) {
		if ((errno == EACCES) || (errno == EAGAIN)) {
			ssa_log_warn(SSA_LOG_VERBOSE,
				     "smdb lock file is locked\n");
			return 0;
		} else {
			ssa_log_err(SSA_LOG_DEFAULT,
				    "locking smdb lock file ERROR %d (%s)\n",
				    errno, strerror(errno));
			return -1;
		}
	}

	ret = stat(smdb_dump_dir, &smdb_dir_stats);
	if (ret < 0) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to get SMDB directory stats\n");
		lockf(smdb_lock_fd, F_ULOCK, 0);
		return -1;
	}

	if (memcmp(&smdb_dir_stats.st_mtime, last_mtime, sizeof(*last_mtime))) {
		ssa_extract_process(p_osm, p_ref_smdb, outstanding_count);
		memcpy(last_mtime, &smdb_dir_stats.st_mtime,
		       sizeof(*last_mtime));
	}

	lockf(smdb_lock_fd, F_ULOCK, 0);
	return 0;
}

static int ssa_extract_process_smdb(struct ssa_db **pp_smdb)
{
	struct ssa_db *p_smdb = NULL;

	p_smdb = ssa_db_load(smdb_dump_dir, smdb_dump);
	if (!p_smdb) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to load SMDB from %s mode (%d)\n",
			    smdb_dump_dir, smdb_dump);
		return -1;
	}

	if (pp_smdb) {
		if (*pp_smdb)
			ssa_db_destroy(*pp_smdb);
		*pp_smdb = p_smdb;
	}

	return 0;
}
#else
static void core_extract_db(osm_opensm_t *p_osm)
{
	struct ssa_db_diff *ssa_db_diff_old = NULL;
	uint64_t prev_epochs[SMDB_TBL_ID_MAX + 1];
	int i;

	prev_epochs[SMDB_TBL_ID_MAX] = DB_EPOCH_INVALID;

	CL_PLOCK_ACQUIRE(&p_osm->lock);
	ssa_db->p_dump_db = ssa_db_extract(p_osm);
	ssa_db_lft_handle();
	CL_PLOCK_RELEASE(&p_osm->lock);

	/* For validation */
	ssa_db_validate(ssa_db->p_dump_db);
	ssa_db_validate_lft(first_extraction);

	/* Update SMDB versions */
	ssa_db_update(ssa_db);

	pthread_mutex_lock(&ssa_db_diff_lock);
	/* Clear previous version */
	if (ssa_db_diff) {
		for (i = 0; i < ssa_db_diff->p_smdb->data_tbl_cnt; i++)
			prev_epochs[i] =
				ssa_db_get_epoch(ssa_db_diff->p_smdb, i);
		prev_epochs[SMDB_TBL_ID_MAX] =
			ssa_db_get_epoch(ssa_db_diff->p_smdb, DB_DEF_TBL_ID);
	}

	ssa_db_diff_old = ssa_db_diff;

	ssa_db_diff = ssa_db_compare(ssa_db, prev_epochs, first_extraction);
	if (ssa_db_diff) {
		if (ssa_db_diff->dirty)
		    ssa_db_diff_destroy(ssa_db_diff_old);
		else if (ssa_db_diff_old) {
		    ssa_db_diff_destroy(ssa_db_diff);
		    ssa_db_diff = ssa_db_diff_old;
		    ssa_db_diff->dirty = 0;
		}

		/*
		 * TODO: use 'ssa_db_get_epoch(ssa_db_diff->p_smdb, DB_DEF_TBL_ID)'
		 * for getting current epoch and sending it to children.
		 */
#ifdef SIM_SUPPORT
		if (smdb_dump && !lockf(smdb_lock_fd, F_LOCK, 0)) {
			ssa_db_save(smdb_dump_dir, ssa_db_diff->p_smdb, smdb_dump);
			lockf(smdb_lock_fd, F_ULOCK, 0);
		}
#else
		if (smdb_dump)
			ssa_db_save(smdb_dump_dir, ssa_db_diff->p_smdb, smdb_dump);

		ssa_extract_db_update(ssa_db_diff->p_smdb, ssa_db_diff->dirty);
#endif
	}
	pthread_mutex_unlock(&ssa_db_diff_lock);
}
#endif

#ifndef SIM_SUPPORT
#ifdef SIM_SUPPORT_SMDB
static void ssa_extract_update_ready_process(osm_opensm_t *p_osm,
					     struct ssa_db *p_ref_smdb,
					     int *outstanding_count)
{
	if (*outstanding_count > 0) {
		if (--(*outstanding_count) == 0) {
			if (lockf(smdb_lock_fd, F_LOCK, 0)) {
				if (errno == EDEADLK)
					ssa_log_warn(SSA_LOG_VERBOSE,
						     "locking smdb lock file would cause a deadlock\n");
				else
					ssa_log_err(SSA_LOG_DEFAULT,
						    "locking smdb lock file ERROR %d (%s)\n",
						    errno, strerror(errno));
				return;
			}

			if (!ssa_extract_process_smdb(&p_ref_smdb))
				ssa_extract_db_update(p_ref_smdb, 1); /* 1 indicates that smdb was changed */
			lockf(smdb_lock_fd, F_ULOCK, 0);
		}
	}
}

static int ssa_extract_process(osm_opensm_t *p_osm, struct ssa_db *p_ref_smdb,
			       int *outstanding_count)
{
	if (*outstanding_count == 0) {
		if (p_ref_smdb)
			*outstanding_count = ssa_extract_db_update_prepare(p_ref_smdb);
ssa_log(SSA_LOG_DEFAULT, "%d DB update prepare msgs sent\n", *outstanding_count);
		if (*outstanding_count == 0) {
			if (!ssa_extract_process_smdb(&p_ref_smdb))
				ssa_extract_db_update(p_ref_smdb, 1); /* 1 indicates that smdb was changed */
ssa_log(SSA_LOG_DEFAULT, "DB extracted and DB update msgs sent\n");
		}
else ssa_log(SSA_LOG_DEFAULT, "extract event but extract now pending with outstanding count %d\n", *outstanding_count);
	}
else ssa_log(SSA_LOG_DEFAULT, "extract event with extract already pending\n");

	return 0;
}
#else
static void ssa_extract_update_ready_process(osm_opensm_t *p_osm,
					     int *outstanding_count)
{
	if (*outstanding_count > 0) {
		if (--(*outstanding_count) == 0) {
			core_extract_db(p_osm);
		}
	}
}

static int ssa_extract_process(osm_opensm_t *p_osm, int *outstanding_count)
{
	if (*outstanding_count == 0) {
		if (ssa_db_diff)
			*outstanding_count = ssa_extract_db_update_prepare(ssa_db_diff->p_smdb);
ssa_log(SSA_LOG_DEFAULT, "%d DB update prepare msgs sent\n", *outstanding_count);
		if (*outstanding_count == 0) {
			core_extract_db(p_osm);
ssa_log(SSA_LOG_DEFAULT, "DB extracted and DB update msgs sent\n");
		}
else ssa_log(SSA_LOG_DEFAULT, "extract event but extract now pending with outstanding count %d\n", *outstanding_count);
	}
else ssa_log(SSA_LOG_DEFAULT, "extract event with extract already pending\n");

	return 0;
}
#endif
#endif

#ifndef SIM_SUPPORT
static void core_process_extract_data(struct ssa_extract_data *data)
{
	struct ssa_core *core;
	int i;

	for (i = 0; i < data->num_svcs; i++) {
		core = container_of(data->svcs[i], struct ssa_core, svc);
		core_process_orphans(core);
	}
}

static int core_has_orphans(struct ssa_extract_data *data)
{
	struct ssa_core *core;
	int i, ret = 0;

	for (i = 0; i < data->num_svcs; i++) {
		core = container_of(data->svcs[i], struct ssa_core, svc);
		pthread_mutex_lock(&core->list_lock);
		if (!DListEmpty(&core->orphan_list)) {
			ret = 1;
			pthread_mutex_unlock(&core->list_lock);
			break;
		}
		pthread_mutex_unlock(&core->list_lock);
	}
	return ret;
}

static void core_start_timer(struct pollfd **fds, int fd_slot,
			     time_t time_sec, time_t interval_sec)
{
	struct itimerspec timer;
	struct pollfd *pfd;
	int ret;

	timer.it_value.tv_sec		= time_sec;
	timer.it_value.tv_nsec		= 0;
	timer.it_interval.tv_sec	= interval_sec;
	timer.it_interval.tv_nsec	= 0;

	pfd = (struct pollfd *)(fds + fd_slot);
	ret = timerfd_settime(pfd->fd, 0, &timer, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL,
			    "timerfd_settime %d %d (%s) for fd %d\n",
			    ret, errno, strerror(errno), fd_slot);
		close(pfd->fd);
		pfd->fd = -1;
		return;
	}
	pfd->events = POLLIN;
	pfd->revents = 0;
}
#endif

static void *core_extract_handler(void *context)
{
	struct ssa_extract_data *p_extract_data = (struct ssa_extract_data *) context;
	osm_opensm_t *p_osm = p_extract_data->opensm;
	struct pollfd **fds = NULL;
	struct pollfd *pfd;
	struct ssa_db_ctrl_msg msg;
	struct ssa_ctrl_msg_buf msg2;
	int ret, i, timeout_msec = -1;
#ifndef SIM_SUPPORT
	int outstanding_count = 0;
#endif
#ifdef SIM_SUPPORT_SMDB
	struct timespec smdb_last_mtime;
#endif

	SET_THREAD_NAME(extract_thread, "EXTRACT");

	ssa_log(SSA_LOG_VERBOSE, "Starting smdb extract thread\n");

#ifdef SIM_SUPPORT_SMDB
	timeout_msec = 1000;	/* 1 sec */
	memset(&smdb_last_mtime, 0, sizeof(smdb_last_mtime));
#endif

	fds = calloc(p_extract_data->num_svcs + FIRST_DOWNSTREAM_FD_SLOT,
		     sizeof(**fds));
	if (!fds)
		goto out2;
	pfd = (struct pollfd *)fds;
	pfd->fd = sock_coreextract[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 1);
	pfd->fd = sock_accessextract[0];
	if (sock_accessextract[0] >= 0)
		pfd->events = POLLIN;
	else
		pfd->events = 0;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + EXTRACT_TIMER_FD_SLOT);
	pfd->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (pfd->fd < 0) {
		ssa_log_err(SSA_LOG_CTRL, "timerfd_create %d (%s)\n",
			    errno, strerror(errno));
		goto out;
	}
	pfd->events = 0;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + TREE_BALANCE_TIMER_FD_SLOT);
	pfd->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (pfd->fd < 0) {
		ssa_log_err(SSA_LOG_CTRL, "timerfd_create %d (%s)\n",
			    errno, strerror(errno));
		goto out;
	}
	pfd->events = 0;
	pfd->revents = 0;
	for (i = 0; i < p_extract_data->num_svcs; i++) {
		pfd = (struct pollfd *)(fds + i + FIRST_DOWNSTREAM_FD_SLOT);
		pfd->fd = p_extract_data->svcs[i]->sock_extractdown[1];
		pfd->events = POLLIN;
		pfd->revents = 0;
	}

	for (;;) {
		ret = poll((struct pollfd *)fds,
			   p_extract_data->num_svcs + FIRST_DOWNSTREAM_FD_SLOT,
			   timeout_msec);
		if (ret < 0) {
			ssa_log(SSA_LOG_VERBOSE, "ERROR polling fds\n");
			continue;
		}

		if (!ret) {
#ifndef SIM_SUPPORT
			core_process_extract_data(p_extract_data);
			if (!core_has_orphans(p_extract_data))
				timeout_msec = -1;
#endif
#ifdef SIM_SUPPORT_SMDB
			if (ssa_extract_load_smdb(p_osm, p_ref_smdb,
						  &outstanding_count,
						  &smdb_last_mtime) < 0)
				goto out;
			timeout_msec = 1000;
			continue;
#endif
		}

		pfd = (struct pollfd *)fds;
		if (pfd->revents) {
			pfd->revents = 0;
			read(sock_coreextract[1], (char *) &msg, sizeof(msg));
			switch (msg.type) {
			case SSA_DB_START_EXTRACT:
#ifndef SIM_SUPPORT
				if (first_extraction) {
					core_process_extract_data(p_extract_data);
					core_start_timer(fds, TREE_BALANCE_TIMER_FD_SLOT, CORE_BALANCE_TIMEOUT, 0);
				}
#endif
#ifdef SIM_SUPPORT
				core_extract_db(p_osm);
#elif !defined(SIM_SUPPORT_SMDB)
				ssa_extract_process(p_osm, &outstanding_count);
#endif
				if (first_extraction)
					first_extraction = 0;

#ifndef SIM_SUPPORT
				if (core_has_orphans(p_extract_data))
					timeout_msec = 1000;

				core_start_timer(fds, EXTRACT_TIMER_FD_SLOT, 1, 1);
#endif
				break;
			case SSA_DB_LFT_CHANGE:
				ssa_log(SSA_LOG_VERBOSE,
					"Start handling LFT change event\n");
				ssa_db_lft_handle();
				break;
			case SSA_DB_EXIT:
				goto out;
			default:
				ssa_log(SSA_LOG_VERBOSE,
					"ERROR: Unknown msg type %d from extract\n",
					msg.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 1);
		if (pfd->revents) {
			pfd->revents = 0;
			read(sock_accessextract[0], (char *) &msg2,
			     sizeof(msg2.hdr));
			if (msg2.hdr.len > sizeof msg2.hdr) {
				read(sock_accessextract[0],
				     (char *) &msg2.hdr.data,
				     msg2.hdr.len - sizeof msg2.hdr);
			}
			switch (msg2.hdr.type) {
#ifndef SIM_SUPPORT
			case SSA_DB_UPDATE_READY:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_READY from access with outstanding count %d\n", outstanding_count);
#ifdef SIM_SUPPORT_SMDB
				ssa_extract_update_ready_process(p_osm,
								 p_ref_smdb,
								 &outstanding_count);
#else
				ssa_extract_update_ready_process(p_osm,
								 &outstanding_count);
#endif
				break;
#endif
			default:
				ssa_log(SSA_LOG_VERBOSE,
					"ERROR: Unknown msg type %d from access\n",
					msg2.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + EXTRACT_TIMER_FD_SLOT);
		if (pfd->revents & POLLIN) {
			ssize_t s;
			uint64_t exp;

			s = read(pfd->fd, &exp, sizeof exp);
			if (s != sizeof exp) {
				ssa_log_err(SSA_LOG_DEFAULT,
					    "%" PRId64 " bytes read\n", s);
			} else {
#ifndef SIM_SUPPORT
				for (i = 0; i < p_extract_data->num_svcs; i++) {
					struct ssa_svc *svc = p_extract_data->svcs[i];
					struct ssa_core *core;

					core = container_of(svc, struct ssa_core, svc);
					if (dtree_epoch_prev != dtree_epoch_cur &&
					    svc->state != SSA_STATE_IDLE) {
						if (pthread_mutex_trylock(&core->list_lock) == 0) {
							core_dump_tree(core, svc->name);
							pthread_mutex_unlock(&core->list_lock);
						}
						dtree_epoch_prev = dtree_epoch_cur;
					}
				}
#endif
			}

			pfd->revents = 0;
		}

		pfd = (struct pollfd *)(fds + TREE_BALANCE_TIMER_FD_SLOT);
		if (pfd->revents & POLLIN) {
			ssize_t s;
			uint64_t exp;

			s = read(pfd->fd, &exp, sizeof exp);
			if (s != sizeof exp) {
				ssa_log_err(SSA_LOG_DEFAULT,
					    "%" PRId64 " bytes read\n", s);
			} else {
#ifndef SIM_SUPPORT
				for (i = 0; i < p_extract_data->num_svcs; i++) {
					struct ssa_svc *svc = p_extract_data->svcs[i];
					struct ssa_core *core;

					if (svc->port->state == IBV_PORT_ACTIVE) {
						core = container_of(svc, struct ssa_core, svc);
						core_rebalance_tree(core);
					}
				}
#endif
			}

			pfd->revents = 0;
		}


		for (i = 0; i < p_extract_data->num_svcs; i++) {
			pfd = (struct pollfd *)(fds + i + FIRST_DOWNSTREAM_FD_SLOT);
			if (pfd->revents) {
				pfd->revents = 0;
				read(p_extract_data->svcs[i]->sock_extractdown[1],
				     (char *) &msg2, sizeof msg2.hdr);
				if (msg2.hdr.len > sizeof msg2.hdr) {
					read(p_extract_data->svcs[i]->sock_extractdown[1],
					     (char *) &msg2.hdr.data,
					     msg2.hdr.len - sizeof msg2.hdr);
				}
#if 0
				if (svc->process_msg && svc->process_msg(svc, &msg2))
					continue;
#endif

				switch (msg2.hdr.type) {
#ifndef SIM_SUPPORT
				case SSA_DB_UPDATE_READY:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_READY on pfds[%u] with outstanding count %d\n", i, outstanding_count);
#ifdef SIM_SUPPORT_SMDB
					ssa_extract_update_ready_process(p_osm,
									 p_ref_smdb,
									 &outstanding_count);
#else
					ssa_extract_update_ready_process(p_osm,
									 &outstanding_count);
#endif
					break;
#endif
				default:
					ssa_log(SSA_LOG_VERBOSE,
						"ERROR: Unknown msg type %d " 
						"from downstream\n",
						msg2.hdr.type);
					break;
				}
			}
		}
	}
out:
	pfd = (struct pollfd *)(fds + TREE_BALANCE_TIMER_FD_SLOT);
	if (pfd->fd >= 0) {
		close(pfd->fd);
		pfd->events = 0;
		pfd->revents = 0;
	}

	pfd = (struct pollfd *)(fds + EXTRACT_TIMER_FD_SLOT);
	if (pfd->fd >= 0) {
		close(pfd->fd);
		pfd->events = 0;
		pfd->revents = 0;
	}
out2:
	ssa_log(SSA_LOG_VERBOSE, "Exiting smdb extract thread\n");
	free(fds);

	/* currently memory deallocation is done in ssa_close_devices() */
#if 0
#ifdef SIM_SUPPORT_SMDB
	if (p_ref_smdb) {
		ssa_db_destroy(p_ref_smdb);
		p_ref_smdb = NULL;
	}
#endif
#endif

	pthread_exit(NULL);
}

static void core_send_msg(enum ssa_db_ctrl_msg_type type)
{
	struct ssa_db_ctrl_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	ssa_log(SSA_LOG_VERBOSE,
		"Sending msg type %d from core to extract thread\n", type);
	msg.len = sizeof(msg);
	msg.type = type;
	write(sock_coreextract[0], (char *) &msg, sizeof(msg));
}

#ifndef SIM_SUPPORT_SMDB
static void core_process_lft_change(osm_epi_lft_change_event_t *p_lft_change)
{
	struct ssa_db_lft_change_rec *p_lft_change_rec;
	size_t size;

	if (!p_lft_change || !p_lft_change->p_sw)
		return;

	ssa_log(SSA_LOG_VERBOSE, "LFT change event for switch GUID 0x%" PRIx64"\n",
		ntohll(osm_node_get_node_guid(p_lft_change->p_sw->p_node)));

	size = sizeof(*p_lft_change_rec);
	if (p_lft_change->flags == LFT_CHANGED_BLOCK)
		size += sizeof(p_lft_change_rec->block[0]) * UMAD_LEN_SMP_DATA;

	p_lft_change_rec = (struct ssa_db_lft_change_rec *) malloc(size);
	if (!p_lft_change_rec) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to allocate LFT change object\n");
		return;
	}

	memcpy(&p_lft_change_rec->lft_change, p_lft_change,
	       sizeof(p_lft_change_rec->lft_change));
	p_lft_change_rec->lid = osm_node_get_base_lid(p_lft_change->p_sw->p_node, 0);

	if (p_lft_change->flags == LFT_CHANGED_BLOCK)
		memcpy(p_lft_change_rec->block, p_lft_change->p_sw->lft +
		       p_lft_change->block_num * UMAD_LEN_SMP_DATA,
		       UMAD_LEN_SMP_DATA);

	pthread_mutex_lock(&ssa_db->lft_rec_list_lock);
	cl_qlist_insert_tail(&ssa_db->lft_rec_list, &p_lft_change_rec->list_item);
	pthread_mutex_unlock(&ssa_db->lft_rec_list_lock);

	core_send_msg(SSA_DB_LFT_CHANGE);
}
#endif

#ifdef SIM_SUPPORT_SMDB
static void core_report(void *context, osm_epi_event_id_t event_id, void *event_data)
{
	switch (event_id) {
	case OSM_EVENT_ID_TRAP:
		handle_trap_event((ib_mad_notice_attr_t *) event_data);
		break;
	case OSM_EVENT_ID_LFT_CHANGE:
		ssa_log(SSA_LOG_VERBOSE, "LFT change event\n");
		break;
	case OSM_EVENT_ID_UCAST_ROUTING_DONE:
		ssa_log(SSA_LOG_VERBOSE, "Ucast routing done event\n");
		break;
	case OSM_EVENT_ID_SUBNET_UP:
		/* For now, ignore SUBNET UP events when there is subnet init error */
		if (osm->subn.subnet_initialization_error)
			break;
		ssa_log(SSA_LOG_VERBOSE, "Subnet up event\n");
		core_send_msg(SSA_DB_START_EXTRACT);
		break;
	case OSM_EVENT_ID_STATE_CHANGE:
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE,
			"SM state (%u: %s) change event\n",
			osm->subn.sm_state,
			sm_state_str(osm->subn.sm_state));
		break;
	default:
		/* Ignoring all other events for now... */
		if (event_id >= OSM_EVENT_ID_MAX) {
			ssa_log(SSA_LOG_ALL, "Unknown event (%d)\n", event_id);
			osm_log(&osm->log, OSM_LOG_ERROR,
				"Unknown event (%d) reported to SSA plugin\n",
				event_id);
		}
	}
}
#else
static void core_report(void *context, osm_epi_event_id_t event_id, void *event_data)
{
	osm_epi_ucast_routing_flags_t ucast_routing_flag;

	switch (event_id) {
	case OSM_EVENT_ID_TRAP:
		handle_trap_event((ib_mad_notice_attr_t *) event_data);
		break;
	case OSM_EVENT_ID_LFT_CHANGE:
		ssa_log(SSA_LOG_VERBOSE, "LFT change event\n");
		core_process_lft_change((osm_epi_lft_change_event_t *) event_data);
		break;
	case OSM_EVENT_ID_UCAST_ROUTING_DONE:
		ucast_routing_flag = (osm_epi_ucast_routing_flags_t) event_data;
		if (ucast_routing_flag == UCAST_ROUTING_REROUTE) {
			/* We get here in case of subnet rerouting not followed by SUBNET_UP */
			/* TODO: notify the distribution thread and push the LFT changes */
			ssa_log(SSA_LOG_VERBOSE,
				"Unicast rerouting completed event - not implemented yet\n");
		}
		break;
	case OSM_EVENT_ID_SUBNET_UP:
		/* For now, ignore SUBNET UP events when there is subnet init error */
		if (osm->subn.subnet_initialization_error)
			break;
		ssa_log(SSA_LOG_VERBOSE, "Subnet up event\n");
		core_send_msg(SSA_DB_START_EXTRACT);
		break;
	case OSM_EVENT_ID_STATE_CHANGE:
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE,
			"SM state (%u: %s) change event currently ignored\n",
			osm->subn.sm_state,
			sm_state_str(osm->subn.sm_state));
		if (osm->subn.sm_state != IB_SMINFO_STATE_MASTER)
			first_extraction = 1;
		break;
	default:
		/* Ignoring all other events for now... */
		if (event_id >= OSM_EVENT_ID_MAX) {
			ssa_log(SSA_LOG_ALL, "Unknown event (%d)\n", event_id);
			osm_log(&osm->log, OSM_LOG_ERROR,
				"Unknown event (%d) reported to SSA plugin\n",
				event_id);
		}
	}
}
#endif

static int core_convert_node_type(const char *node_type_string)
{
	int node_type = SSA_NODE_CORE;

	if (!strcasecmp("combined", node_type_string))
		node_type |= SSA_NODE_ACCESS;
	return node_type;
}

static void core_set_options(void)
{
	FILE *f;
	char s[160];
	char opt[32], value[128];

	if (!(f = fopen(opts_file, "r")))
		return;

	while (fgets(s, sizeof s, f)) {
		if (s[0] == '#')
			continue;

		if (sscanf(s, "%31s%127s", opt, value) != 2)
			continue;

		if (!strcasecmp("log_file", opt))
			strcpy(log_file, value);
		else if (!strcasecmp("log_level", opt))
			ssa_set_log_level(atoi(value));
#ifndef SIM_SUPPORT
		else if (!strcasecmp("distrib_tree_level", opt))
			distrib_tree_level = atoi(value);
		else if (!strcasecmp("join_timeout", opt))
			join_timeout = atoi(value);
#endif
		else if (!strcasecmp("log_flush", opt))
			log_flush = atoi(value);
		else if (!strcasecmp("accum_log_file", opt))
			accum_log_file = atoi(value);
		else if (!strcasecmp("lock_file", opt))
			strcpy(lock_file, value);
		else if (!strcasecmp("smdb_dump_dir", opt))
			strcpy(smdb_dump_dir, value);
		else if (!strcasecmp("prdb_dump_dir", opt))
			strcpy(prdb_dump_dir, value);
		else if (!strcasecmp("node_type", opt))
			node_type = core_convert_node_type(value);
		else if (!strcasecmp("smdb_port", opt))
			smdb_port = (short) atoi(value);
		else if (!strcasecmp("prdb_port", opt))
			prdb_port = (short) atoi(value);
		else if (!strcasecmp("smdb_dump", opt))
			smdb_dump = atoi(value);
		else if (!strcasecmp("err_smdb_dump", opt))
			err_smdb_dump = atoi(value);
		else if (!strcasecmp("prdb_dump", opt))
			prdb_dump = atoi(value);
		else if (!strcasecmp("smdb_deltas", opt))
			smdb_deltas = atoi(value);
		else if (!strcasecmp("keepalive", opt))
			keepalive = atoi(value);
#ifdef SIM_SUPPORT_FAKE_ACM
		else if (!strcasecmp("fake_acm_num", opt))
			fake_acm_num = atoi(value);
#endif
		else if (!strcasecmp("addr_preload", opt))
			addr_preload = atoi(value);
		else if (!strcasecmp("addr_data_file", opt))
			strncpy(addr_data_file, value, sizeof(addr_data_file));
	}

	fclose(f);
}

static void core_log_options(void)
{
	ssa_log_options();
	ssa_log(SSA_LOG_DEFAULT, "config file %s\n", opts_file);
	ssa_log(SSA_LOG_DEFAULT, "lock file %s\n", lock_file);
	ssa_log(SSA_LOG_DEFAULT, "node type %d (%s)\n", node_type,
		ssa_node_type_str(node_type));
	ssa_log(SSA_LOG_DEFAULT, "smdb port %u\n", smdb_port);
	ssa_log(SSA_LOG_DEFAULT, "prdb port %u\n", prdb_port);
	ssa_log(SSA_LOG_DEFAULT, "admin port %u\n", admin_port);
	ssa_log(SSA_LOG_DEFAULT, "smdb dump %d\n", smdb_dump);
	ssa_log(SSA_LOG_DEFAULT, "err smdb dump %d\n", err_smdb_dump);
	ssa_log(SSA_LOG_DEFAULT, "smdb dump dir %s\n", smdb_dump_dir);
	ssa_log(SSA_LOG_DEFAULT, "prdb dump %d\n", prdb_dump);
	ssa_log(SSA_LOG_DEFAULT, "prdb dump dir %s\n", prdb_dump_dir);
	ssa_log(SSA_LOG_DEFAULT, "smdb deltas %d\n", smdb_deltas);
	ssa_log(SSA_LOG_DEFAULT, "keepalive time %d\n", keepalive);
#ifndef SIM_SUPPORT
	ssa_log(SSA_LOG_DEFAULT, "distrib tree level 0x%x\n", distrib_tree_level);
#endif
#ifdef SIM_SUPPORT_SMDB
	ssa_log(SSA_LOG_DEFAULT, "running in simulated SMDB operation mode\n");
#endif
#ifdef SIM_SUPPORT_FAKE_ACM
	if (node_type & SSA_NODE_ACCESS) {
		ssa_log(SSA_LOG_DEFAULT, "running in ACM clients simulated mode\n");
		if (fake_acm_num >= 0)
			ssa_log(SSA_LOG_DEFAULT, "Max. number of simulated"
				" clients is %d\n", fake_acm_num);
		else
			ssa_log(SSA_LOG_DEFAULT, "Max. number of simulated"
				" clients is unlimited\n");
	}
#endif
#ifndef SIM_SUPPORT
	ssa_log(SSA_LOG_DEFAULT, "join timeout %d\n", join_timeout);
#endif
	ssa_log(SSA_LOG_DEFAULT, "addr preload %d\n", addr_preload);
	ssa_log(SSA_LOG_DEFAULT, "addr data file %s\n", addr_data_file);
}

static void *core_construct(osm_opensm_t *opensm)
{
#ifndef SIM_SUPPORT
	struct ssa_svc *svc;
	struct ssa_port *port;
	int d, p, j;
#endif
	int ret;
#if defined(SIM_SUPPORT) || defined (SIM_SUPPORT_SMDB)
	int i;
	char buf[PATH_MAX];
#else
	char msg[256] = {};

	if (ssa_open_lock_file(lock_file, msg, sizeof msg)) {
		openlog("ibssa", LOG_PERROR | LOG_PID, LOG_USER);
		syslog(LOG_INFO, "%s", msg);
		closelog();
		return NULL;
	}
#endif

	core_set_options();

	ssa_open_log(log_file);
	ssa_log(SSA_LOG_DEFAULT, "Scalable SA Core - OpenSM Plugin\n");
	core_log_options();

	/* TODO: remove when incremental changes is supported */
	if (smdb_deltas) {
		smdb_deltas = 0;
		ssa_log_warn(SSA_LOG_DEFAULT,
			     "SSA DB incremental changes are not currently "
			     "supported by SSA framework. Falling back to full "
			     "SSA DB updates.\n");
	}

	ssa_set_ssa_signal_handler();

	ret = ssa_init(&ssa, node_type, sizeof(struct ssa_device),
		       sizeof(struct ssa_port));
	if (ret) {
		ssa_close_log();
		ssa_close_lock_file();
		return NULL;
	}

	extract_data.opensm = opensm;
	extract_data.num_svcs = 0;
	extract_data.svcs = NULL;

#if defined(SIM_SUPPORT) || defined (SIM_SUPPORT_SMDB)
	snprintf(buf, PATH_MAX, "%s", smdb_dump_dir);
	for (i = strlen(buf); i > 0; i--) {
		if (buf[i] == '/') {
			buf[++i] = '\0';
			break;
		}
	}
	snprintf(buf + i, PATH_MAX - strlen(buf), "%s", smdb_lock_file);
	smdb_lock_fd = open(buf, O_RDWR | O_CREAT, 0640);
	if (smdb_lock_fd < 0) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "can't open smdb lock file: %s\n", buf);
		goto err1;
	}
#endif

	ssa_db = ssa_database_init();
	if (!ssa_db) {
		ssa_log(SSA_LOG_ALL, "SSA database init failed\n");
		goto err2;
	}

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sock_coreextract);
	if (ret) {
		ssa_log(SSA_LOG_ALL, "ERROR %d (%s): creating socketpair\n",
			errno, strerror(errno));
		goto err3;
	}

	pthread_mutex_init(&ssa_db_diff_lock, NULL);

#ifndef SIM_SUPPORT
	ret = ssa_open_devices(&ssa);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT, "ERROR opening devices\n");
		goto err4;
	}

	for (d = 0; d < ssa.dev_cnt; d++) {
		for (p = 1; p <= ssa_dev(&ssa, d)->port_cnt; p++) {
			port = ssa_dev_port(ssa_dev(&ssa, d), p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			extract_data.num_svcs++;
		}
	}

	extract_data.svcs = calloc(extract_data.num_svcs, sizeof(*extract_data.svcs));
	if (!extract_data.svcs) {
		ssa_log_err(SSA_LOG_DEFAULT, "unable to allocate extract_data.svcs\n");
		ssa_close_devices(&ssa);
		goto err4;
	}

	j = 0;
	for (d = 0; d < ssa.dev_cnt; d++) {
		for (p = 1; p <= ssa_dev(&ssa, d)->port_cnt; p++) {
			port = ssa_dev_port(ssa_dev(&ssa, d), p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			svc = ssa_start_svc(port, SSA_DB_PATH_DATA,
					    sizeof(struct ssa_core),
					    core_process_msg, core_init_svc,
					    core_destroy_svc);
			if (!svc) {
				ssa_log(SSA_LOG_DEFAULT, "ERROR starting service\n");
				goto err5;
			}
			extract_data.svcs[j] = svc;
			j++;
		}
	}

	ret = ssa_start_access(&ssa);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT, "ERROR starting access thread\n");
		goto err5;
	}
#endif

	ret = ssa_start_admin(&ssa);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT, "ERROR starting admin thread\n");
		goto err6;
	}

	ret = pthread_create(&extract_thread, NULL, core_extract_handler,
			     (void *) &extract_data);
	if (ret) {
		ssa_log(SSA_LOG_ALL,
			"ERROR %d (%s): error creating smdb extract thread\n",
			ret, strerror(ret));
		goto err7;
	}

#ifndef SIM_SUPPORT
	ret = pthread_create(&ctrl_thread, NULL, core_ctrl_handler, NULL);
	if (ret) {
		ssa_log(SSA_LOG_ALL,
			"ERROR %d (%s): error creating core ctrl thread\n",
			ret, strerror(ret));
		goto err8;
	}
#endif

	osm = opensm;
	return &ssa;

#ifndef SIM_SUPPORT
err8:
	core_send_msg(SSA_DB_EXIT);
	pthread_join(extract_thread, NULL);
#endif
err7:
	ssa_stop_admin();
err6:
#ifndef SIM_SUPPORT
	ssa_stop_access(&ssa);
err5:
	free(extract_data.svcs);
	ssa_close_devices(&ssa);
err4:
#endif
	close(sock_coreextract[0]);
	close(sock_coreextract[1]);
err3:
	ssa_database_delete(ssa_db);
err2:
#if defined(SIM_SUPPORT) || defined (SIM_SUPPORT_SMDB)
	if (smdb_lock_fd >= 0)
		close(smdb_lock_fd);
err1:
#endif
	ssa_close_log();
	ssa_cleanup(&ssa);
	ssa_close_lock_file();
	return NULL;
}

static void core_destroy(void *context)
{
#ifndef SIM_SUPPORT
	struct ssa_port *port;
	int d, p, s;

	ssa_log(SSA_LOG_DEFAULT, "shutting down control thread\n");
	ssa_ctrl_stop(&ssa);
	pthread_join(ctrl_thread, NULL);
#endif

	ssa_log(SSA_LOG_CTRL, "shutting down smdb extract thread\n");
	core_send_msg(SSA_DB_EXIT);
	pthread_join(extract_thread, NULL);
	free(extract_data.svcs);

#ifndef SIM_SUPPORT
	ssa_log(SSA_LOG_CTRL, "shutting down access thread\n");
	ssa_stop_access(&ssa);

	for (d = 0; d < ssa.dev_cnt; d++) {
		for (p = 1; p <= ssa_dev(&ssa, d)->port_cnt; p++) {
			port = ssa_dev_port(ssa_dev(&ssa, d), p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < port->svc_cnt; s++) {
				core_destroy_svc(port->svc[s]);
			}
		}
	}
#endif

	close(sock_coreextract[0]);
	close(sock_coreextract[1]);

#ifndef SIM_SUPPORT
	ssa_log(SSA_LOG_CTRL, "closing devices\n");
	ssa_close_devices(&ssa);
#endif

	if (ipdb)
		ssa_db_destroy(ipdb);

	pthread_mutex_lock(&ssa_db_diff_lock);
	ssa_db_diff_destroy(ssa_db_diff);
	pthread_mutex_unlock(&ssa_db_diff_lock);
	pthread_mutex_destroy(&ssa_db_diff_lock);

	ssa_log(SSA_LOG_CTRL, "destroying SMDB\n");
	ssa_database_delete(ssa_db);

#if defined(SIM_SUPPORT) || defined(SIM_SUPPORT_SMDB)
	if (smdb_lock_fd >= 0)
		close(smdb_lock_fd);
#endif

	ssa_log(SSA_LOG_VERBOSE, "that's all folks!\n");
	ssa_cleanup(&ssa);
	ssa_close_log();
	ssa_close_lock_file();
}

#if OSM_EVENT_PLUGIN_INTERFACE_VER != 2
#error OpenSM plugin interface version missmatch
#endif
osm_event_plugin_t osm_event_plugin = {
      OSM_VERSION,
      core_construct,
      core_destroy,
      core_report
};
