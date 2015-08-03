/*
 * Copyright (c) 2013-2015 Mellanox Technologies LTD. All rights reserved.
 * Copyright (c) 2013 Intel Corporation. All rights reserved.
 * Copyright (c) 2013 Lawrence Livermore National Securities. All rights reserved.
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <string.h>
#include <osd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <rdma/rsocket.h>
#include <netinet/tcp.h>
#include <infiniband/umad.h>
#include <infiniband/umad_str.h>
#include <infiniband/verbs.h>
#include <infiniband/ssa.h>
#include <infiniband/ib.h>
#include <infiniband/ssa_db.h>
#ifdef SIM_SUPPORT_FAKE_ACM
#include <infiniband/ssa_smdb.h>
#endif
#include <infiniband/ssa_path_record.h>
#include <infiniband/ssa_db_helper.h>
#include <dlist.h>
#include <search.h>
#include <ssa_admin.h>
#include <common.h>
#include <ssa_ctrl.h>
#include <inttypes.h>
#include <ssa_log.h>
#include <glib.h>

/* not sure why this isn't in verbs.h but is in libibverbs.map */
extern int ibv_read_sysfs_file(const char *dir, const char *file,
			       char *buf, size_t size);

#define DEFAULT_UMAD_TIMEOUT	1000 /* in milliseconds */
#define MAX_UMAD_TIMEOUT	120 * DEFAULT_UMAD_TIMEOUT /* in milliseconds */

#define FIRST_DATA_FD_SLOT		6
#define ACCESS_FDS_PER_SERVICE		2
#define ACCESS_FIRST_SERVICE_FD_SLOT	2
#define PRDB_LISTEN_FD_SLOT	FIRST_DATA_FD_SLOT - 1
#define SMDB_LISTEN_FD_SLOT	FIRST_DATA_FD_SLOT - 2

#define UPSTREAM_DATA_FD_SLOT		4
#define UPSTREAM_RECONNECT_TIMER_SLOT	5
#define UPSTREAM_JOIN_TIMER_SLOT	6
#define UPSTREAM_FD_SLOTS		7

#define ADMIN_FIRST_SERVICE_FD_SLOT 3
#define ADMIN_FDS_PER_SERVICE  2

#define SMDB_DUMP_PATH RDMA_CONF_DIR "/smdb_dump"
#define PRDB_DUMP_PATH RDMA_CONF_DIR "/prdb_dump"

#ifdef SIM_SUPPORT_FAKE_ACM
#define ACM_FAKE_RSOCKET_ID -1
#endif

#ifndef RCLOSE_THREAD_POOL_WORKERS_NUM
#define RCLOSE_THREAD_POOL_WORKERS_NUM 1
#endif

#ifndef MAX_ACCESS_POOL_WORKERS_NUM
#define MAX_ACCESS_POOL_WORKERS_NUM 0xffff
#endif

#ifndef MAX_REJOIN_TIMEOUT_FACTOR
#define MAX_REJOIN_TIMEOUT_FACTOR 120
#endif

struct ssa_db_update_record {
	DLIST_ENTRY		list_entry;
	struct ssa_db_update	db_upd;
};

struct ssa_db_update_queue {
	pthread_mutex_t		lock;
	pthread_mutex_t		cond_lock;
	pthread_cond_t		cond_var;
	DLIST_ENTRY		list;
};

struct ssa_access_context {
	struct ssa_db		*smdb;
	void			*context;
	GThreadPool		*g_th_pool;
	pthread_cond_t		th_pool_cond;
	pthread_mutex_t		th_pool_mtx;
	int			num_workers;
	atomic_t		num_tasks;
};

struct ssa_access_task {
	struct ssa_access_member *consumer;
	struct ssa_svc *svc;
};

static struct ssa_db *smdb;
static struct ssa_db *db_previous;
static int smdb_refcnt;
static uint64_t epoch;

__thread char log_data[128];
__thread char log_data1[128];
__thread int update_pending;
__thread int update_waiting;

static int sock_adminctrl[2];
static pthread_t *admin_thread;
static struct ssa_access_context access_context;
static int sock_accessctrl[2];
int sock_accessextract[2];
static pthread_t *access_thread;
#ifdef ACCESS
static struct ssa_db_update_queue update_queue;
static pthread_t *access_prdb_handler;
#endif
#if (RCLOSE_THREAD_POOL_WORKERS_NUM > 0)
static GThreadPool *thpool_rclose;
#endif /* RCLOSE_THREAD_POOL_WORKERS_NUM > 0 */

static int lock_fd = -1;

int smdb_dump = 0;
int err_smdb_dump = 0;
int prdb_dump = 0;
char smdb_dump_dir[128] = SMDB_DUMP_PATH;
char prdb_dump_dir[128] = PRDB_DUMP_PATH;
//static short server_port = 6125;
short smdb_port = 7475;
short prdb_port = 7476;
short admin_port = 7477;
int keepalive = 60;		/* seconds */
int reconnect_timeout = 10;	/* seconds */
int reconnect_max_count = 10;
int rejoin_timeout = 1;		/* seconds */

#ifdef ACCESS
#ifdef SIM_SUPPORT_FAKE_ACM
int fake_acm_num = 0;
#endif

struct ssa_access_member {
	union ibv_gid gid;		/* consumer GID */
	struct ssa_db *prdb_current;
	uint64_t smdb_epoch;
	int rsock;
	uint16_t lid;
};
#endif

struct ssa_sysinfo {
	int nprocs;
};

static struct ssa_sysinfo ssa_sysinfo;

/* Forward declarations */
static void ssa_close_ssa_conn(struct ssa_conn *conn);
static int ssa_downstream_svc_server(struct ssa_svc *svc, struct ssa_conn *conn);
static int ssa_upstream_initiate_conn(struct ssa_svc *svc, short dport);
static int ssa_upstream_svc_client(struct ssa_svc *svc);
static void ssa_upstream_query_db_resp(struct ssa_svc *svc, int status);
static void ssa_upstream_reconnect(struct ssa_svc *svc, struct pollfd *fds);
static int ssa_downstream_smdb_xfer_in_progress(struct ssa_svc *svc,
						struct pollfd *fds, int nfds);
static void ssa_send_db_update_ready(int fd);
static void ssa_downstream_smdb_update_ready(struct ssa_conn *conn,
					     struct ssa_svc *svc,
					     struct pollfd **fds);
static void ssa_downstream_start_listen(struct ssa_svc *svc, struct pollfd **fds);
static void ssa_close_port(struct ssa_port *port);
#ifdef ACCESS
static void ssa_db_update_init(struct ssa_svc *svc, struct ssa_db *db,
			       uint16_t remote_lid, union ibv_gid *remote_gid,
			       int rsock, int flags, uint64_t epoch,
			       struct ssa_db_update *p_db_upd);
static int ssa_push_db_update(struct ssa_db_update_queue *p_queue,
			      struct ssa_db_update *db_upd);
static void ssa_access_wait_for_tasks_completion();
static void ssa_access_process_task(struct ssa_access_task *task);
#endif
static void ssa_svc_schedule_join(struct ssa_svc *svc);
static void ssa_upstream_conn(struct ssa_svc *svc, struct ssa_conn *conn,
			      int gone);

static inline int get_max_rejoin_timeout()
{
	return MAX_REJOIN_TIMEOUT_FACTOR * rejoin_timeout;
}

static void g_rclose_callback(gint rsock, gpointer user_data)
{
	int ret;

	(void) user_data;
	ssa_log(SSA_LOG_DEFAULT, "closing rsock %d\n", GPOINTER_TO_INT(rsock));
	ret = rclose(GPOINTER_TO_INT(rsock));
	if (ret)
		ssa_log_err(SSA_LOG_CTRL, "rclose error %d on rsocket %d\n", ret, GPOINTER_TO_INT(rsock));
	else
		ssa_log(SSA_LOG_VERBOSE, "rsock %d now closed\n", GPOINTER_TO_INT(rsock));
}

static inline void ssa_close_rsocket(int rsock)
{
#if (RCLOSE_THREAD_POOL_WORKERS_NUM > 0)
	GError *g_error = NULL;

	g_thread_pool_push(thpool_rclose, GINT_TO_POINTER(rsock), &g_error);
	if (g_error != NULL) {
		ssa_log_err(SSA_LOG_CTRL,
			    "rsock %d thread pool push failed: %s\n",
			    rsock, g_error->message);
		g_error_free(g_error);
	}
#else
	g_rclose_callback(rsock, NULL);
#endif
}

static void ssa_get_sysinfo()
{
	ssa_sysinfo.nprocs = get_nprocs();
}

static void ssa_log_sysinfo()
{
	ssa_log(SSA_LOG_DEFAULT, "Number of cores %d\n", ssa_sysinfo.nprocs);
}

const char *ssa_method_str(uint8_t method)
{
	return umad_method_str(UMAD_CLASS_SUBN_ADM, method);
}

const char *ssa_attribute_str(be16_t attr_id)
{
	switch  (ntohs(attr_id)) {
	case SSA_ATTR_MEMBER_REC:
		return "MemberRecord";
	case SSA_ATTR_INFO_REC:
		return "InfoRecord";
	default:
		return umad_attribute_str(UMAD_CLASS_SUBN_ADM, attr_id);
	}
}

const char *ssa_mad_status_str(be16_t status)
{
	return umad_sa_mad_status_str(status);
}

int ssa_compare_gid(const void *gid1, const void *gid2)
{
	return memcmp(gid1, gid2, 16);
}

static be64_t ssa_svc_tid(struct ssa_svc *svc)
{
	return htonll((((uint64_t) svc->index) << 16) | svc->tid++);
}

static struct ssa_svc *ssa_svc_from_tid(struct ssa_port *port, be64_t tid)
{
	uint16_t index = (uint16_t) (ntohll(tid) >> 16);
	return (index < port->svc_cnt) ? port->svc[index] : NULL;
}

static struct ssa_svc *ssa_find_svc(struct ssa_port *port, uint64_t database_id)
{
	int i;
	for (i = 0; i < port->svc_cnt; i++) {
		if (port->svc[i] && port->svc[i]->database_id == database_id)
			return port->svc[i];
	}
	return NULL;
}

void ssa_init_mad_hdr(struct ssa_svc *svc, struct umad_hdr *hdr,
		      uint8_t method, uint16_t attr_id)
{
	hdr->base_version = UMAD_BASE_VERSION;
	hdr->mgmt_class = SSA_CLASS;
	hdr->class_version = SSA_CLASS_VERSION;
	hdr->method = method;
	hdr->tid = ssa_svc_tid(svc);
	hdr->attr_id = htons(attr_id);
}

static void sa_init_mad_hdr(struct ssa_svc *svc, struct umad_hdr *hdr,
			    uint8_t method, uint16_t attr_id)
{
	hdr->base_version = UMAD_BASE_VERSION;
	hdr->mgmt_class = UMAD_CLASS_SUBN_ADM;
	hdr->class_version = UMAD_SA_CLASS_VERSION;
	hdr->method = method;
	hdr->tid = ssa_svc_tid(svc);
	hdr->attr_id = htons(attr_id);
}

static void ssa_init_join(struct ssa_svc *svc, uint8_t bad_parent,
			  struct ssa_mad_packet *mad)
{
	struct ssa_member_record *rec;

	ssa_init_mad_hdr(svc, &mad->mad_hdr, UMAD_METHOD_SET, SSA_ATTR_MEMBER_REC);
	mad->ssa_key = 0;	/* TODO: set for real */

	rec = &mad->ssa_mad.member;
	memcpy(rec->port_gid, svc->port->gid.raw, 16);
	rec->database_id = htonll(svc->database_id);
	rec->node_guid = svc->port->dev->guid;
	rec->node_type = svc->port->dev->ssa->node_type;
	rec->bad_parent = bad_parent;
	if ((svc->port->dev->ssa->node_type & SSA_NODE_CORE) == 0 || bad_parent)
		memcpy(rec->parent_gid, svc->conn_dataup.remote_gid.raw, 16);
	else
		memset(rec->parent_gid, 0, 16);
}

static void sa_init_path_query(struct ssa_svc *svc, struct sa_path_record *mad,
			       union ibv_gid *dgid, union ibv_gid *sgid)
{
	struct ibv_path_record *path;

	sa_init_mad_hdr(svc, &mad->mad_hdr, UMAD_METHOD_GET,
			UMAD_SA_ATTR_PATH_REC);
	mad->comp_mask = htonll(((uint64_t)1) << 2 |	/* DGID */
				((uint64_t)1) << 3 |	/* SGID */
				((uint64_t)1) << 11 |	/* Reversible */
				((uint64_t)1) << 13);	/* P_Key */

	path = &mad->path;
	memcpy(path->dgid.raw, dgid, 16);
	memcpy(path->sgid.raw, sgid, 16);
	path->reversible_numpath = IBV_PATH_RECORD_REVERSIBLE;
	path->pkey = 0xFFFF;	/* default partition */
}

static int ssa_svc_join(struct ssa_svc *svc, uint8_t bad_parent)
{
	struct ssa_umad umad;
	int ret;
	uint16_t lid;

	ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, svc->port->gid.raw, sizeof svc->port->gid);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", svc->name, log_data);
	memset(&umad, 0, sizeof umad);
	if (svc->port->dev->ssa->node_type & SSA_NODE_CORE)
		lid = svc->port->lid;
	else
		lid = svc->port->sm_lid;

	umad_set_addr(&umad.umad, lid, 1, svc->port->sm_sl, UMAD_QKEY);
	ssa_init_join(svc, bad_parent, &umad.packet);
	svc->state = SSA_STATE_JOINING;

	ret = umad_send(svc->port->mad_portid, svc->port->mad_agentid,
			(void *) &umad, sizeof umad.packet, svc->umad_timeout, 0);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "failed to send join request\n");
		svc->state = SSA_STATE_IDLE;
	}
	return ret;
}

static void ssa_init_ssa_msg_hdr(struct ssa_msg_hdr *hdr, uint16_t op,
				 uint32_t len, uint16_t flags, uint32_t id,
				 uint32_t rdma_len, uint64_t rdma_addr)
{
	hdr->version = SSA_MSG_VERSION;
	hdr->class = SSA_MSG_CLASS_DB;
	hdr->op = htons(op);
	hdr->len = htonl(len);
	hdr->flags = htons(flags);
	hdr->status = 0;
	hdr->id = htonl(id);
	hdr->reserved = 0;
	hdr->rdma_len = htonl(rdma_len);
	hdr->rdma_addr = htonll(rdma_addr);
}

static int validate_ssa_msg_hdr(struct ssa_msg_hdr *hdr)
{
	if (hdr->version != SSA_MSG_VERSION)
		return 0;
	if (hdr->class != SSA_MSG_CLASS_DB)
		return 0;
	switch (ntohs(hdr->op)) {
	case SSA_MSG_DB_QUERY_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
	case SSA_MSG_DB_QUERY_DATA_DATASET:
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
	case SSA_MSG_DB_UPDATE:
		return 1;
	default:
		return 0;
	}
}

static int ssa_downstream_listen(struct ssa_svc *svc,
				 struct ssa_conn *conn_listen, short sport)
{
	struct sockaddr_ib src_addr;
	int ret, val;

	/* Only listening on rsocket when server (not consumer - ACM) */
	if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER)
		return -1;

	if (conn_listen->rsock >= 0)
		return conn_listen->rsock;

	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL, "%s\n", svc->port->name);

	conn_listen->rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (conn_listen->rsock < 0) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	val = 1;
	ret = rsetsockopt(conn_listen->rsock, SOL_SOCKET, SO_REUSEADDR,
			  &val, sizeof val);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt SO_REUSEADDR ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), conn_listen->rsock);
		goto err;
	}

	ret = rsetsockopt(conn_listen->rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt TCP_NODELAY ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), conn_listen->rsock);
		goto err;
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS &&
	    sport == prdb_port) {
		ret = rsetsockopt(conn_listen->rsock, SOL_RDMA, RDMA_IOMAPSIZE,
				  (void *) &val, sizeof(val));
		if (ret) {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"rsetsockopt rsock %d RDMA_IOMAPSIZE ERROR %d (%s)\n",
				conn_listen->rsock, errno, strerror(errno));
		}
	}
	ret = rfcntl(conn_listen->rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), conn_listen->rsock);
		goto err;
	}

	src_addr.sib_family = AF_IB;
	src_addr.sib_pkey = 0xFFFF;
	src_addr.sib_flowinfo = 0;
	src_addr.sib_sid = htonll(((uint64_t) RDMA_PS_TCP << 16) + sport);
	src_addr.sib_sid_mask = htonll(RDMA_IB_IP_PS_MASK | RDMA_IB_IP_PORT_MASK);
	src_addr.sib_scope_id = 0;
	memcpy(&src_addr.sib_addr, &svc->port->gid, 16);

	ret = rbind(conn_listen->rsock, (const struct sockaddr *) &src_addr,
		    sizeof(src_addr));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rbind ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), conn_listen->rsock);
		goto err;
	}
	ret = rlisten(conn_listen->rsock, 1);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rlisten ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), conn_listen->rsock);
		goto err;
	}
	conn_listen->state = SSA_CONN_LISTENING;
	ssa_log(SSA_LOG_VERBOSE, "rlistening on port %d\n", sport);

	return conn_listen->rsock;

err:
	ssa_close_ssa_conn(conn_listen);
	return -1;
}

int ssa_svc_query_path(struct ssa_svc *svc, union ibv_gid *dgid,
		       union ibv_gid *sgid)
{
	struct sa_umad umad;
	int ret;

	memset(&umad, 0, sizeof umad);
	umad_set_addr(&umad.umad, svc->port->sm_lid, 1, svc->port->sm_sl, UMAD_QKEY);
	sa_init_path_query(svc, &umad.sa_mad.path_rec, dgid, sgid);

	ret = umad_send(svc->port->mad_portid, svc->port->mad_agentid,
			(void *) &umad, sizeof umad.sa_mad.packet, svc->umad_timeout, 0);
	if (ret)
		ssa_log_err(SSA_LOG_CTRL, "failed to send path query to SA\n");
	return ret;
}

static void ssa_upstream_dev_event(struct ssa_svc *svc,
				   struct ssa_ctrl_msg_buf *msg,
				   struct pollfd *pfd)
{
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", svc->name,
		ibv_event_type_str(msg->data.event));
	switch (msg->data.event) {
	case IBV_EVENT_PORT_ERR:
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_CLIENT_REREGISTER:
		/* If directly connected to core, close upstream connection */
		if ((msg->data.event == IBV_EVENT_PORT_ERR ||
		     svc->primary_type & SSA_NODE_CORE) &&
		    svc->conn_dataup.rsock >= 0) {
			ssa_upstream_conn(svc, &svc->conn_dataup, 1);
			ssa_close_ssa_conn(&svc->conn_dataup);
			pfd->fd = -1;
			pfd->events = 0;
			pfd->revents = 0;
		}

		/*
		 * For the case of consumer already connected to access layer
		 *
		 * NOTE: If consumer was directly connected to core and
		 * it's rsocket connection was closed by previous
		 * condition above, it will fall through code below
		 * to reactivate.
		 */
		if (svc->conn_dataup.rsock >= 0 &&
		    svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER)
			break;

		/* fall through to reactivate */
		svc->state = SSA_STATE_IDLE;
	case IBV_EVENT_PORT_ACTIVE:
		if (svc->port->state == IBV_PORT_ACTIVE &&
		    svc->state == SSA_STATE_IDLE) {
			svc->umad_timeout = DEFAULT_UMAD_TIMEOUT;
			svc->rejoin_timeout = rejoin_timeout;
			ssa_svc_join(svc, 0);
		}
		break;
	default:
		break;
	}
}

void ssa_upstream_mad(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	struct ssa_umad *umad;
	struct ssa_mad_packet *mad;
	struct ssa_info_record *info_rec;

	umad = &msg->data.umad;
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	if (svc->state == SSA_STATE_IDLE) {
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
			"in idle state, discarding MAD\n");
		svc->umad_timeout = DEFAULT_UMAD_TIMEOUT;
		svc->rejoin_timeout = rejoin_timeout;
		return;
	}

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "method %s attr %s\n",
		ssa_method_str(umad->packet.mad_hdr.method),
		ssa_attribute_str(umad->packet.mad_hdr.attr_id));
	ssa_set_runtime_counter_time(COUNTER_ID_TIME_LAST_SSA_MAD_RCV);
	/* TODO: do we need to check umad->packet.mad_hdr.status too? */
	if (umad->umad.status) {
		ssa_log(SSA_LOG_DEFAULT, "send failed - status 0x%x (%s)\n",
			umad->umad.status, strerror(umad->umad.status));
		if (svc->state != SSA_STATE_JOINING)
			return;

		svc->umad_timeout = min(svc->umad_timeout << 1, MAX_UMAD_TIMEOUT);
		ssa_svc_schedule_join(svc);
		return;
	}

	svc->umad_timeout = DEFAULT_UMAD_TIMEOUT;
	if (svc->state == SSA_STATE_JOINING) {
		if (!umad->packet.mad_hdr.status) {
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "join successful\n");
			svc->state = SSA_STATE_ORPHAN;
		} else {
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				"join rejected with status 0x%x\n",
				ntohs(umad->packet.mad_hdr.status));
			ssa_svc_schedule_join(svc);
			return;
		}
	}

	if (ntohs(umad->packet.mad_hdr.attr_id) != SSA_ATTR_INFO_REC)
		return;

	umad->packet.mad_hdr.method = UMAD_METHOD_GET_RESP;
	umad_send(svc->port->mad_portid, svc->port->mad_agentid,
		  (void *) umad, sizeof umad->packet, 0, 0);

	switch (svc->state) {
	case SSA_STATE_ORPHAN:
		svc->state = SSA_STATE_HAVE_PARENT;
		svc->rejoin_timeout = rejoin_timeout;
	case SSA_STATE_HAVE_PARENT:
	case SSA_STATE_CONNECTING:
	case SSA_STATE_CONNECTED:
		mad = &umad->packet;
		info_rec = &mad->ssa_mad.info;

		if (memcmp(&svc->primary, &info_rec->path_data,
			   sizeof(svc->primary))) {
			if (svc->conn_dataup.rsock >= 0) {
				ssa_upstream_conn(svc, &svc->conn_dataup, 1);
				ssa_close_ssa_conn(&svc->conn_dataup);
				svc->state = SSA_STATE_HAVE_PARENT;
			}
			memcpy(&svc->primary, &info_rec->path_data,
			       sizeof(svc->primary));
			svc->primary_type = info_rec->node_type;
			break;
		}

		if (svc->conn_dataup.rsock >= 0 && svc->state != SSA_STATE_CONNECTING)
			svc->state = SSA_STATE_CONNECTED;

		break;
	default:
		break;
	}
}

static void ssa_init_ssa_conn(struct ssa_conn *conn, int conn_type,
			      int conn_dbtype)
{
	conn->rsock = -1;
	conn->type = conn_type;
	conn->dbtype = conn_dbtype;
	conn->state = SSA_CONN_IDLE;
	conn->phase = SSA_DB_IDLE;
	conn->rbuf = NULL;
	conn->rid = 0;
	conn->rindex = 0;
	conn->rhdr = NULL;
	conn->sbuf = NULL;
	conn->sid = 0;
	conn->sindex = 0;
	conn->sbuf2 = NULL;
	conn->rdma_write = 0;
	conn->ssa_db = NULL;
	conn->epoch = DB_EPOCH_INVALID;
	conn->prdb_epoch = DB_EPOCH_INVALID;
	conn->epoch_len = 0;
	conn->reconnect_count = 0;
}

static void ssa_close_ssa_conn(struct ssa_conn *conn)
{
	if (!conn)
		return;

	if (conn->type == SSA_CONN_TYPE_UPSTREAM &&
	    conn->epoch_len > 0) {
		int ret = riounmap(conn->rsock, (void *) &conn->prdb_epoch,
				   conn->epoch_len);
		if (ret) {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"riounmap rsock %d ret %d ERROR %d (%s)\n",
				conn->rsock, ret, errno, strerror(errno));
		}
	}

	ssa_close_rsocket(conn->rsock);

	conn->rsock = -1;
	conn->dbtype = SSA_CONN_NODB_TYPE;
	conn->state = SSA_CONN_IDLE;
	conn->phase = SSA_DB_IDLE;
	conn->epoch_len = 0;
	conn->rdma_write = 0;
}

static int ssa_upstream_send_query(int rsock, struct ssa_msg_hdr *msg,
				   uint16_t op, uint32_t id)
{
	uint32_t rdma_len;

	if (op == SSA_MSG_DB_PUBLISH_EPOCH_BUF)
		rdma_len = sizeof(((struct ssa_conn *) NULL)->prdb_epoch);
	else
		rdma_len = 0;
	ssa_init_ssa_msg_hdr(msg, op, sizeof(*msg), SSA_MSG_FLAG_END,
			     id, rdma_len, 0);
	return rsend(rsock, msg, sizeof(*msg), MSG_DONTWAIT);
}

#ifdef ACM
int ssa_get_svc_cnt(struct ssa_port *port)
{
	return port->svc_cnt;
}

struct ssa_svc *ssa_get_svc(struct ssa_port *port, int index)
{
	if (index >= port->svc_cnt)
		return NULL;
	return port->svc[index];
}

int ssa_upstream_query_db(struct ssa_svc *svc)
{
	int ret;
	struct ssa_db_query_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_QUERY;
	msg.hdr.len = sizeof(msg);
	msg.status = 0;
	ret = write(svc->sock_upmain[0], (char *) &msg, sizeof(msg));
	if (ret != sizeof(msg))
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof(msg));
	else {
		ret = read(svc->sock_upmain[0], (char *) &msg, sizeof(msg));
		if (ret != sizeof(msg))
			ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes read\n",
				    ret, sizeof(msg));
	}
	return msg.status;
}
#endif

static void ssa_upstream_query_db_resp(struct ssa_svc *svc, int status)
{
	int ret;
	struct ssa_db_query_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_QUERY;
	msg.hdr.len = sizeof(msg);
	msg.status = status;
	ret = write(svc->sock_upmain[1], (char *) &msg, sizeof(msg));
	if (ret != sizeof(msg))
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof(msg));
}

static void ssa_upstream_update_phase(struct ssa_conn *conn, uint16_t op)
{
	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
		conn->phase = SSA_DB_DEFS;
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF:
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		conn->phase = SSA_DB_TBL_DEFS;
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		conn->phase = SSA_DB_FIELD_DEFS;
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		conn->phase = SSA_DB_DATA;
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		if (conn->phase != SSA_DB_IDLE) {
			ssa_log(SSA_LOG_CTRL,
				"SSA_MSG_DB_PUBLISH_EPOCH_BUF in state %d not SSA_DB_IDLE\n",
				conn->phase);
			conn->phase = SSA_DB_IDLE;
		}
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "unknown op %u\n", op);
		break;
	}
}

static short ssa_upstream_query(struct ssa_svc *svc, uint16_t op, short events)
{
	uint32_t id;
	int ret;

	svc->conn_dataup.sbuf = malloc(sizeof(struct ssa_msg_hdr));
	if (svc->conn_dataup.sbuf) {
		svc->conn_dataup.ssize = sizeof(struct ssa_msg_hdr);
		svc->conn_dataup.soffset = 0;
		id = svc->tid++;

		ret = ssa_upstream_send_query(svc->conn_dataup.rsock,
					      svc->conn_dataup.sbuf, op, id);
		if (ret >= 0) {
			ssa_upstream_update_phase(&svc->conn_dataup, op);
			svc->conn_dataup.soffset += ret;
			svc->conn_dataup.sid = id;
			if (svc->conn_dataup.soffset == svc->conn_dataup.ssize) {
				free(svc->conn_dataup.sbuf);
				svc->conn_dataup.sbuf = NULL;
				return POLLIN;
			} else {
				return POLLOUT | POLLIN;
			}
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return POLLOUT | POLLIN;
			}
			ssa_log_err(SSA_LOG_CTRL,
				    "ssa_upstream_send_query for op %u failed "
				    "%d (%s) on rsock %d\n",
				    op, errno, strerror(errno),
				    svc->conn_dataup.rsock);
		}
	} else
		ssa_log_err(SSA_LOG_CTRL,
			    "failed to allocate ssa_msg_hdr for "
			    "ssa_upstream_send_query for op %u on rsock %d\n",
			    op, svc->conn_dataup.rsock);
	return events;
}

static short ssa_riowrite(struct ssa_conn *conn, short events)
{
	int ret;
	short revents = events;

	ssa_log(SSA_LOG_VERBOSE, "epoch 0x%" PRIx64 " remote LID %u\n",
		ntohll(conn->prdb_epoch), conn->remote_lid);

	conn->sbuf = (void *) &conn->prdb_epoch;
	conn->ssize = sizeof(conn->prdb_epoch);
	conn->soffset = 0;
	conn->sbuf2 = NULL;
	conn->rdma_write = 1;
	ret = riowrite(conn->rsock, conn->sbuf, conn->ssize, 0, MSG_DONTWAIT);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return POLLOUT | POLLIN;
		ssa_log(SSA_LOG_DEFAULT, "epoch riowrite ERROR %d (%s)\n",
			errno, strerror(errno));
	} else if (ret < sizeof(conn->prdb_epoch)) {
		revents = POLLOUT | POLLIN;
		ssa_log(SSA_LOG_DEFAULT,
			"epoch riowrite %d out of %d bytes written\n",
			ret, sizeof(conn->prdb_epoch));
	} else {
		conn->rdma_write = 0;
		conn->sbuf = NULL;
	}

	return revents;
}

static short ssa_riowrite_continue(struct ssa_conn *conn, short events)
{
	int ret;

	ret = riowrite(conn->rsock, conn->sbuf + conn->soffset,
		       conn->ssize - conn->soffset, conn->soffset, MSG_DONTWAIT);
	if (ret >= 0) {
		conn->soffset += ret;
		if (conn->soffset == conn->ssize) {
			conn->rdma_write = 0;
			conn->sbuf = NULL;
			return POLLIN;
		} else
			return POLLOUT | POLLIN;
	} else {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return POLLOUT | POLLIN;
		ssa_log_err(SSA_LOG_CTRL,
			    "riowrite continuation failed: %d (%s) on rsock %d\n",
			    errno, strerror(errno), conn->rsock);
		return events;
	}
}

static short ssa_rsend_continue(struct ssa_conn *conn, short events)
{
	int ret;

	ret = rsend(conn->rsock, conn->sbuf + conn->soffset,
		    conn->ssize - conn->soffset, MSG_DONTWAIT);
	if (ret >= 0) {
		conn->soffset += ret;
		if (conn->soffset == conn->ssize) {
			if (conn->sbuf != conn->sbuf2) {
				free(conn->sbuf);
				if (!conn->sbuf2) {
					conn->sbuf = NULL;
					return POLLIN;
				} else {
					conn->sbuf = conn->sbuf2;
					conn->ssize = conn->ssize2;
					conn->soffset = 0;
					ret = rsend(conn->rsock, conn->sbuf,
						    conn->ssize, MSG_DONTWAIT);
					if (ret >= 0) {
						conn->soffset += ret;
						if (conn->soffset == conn->ssize) {
							conn->sbuf2 = NULL;
							return POLLIN;
						} else
							return POLLOUT | POLLIN;
					} else {
						if (errno == EAGAIN ||
						    errno == EWOULDBLOCK)
							return POLLOUT | POLLIN;
						ssa_log_err(SSA_LOG_CTRL,
							    "rsend continuation failed: %d (%s) on rsock %d\n",
							    errno, strerror(errno), conn->rsock);
					}
				}
			} else {
				conn->sbuf2 = NULL;
				return POLLIN;
			}
		} else {
			return POLLOUT | POLLIN;
		}
	} else {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return POLLOUT | POLLIN;
		ssa_log_err(SSA_LOG_CTRL,
			    "rsend continuation failed: %d (%s) on rsock %d\n",
			    errno, strerror(errno), conn->rsock);
	}

	return events;
}

static int ssa_upstream_handle_query_defs(struct ssa_conn *conn,
					  struct ssa_msg_hdr *hdr)
{
	int ret, size;

	if (conn->phase == SSA_DB_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_DEF/TBL_DEF ids 0x%x 0x%x "
				"don't match on rsock %d\n",
				conn->sid, ntohl(hdr->id), conn->rsock);
		} else {
			conn->rhdr = hdr;
			if (conn->rindex)
				size = sizeof(struct db_dataset);
			else
				size = sizeof(struct db_def);
			if (ntohl(hdr->len) != sizeof(*hdr) + size)
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_MSG_DB_QUERY_DEF/TBL_DEF response "
					"length %d is not the expected length "
					"%d on rsock %d\n",
					ntohl(hdr->len), sizeof(*hdr) + size,
					conn->rsock);
			else {
				if (conn->rindex)
					conn->rbuf = &conn->ssa_db->db_table_def;
				else
					conn->rbuf = &conn->ssa_db->db_def;
				conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
				conn->roffset = 0;
				ret = rrecv(conn->rsock, conn->rbuf,
					    conn->rsize, MSG_DONTWAIT);
				if (ret > 0) {
					conn->roffset += ret;
				} else if (ret == 0) {
					ssa_log_err(SSA_LOG_DEFAULT,
						    "rrecv 0 out of %d bytes on rsock %d\n",
						    conn->rsize, conn->rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", conn->rbuf, conn->rsize, conn->roffset, conn->state, conn->phase);
					return ECONNRESET;
				} else {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						return 0;
					ssa_log_err(SSA_LOG_CTRL,
						    "rrecv failed: %d (%s) on rsock %d\n",
						    errno, strerror(errno), conn->rsock);
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_DEF phase %d not SSA_DB_DEFS "
			"on rsock %d\n",
			conn->phase, conn->rsock);

	return 0;
}

static int ssa_upstream_handle_query_tbl_defs(struct ssa_conn *conn,
					      struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_TBL_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_TBL_DEF ids 0x%x 0x%x "
				"don't match on rsock %d\n",
				conn->sid, ntohl(hdr->id), conn->rsock);
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available "
						"for rsock %d\n",
						conn->rsock);
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					} else if (ret == 0) {
						ssa_log_err(SSA_LOG_DEFAULT,
							    "rrecv 0 out of %d bytes on rsock %d\n",
							    conn->rsize, conn->rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", conn->rbuf, conn->rsize, conn->roffset, conn->state, conn->phase);
						return ECONNRESET;
					} else {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							return 0;
						ssa_log_err(SSA_LOG_CTRL,
							    "rrecv failed: %d (%s) on rsock %d\n",
							    errno, strerror(errno), conn->rsock);
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_TBL_DEF phase %d not "
			"SSA_DB_TBL_DEFS on rsock %d\n",
			conn->phase, conn->rsock);

	return 0;
}

static int ssa_upstream_handle_query_field_defs(struct ssa_conn *conn,
						struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_FIELD_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_FIELD_DEF ids 0x%x 0x%x "
				"don't match on rsock %d\n",
				conn->sid, ntohl(hdr->id), conn->rsock);
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available "
						"for rsock %d\n",
						conn->rsock);
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					} else if (ret == 0) {
						ssa_log_err(SSA_LOG_DEFAULT,
							    "rrecv 0 out of %d bytes on rsock %d\n",
							    conn->rsize, conn->rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", conn->rbuf, conn->rsize, conn->roffset, conn->state, conn->phase);
						return ECONNRESET;
					} else {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							return 0;
						ssa_log_err(SSA_LOG_CTRL,
							    "rrecv failed: %d (%s) on rsock %d\n",
							    errno, strerror(errno), conn->rsock);
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_FIELD_DEF phase %d not "
			"SSA_DB_FIELD_DEFS on rsock %d\n",
			conn->phase, conn->rsock);

	return 0;
}

static int ssa_upstream_handle_query_data(struct ssa_conn *conn,
					  struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_DATA) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_DATA_DATASET ids 0x%x 0x%x "
				"don't match on rsock %d\n",
				conn->sid, ntohl(hdr->id), conn->rsock);
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available "
						"for rsock %d\n",
						conn->rsock);
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					} else if (ret == 0) {
						ssa_log_err(SSA_LOG_DEFAULT,
							    "rrecv 0 out of %d bytes on rsock %d\n",
							    conn->rsize, conn->rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", conn->rbuf, conn->rsize, conn->roffset, conn->state, conn->phase);
						return ECONNRESET;
					} else {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							return 0;
						ssa_log_err(SSA_LOG_CTRL,
							    "rrecv failed: %d (%s) on rsock %d\n",
							    errno, strerror(errno), conn->rsock);
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_DATA_DATASET phase %d not "
			"SSA_DB_DATA on rsock %d\n",
			conn->phase, conn->rsock);

	return 0;
}

static void ssa_upstream_handle_db_update(struct ssa_conn *conn,
					  struct ssa_msg_hdr *hdr)
{
	conn->roffset = 0;
	free(hdr);		/* same as svc->conn_dataup.rbuf */
	conn->rbuf = NULL;
}

static int ssa_upstream_send_db_update_prepare(struct ssa_svc *svc)
{
	int count = 0, ret;
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);

	msg.hdr.type = SSA_DB_UPDATE_PREPARE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = NULL;
	msg.db_upd.svc = NULL;
	msg.db_upd.flags = 0;
	msg.db_upd.epoch = DB_EPOCH_INVALID;

	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		ret = write(svc->sock_accessup[0], (char *) &msg, sizeof(msg));
		if (ret != sizeof(msg))
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to access\n",
				    ret, sizeof(msg));
		count++;
	}

	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		ret = write(svc->sock_updown[0], (char *) &msg, sizeof(msg));
		if (ret != sizeof(msg))
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to downstream\n",
				    ret, sizeof(msg));
		count++;
	}

	return count;
}

void ssa_db_update_change_counters(uint64_t epoch)
{
	static short first = 1;

	if (first) {
		ssa_set_runtime_counter_time(COUNTER_ID_DB_FIRST_UPDATE_TIME);
		first = 0;
	}

	ssa_set_runtime_counter_time(COUNTER_ID_DB_LAST_UPDATE_TIME);
	ssa_inc_runtime_counter(COUNTER_ID_DB_UPDATES_NUM);
	ssa_set_runtime_counter(COUNTER_ID_DB_EPOCH, epoch);
}

static void ssa_upstream_send_db_update(struct ssa_svc *svc, struct ssa_db *db,
					int flags, union ibv_gid *gid,
					uint64_t epoch)
{
	int ret;
	struct ssa_db_update_msg msg;

	msg.hdr.type = SSA_DB_UPDATE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = db;
	msg.db_upd.svc = NULL;
	msg.db_upd.flags = flags;
	if (gid)
		memcpy(&msg.db_upd.remote_gid, gid, 16);
	else
		memset(&msg.db_upd.remote_gid, 0, 16);
	msg.db_upd.remote_lid = 0;
	msg.db_upd.epoch = epoch;
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		ret = write(svc->sock_accessup[0], (char *) &msg, sizeof(msg));
		if (ret != sizeof(msg))
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to access\n",
				    ret, sizeof(msg));
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		ret = write(svc->sock_updown[0], (char *) &msg, sizeof(msg));
		if (ret != sizeof(msg))
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to downstream\n",
				    ret, sizeof(msg));
	}
	if (svc->process_msg)
		svc->process_msg(svc, (struct ssa_ctrl_msg_buf *) &msg);
	ssa_db_update_change_counters(epoch);
}

static short ssa_upstream_update_conn(struct ssa_svc *svc, short events)
{
	uint64_t data_tbl_cnt, epoch;
	short revents = events;

	switch (svc->conn_dataup.phase) {
	case SSA_DB_IDLE:
		revents = ssa_upstream_query(svc, SSA_MSG_DB_QUERY_DEF, events);
		svc->conn_dataup.rindex = 0;
		break;
	case SSA_DB_DEFS:
		if (svc->conn_dataup.rindex)
			svc->conn_dataup.phase = SSA_DB_TBL_DEFS;
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     svc->conn_dataup.rindex == 0 ?
					     SSA_MSG_DB_QUERY_TBL_DEF :
					     SSA_MSG_DB_QUERY_TBL_DEF_DATASET,
					     events);
		if (svc->conn_dataup.phase == SSA_DB_DEFS)
			svc->conn_dataup.rindex++;
		else
			svc->conn_dataup.rindex = 0;
		break;
	case SSA_DB_TBL_DEFS:
		svc->conn_dataup.phase = SSA_DB_FIELD_DEFS;
		svc->conn_dataup.roffset = 0;
		svc->conn_dataup.ssa_db->p_def_tbl = svc->conn_dataup.rbuf;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					     events);
		break;
	case SSA_DB_FIELD_DEFS:
		if (svc->conn_dataup.rbuf == svc->conn_dataup.rhdr &&
		    ntohs(((struct ssa_msg_hdr *)svc->conn_dataup.rhdr)->flags) & SSA_MSG_FLAG_END) {
			svc->conn_dataup.phase = SSA_DB_DATA;
			if (svc->conn_dataup.rindex != ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db))
				ssa_log_err(SSA_LOG_DEFAULT,
					    "SSA_DB_FIELD_DEFS protocol error - rindex %d num tables %d mismatch\n",
					    svc->conn_dataup.rindex,
					    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db));
		} else {
			if (!svc->conn_dataup.ssa_db->p_db_field_tables) {
				svc->conn_dataup.ssa_db->p_db_field_tables = svc->conn_dataup.rbuf;
				data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
				svc->conn_dataup.ssa_db->pp_field_tables = calloc(1, data_tbl_cnt * sizeof(*svc->conn_dataup.ssa_db->pp_field_tables));
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_FIELD_DEFS ssa_db allocated pp_field_tables %p num tables %d rsock %d\n", svc->conn_dataup.ssa_db->pp_field_tables, data_tbl_cnt, svc->conn_dataup.rsock);
				svc->conn_dataup.rindex = 0;
			} else {
				if (svc->conn_dataup.rindex >=
				    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db))
					ssa_log_err(SSA_LOG_DEFAULT,
						    "SSA_DB_FIELD_DEFS protocol error - ignoring rindex %d num tables %d\n",
						    svc->conn_dataup.rindex,
						    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db));
				else if (svc->conn_dataup.ssa_db->pp_field_tables) {                                       
					if (svc->conn_dataup.ssa_db->pp_field_tables[svc->conn_dataup.rindex])
						ssa_log_err(SSA_LOG_DEFAULT,
							    "SSA_DB_FIELD_DEFS pp_field_tables rindex %d %p not NULL as expected\n",
							    svc->conn_dataup.rindex,
							    svc->conn_dataup.ssa_db->pp_field_tables[svc->conn_dataup.rindex]);
					if (svc->conn_dataup.rbuf != svc->conn_dataup.rhdr)
						svc->conn_dataup.ssa_db->pp_field_tables[svc->conn_dataup.rindex] = svc->conn_dataup.rbuf;
				} else ssa_log_err(SSA_LOG_DEFAULT,
						   "SSA_DB_FIELD_DEFS no pp_field_tables for rindex %d\n",
						   svc->conn_dataup.rindex);
{
void *rbuf;
int rsize;

if (svc->conn_dataup.rbuf != svc->conn_dataup.rhdr) {
rbuf = svc->conn_dataup.rbuf;
rsize = svc->conn_dataup.rsize;
} else {
rbuf = NULL;
rsize = 0;
}
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_FIELD_DEFS index %d %p len %d rsock %d\n", svc->conn_dataup.rindex, rbuf, rsize, svc->conn_dataup.rsock);
}
				svc->conn_dataup.rindex++;
			}
		}
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     svc->conn_dataup.phase == SSA_DB_DATA ?
					     SSA_MSG_DB_QUERY_DATA_DATASET :
					     SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					     events);
		break;
	case SSA_DB_DATA:
		if (svc->conn_dataup.rbuf == svc->conn_dataup.rhdr &&
		    ntohs(((struct ssa_msg_hdr *)svc->conn_dataup.rhdr)->flags) & SSA_MSG_FLAG_END) {
			svc->conn_dataup.phase = SSA_DB_IDLE;
			if (svc->conn_dataup.rindex != ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db))
				ssa_log_err(SSA_LOG_DEFAULT,
					    "SSA_DB_DATA protocol error - rindex %d num tables %d mismatch\n",
					    svc->conn_dataup.rindex,
					    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db));
		} else {
			if (!svc->conn_dataup.ssa_db->p_db_tables) {
				svc->conn_dataup.ssa_db->p_db_tables = svc->conn_dataup.rbuf;
				data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
				svc->conn_dataup.ssa_db->pp_tables = calloc(1, data_tbl_cnt * sizeof(*svc->conn_dataup.ssa_db->pp_tables));
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_DATA ssa_db allocated pp_tables %p num tables %d rsock %d\n", svc->conn_dataup.ssa_db->pp_tables, data_tbl_cnt, svc->conn_dataup.rsock);
				svc->conn_dataup.rindex = 0;
			} else {
				if (svc->conn_dataup.rindex >=
				    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db))
					ssa_log_err(SSA_LOG_DEFAULT,
						    "SSA_DB_DATA protocol error - ignoring rindex %d num tables %d\n",
						    svc->conn_dataup.rindex,
						    ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db));
				else if (svc->conn_dataup.ssa_db->pp_tables) {
					if (svc->conn_dataup.ssa_db->pp_tables[svc->conn_dataup.rindex])
						ssa_log_err(SSA_LOG_DEFAULT,
							    "SSA_DB_DATA pp_tables rindex %d %p not NULL as expected\n",
							    svc->conn_dataup.rindex,
							    svc->conn_dataup.ssa_db->pp_tables[svc->conn_dataup.rindex]);
					if (svc->conn_dataup.rbuf != svc->conn_dataup.rhdr)
						svc->conn_dataup.ssa_db->pp_tables[svc->conn_dataup.rindex] = svc->conn_dataup.rbuf;
				} else ssa_log_err(SSA_LOG_DEFAULT,
						   "SSA_DB_DATA no pp_tables for rindex %d\n",
						   svc->conn_dataup.rindex);
{
void *rbuf;
int rsize;

if (svc->conn_dataup.rbuf != svc->conn_dataup.rhdr) {
rbuf = svc->conn_dataup.rbuf;
rsize = svc->conn_dataup.rsize;
epoch = ssa_db_get_epoch(svc->conn_dataup.ssa_db, svc->conn_dataup.rindex);
} else {
rbuf = NULL;
rsize = 0;
epoch = DB_EPOCH_INVALID;
}
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_DATA index %d epoch 0x%" PRIx64 " %p len %d rsock %d\n", svc->conn_dataup.rindex, epoch, rbuf, rsize, svc->conn_dataup.rsock);
}
				svc->conn_dataup.rindex++;
			}
		}
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		if (svc->conn_dataup.phase == SSA_DB_DATA) {
			revents = ssa_upstream_query(svc,
						     SSA_MSG_DB_QUERY_DATA_DATASET,
						     events);
		} else {
			svc->conn_dataup.ssa_db->data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
			epoch = ssa_db_get_epoch(svc->conn_dataup.ssa_db,
						 DB_DEF_TBL_ID);
ssa_log(SSA_LOG_DEFAULT, "ssa_db %p epoch 0x%" PRIx64 " complete with num tables %d rsock %d\n", svc->conn_dataup.ssa_db, epoch, svc->conn_dataup.ssa_db->data_tbl_cnt, svc->conn_dataup.rsock);
			ssa_upstream_send_db_update(svc, svc->conn_dataup.ssa_db,
						    0, NULL, epoch);
if (db_previous)
ssa_log(SSA_LOG_DEFAULT, "destroying previous ssa_db %p\n", db_previous);
			ssa_db_destroy(db_previous);
			db_previous = svc->conn_dataup.ssa_db;
ssa_log(SSA_LOG_DEFAULT, "previous ssa_db now %p\n", db_previous);
		}
		break;
	default:
		ssa_log(SSA_LOG_DEFAULT, "unknown phase %d on rsock %d\n",
			svc->conn_dataup.phase, svc->conn_dataup.rsock);
		break;
	}
	return revents;
}

static short ssa_upstream_handle_op(struct ssa_svc *svc,
				    struct ssa_msg_hdr *hdr, short events,
				    int *count, struct pollfd *fds)
{
	uint16_t op;
	short revents = events;

	op = ntohs(hdr->op);
	if (op != SSA_MSG_DB_UPDATE) {
		if (!(ntohs(hdr->flags) & SSA_MSG_FLAG_RESP))
			ssa_log(SSA_LOG_DEFAULT,
				"Ignoring SSA_MSG_FLAG_RESP not set in op %u "
				"response in phase %d rsock %d\n",
				op, svc->conn_dataup.phase, svc->conn_dataup.rsock);
	} else {
		if ((ntohs(hdr->flags) & SSA_MSG_FLAG_RESP))
			ssa_log(SSA_LOG_DEFAULT,
				"Ignoring SSA_MSG_FLAG_RESP set in op %u "
				"(SSA_MSG_DB_UPDATE) in phase %d rsock %d\n",
				op, svc->conn_dataup.phase, svc->conn_dataup.rsock);
	}

	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF:
		if (ssa_upstream_handle_query_defs(&svc->conn_dataup, hdr)) {
			ssa_upstream_reconnect(svc, fds);
			return 0;
		}
		if (svc->conn_dataup.phase == SSA_DB_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_DEFS received id 0x%x "
					"expected id 0x%x on rsock %d\n",
					ntohl(hdr->id), svc->conn_dataup.sid,
					svc->conn_dataup.rsock);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_DEFS on rsock %d\n",
				svc->conn_dataup.phase, svc->conn_dataup.rsock);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		if (ssa_upstream_handle_query_tbl_defs(&svc->conn_dataup, hdr)) {
			ssa_upstream_reconnect(svc, fds);
			return 0;
		}
		if (svc->conn_dataup.phase == SSA_DB_TBL_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_TBL_DEFS received id 0x%x "
					"expected id 0x%x on rsock %d\n",
					ntohl(hdr->id), svc->conn_dataup.sid,
					svc->conn_dataup.rsock);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_TBL_DEFS on rsock %d\n",
				svc->conn_dataup.phase, svc->conn_dataup.rsock);
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		if (ssa_upstream_handle_query_field_defs(&svc->conn_dataup, hdr)) {
			ssa_upstream_reconnect(svc, fds);
			return 0;
		}
		if (svc->conn_dataup.phase == SSA_DB_FIELD_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_FIELD_DEFS received id 0x%x "
					"expected id 0x%x on rsock %d\n",
					ntohl(hdr->id), svc->conn_dataup.sid,
					svc->conn_dataup.rsock);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_FIELD_DEFS on rsock %d\n",
				svc->conn_dataup.phase, svc->conn_dataup.rsock);
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		if (ssa_upstream_handle_query_data(&svc->conn_dataup, hdr)) {
			ssa_upstream_reconnect(svc, fds);
			return 0;
		}
		if (svc->conn_dataup.phase == SSA_DB_DATA) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* dupli
cate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_DATA received id 0x%x "
					"expected id 0x%x on rsock %d\n",
					ntohl(hdr->id), svc->conn_dataup.sid,
					svc->conn_dataup.rsock);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_DATA on rsock %d\n",
				svc->conn_dataup.phase, svc->conn_dataup.rsock);
		break;
	case SSA_MSG_DB_UPDATE:
ssa_log(SSA_LOG_DEFAULT, "SSA_MSG_DB_UPDATE received from upstream when ssa_db %p epoch 0x%" PRIx64 " phase %d rsock %d\n", svc->conn_dataup.ssa_db, ntohll(hdr->rdma_addr), svc->conn_dataup.phase, svc->conn_dataup.rsock);
		ssa_upstream_handle_db_update(&svc->conn_dataup, hdr);
		/* Ignore DB update notification message if phase is not IDLE */
		if (svc->conn_dataup.phase == SSA_DB_IDLE) {
			if (svc->conn_dataup.ssa_db) {
				if (*count == 0) {
					*count = ssa_upstream_send_db_update_prepare(svc);
ssa_log(SSA_LOG_DEFAULT, "%d DB update prepare msgs sent\n", *count);
					if (*count == 0)
						revents = ssa_upstream_update_conn(svc, events);
				}
			} else
				revents = ssa_upstream_update_conn(svc, events);
		}
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		ssa_log_warn(SSA_LOG_CTRL,
			     "ignoring SSA_MSG_DB_PUBLISH_EPOCH_BUF on rsock %d\n",
			     svc->conn_dataup.rsock);
		free(hdr);		/* same as svc->conn_dataup.rbuf */
		svc->conn_dataup.rbuf = NULL;
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "ignoring unknown op %u on rsock %d\n",
			     op, svc->conn_dataup.rsock);
		free(hdr);		/* same as svc->conn_dataup.rbuf */
		svc->conn_dataup.rbuf = NULL;
		break;
	}
	return revents;
}

static short ssa_upstream_rrecv(struct ssa_svc *svc, short events, int *count,
				struct pollfd *fds)
{
	struct ssa_msg_hdr *hdr;
	int ret;
	short revents = events;

	if (svc->conn_dataup.rsize - svc->conn_dataup.roffset == 0) {
		ssa_log_err(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d\n",
			    svc->conn_dataup.rbuf, svc->conn_dataup.rsize,
			    svc->conn_dataup.roffset);
		return 0; 
	}

	ret = rrecv(svc->conn_dataup.rsock,
		    svc->conn_dataup.rbuf + svc->conn_dataup.roffset,
		    svc->conn_dataup.rsize - svc->conn_dataup.roffset,
		    MSG_DONTWAIT);
	if (ret > 0) {
		svc->conn_dataup.roffset += ret;
		if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
			if (!svc->conn_dataup.rhdr) {
				hdr = svc->conn_dataup.rbuf;
				if (validate_ssa_msg_hdr(hdr))
					revents = ssa_upstream_handle_op(svc, hdr, events, count, fds);
				else {
					ssa_log_warn(SSA_LOG_CTRL,
						     "validate_ssa_msg_hdr failed: version %d class %d op %u id 0x%x on rsock %d\n",
						     hdr->version, hdr->class,
						     ntohs(hdr->op),
						     ntohl(hdr->id),
						     svc->conn_dataup.rsock);
					free(hdr);	/* same as svc->conn_dataup.rbuf */
					svc->conn_dataup.rbuf = NULL;
				}
			} else
				revents = ssa_upstream_update_conn(svc, events);
		}
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return revents;
		ssa_log_err(SSA_LOG_CTRL, "rrecv failed: %d (%s) on rsock %d\n",
			    errno, strerror(errno), svc->conn_dataup.rsock);
	} else if (ret == 0) {
		ssa_log_err(SSA_LOG_DEFAULT, "rrecv 0 out of %d bytes on rsock %d\n",
			    svc->conn_dataup.rsize - svc->conn_dataup.roffset,
			    svc->conn_dataup.rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", svc->conn_dataup.rbuf, svc->conn_dataup.rsize, svc->conn_dataup.roffset, svc->conn_dataup.state, svc->conn_dataup.phase);
		ssa_upstream_reconnect(svc, fds);
		return 0;
	}

	return revents;
}

static void ssa_svc_schedule_join(struct ssa_svc *svc)
{
	int ret;
	struct itimerspec join_timer;
	long random_shift;

	if (svc->join_timer_fd < 0) {
		ssa_log_err(SSA_LOG_CTRL, "join timer disarmed\n");
		return;
	}

	if (svc->rejoin_timeout < 0)
		return;

	if (svc->port->state != IBV_PORT_ACTIVE)
		/*
		 * Join request will be sent at IBV_EVENT_PORT_ACTIVE
		 * event processing.
		 */
		return;

	random_shift = 1000 + 999999000 * (rand() / RAND_MAX);

	join_timer.it_value.tv_sec = svc->rejoin_timeout;
	join_timer.it_value.tv_nsec = random_shift;
	join_timer.it_interval.tv_sec = 0;
	join_timer.it_interval.tv_nsec = 0;

	ret = timerfd_settime(svc->join_timer_fd, 0, &join_timer, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL,
			    "timerfd_settime %d %d (%s)\n",
			    ret, errno, strerror(errno));
		close(svc->join_timer_fd);
		svc->join_timer_fd = -1;
		return;
	} else {
		ssa_log(SSA_LOG_DEFAULT,
			"join to distribution tree after %d sec\n",
			svc->rejoin_timeout);
	}

	svc->rejoin_timeout = min(svc->rejoin_timeout << 1, get_max_rejoin_timeout());
	svc->state = SSA_STATE_IDLE;
}

static void ssa_upstream_reconnect(struct ssa_svc *svc, struct pollfd *fds)
{
	int ret, first_timeout;
	struct itimerspec reconnect_timer;

	/*
	 * Set state to IDLE regardless to current reconnection count.
	 * After unsuccessful reconnection, the state could be SSA_CONN_CONNECTING.
	 */
	svc->conn_dataup.state = SSA_CONN_IDLE;
	svc->state = SSA_STATE_HAVE_PARENT;

	if (svc->conn_dataup.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_dataup);

	fds[UPSTREAM_DATA_FD_SLOT].fd = -1;
	fds[UPSTREAM_DATA_FD_SLOT].events = 0;
	fds[UPSTREAM_DATA_FD_SLOT].revents = 0;

	/* Upstream is in a middle of reconnection */
	if (svc->conn_dataup.reconnect_count > 0)
		return;

	if (reconnect_max_count >= 0 && reconnect_timeout >= 0 &&
	    svc->port->state == IBV_PORT_ACTIVE) {
		reconnect_timer.it_value.tv_sec = reconnect_timeout;
		first_timeout = rand() % (2 * reconnect_timeout);
		reconnect_timer.it_value.tv_sec = first_timeout;

		if (first_timeout > 0)
			reconnect_timer.it_value.tv_nsec = 0;
		else
			/* Generate reconnect event immediately */
			reconnect_timer.it_value.tv_nsec = 100;

		if (reconnect_max_count > 1)
			reconnect_timer.it_interval.tv_sec = reconnect_timeout;
		else
			reconnect_timer.it_interval.tv_sec = 0;

		reconnect_timer.it_interval.tv_nsec = 0;

		ret = timerfd_settime(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd, 0,
				      &reconnect_timer, NULL);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "timerfd_settime %d %d (%s)\n",
				    ret, errno, strerror(errno));
			close(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd);
			fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd = -1;
			fds[UPSTREAM_RECONNECT_TIMER_SLOT].events = 0;
			fds[UPSTREAM_RECONNECT_TIMER_SLOT].revents = 0;
			return;
		} else {
			ssa_log(SSA_LOG_DEFAULT,
				"reconnect to upstream node. first reconnection"
				" after %d sec., next after %d sec.\n",
				reconnect_timer.it_value.tv_sec,
				reconnect_timer.it_interval.tv_sec);
		}
		fds[UPSTREAM_RECONNECT_TIMER_SLOT].events = POLLIN;
		fds[UPSTREAM_RECONNECT_TIMER_SLOT].revents = 0;
	} else {
		if (svc->port->state == IBV_PORT_ACTIVE)
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "upstream connection lost. reconnection disabled\n");
		else
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "upstream connection lost. IB port is not active\n");
	}
}

static void ssa_upstream_stop_reconnection(struct ssa_svc *svc, struct pollfd *fds)
{
	int ret;
	struct itimerspec reconnect_timer;

	reconnect_timer.it_value.tv_sec = 0;
	reconnect_timer.it_value.tv_nsec = 0;
	reconnect_timer.it_interval.tv_sec = 0;
	reconnect_timer.it_interval.tv_nsec = 0;

	ret = timerfd_settime(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd, 0,
			      &reconnect_timer, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "timerfd_settime %d %d (%s)\n",
			    ret, errno, strerror(errno));
		close(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd);
		fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd = -1;
	}

	fds[UPSTREAM_RECONNECT_TIMER_SLOT].events = 0;
	fds[UPSTREAM_RECONNECT_TIMER_SLOT].revents = 0;

	svc->conn_dataup.reconnect_count = 0;
}

static void *ssa_upstream_handler(void *context)
{
	struct ssa_svc *svc = context, *conn_svc;
	struct ssa_ctrl_msg_buf msg;
	struct pollfd fds[UPSTREAM_FD_SLOTS];
	int ret, timeout = -1;		/* infinite */
	int outstanding_count = 0;
	short port;

	SET_THREAD_NAME(svc->upstream, "UP_%s", svc->name);

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	ret = write(svc->sock_upctrl[1], (char *) &msg, sizeof msg.hdr);
	if (ret != sizeof msg.hdr)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg.hdr);

	fds[0].fd = svc->sock_upctrl[1];
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	fds[1].fd = svc->sock_accessup[0];
	fds[1].events = POLLIN;
	fds[1].revents = 0;
	fds[2].fd = svc->sock_upmain[1];
	if (svc->sock_upmain[1] >= 0)
		fds[2].events = POLLIN;
	else
		fds[2].events = 0;
	fds[2].revents = 0;
	fds[3].fd = svc->sock_updown[0];
	if (svc->sock_updown[0] >= 0)
		fds[3].events = POLLIN;
	else
		fds[3].events = 0;
	fds[3].revents = 0;
	fds[UPSTREAM_DATA_FD_SLOT].fd = -1; /* placeholder for upstream connection */
	fds[UPSTREAM_DATA_FD_SLOT].events = 0;
	fds[UPSTREAM_DATA_FD_SLOT].revents = 0;

	fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd < 0) {
		ssa_log_err(SSA_LOG_CTRL, "timerfd_create %d %d (%s)\n",
			    ret, errno, strerror(errno));
		return NULL;
	}

	fds[UPSTREAM_JOIN_TIMER_SLOT].fd = svc->join_timer_fd;
	fds[UPSTREAM_JOIN_TIMER_SLOT].events = POLLIN;
	fds[UPSTREAM_JOIN_TIMER_SLOT].revents = 0;

	for (;;) {
		ret = rpoll(&fds[0], UPSTREAM_FD_SLOTS, timeout);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}
		if (fds[0].revents) {
			fds[0].revents = 0;
			ret = read(svc->sock_upctrl[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from ctrl\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_upctrl[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from ctrl\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
			if (svc->process_msg && svc->process_msg(svc, &msg))
				goto check_fd1;

			switch (msg.hdr.type) {
			case SSA_CTRL_MAD:
				ssa_upstream_mad(svc, &msg);
				break;
			case SSA_CTRL_DEV_EVENT:
				ssa_upstream_dev_event(svc, &msg,
						       &fds[UPSTREAM_DATA_FD_SLOT]);
				break;
			case SSA_CONN_REQ:
				conn_svc = msg.data.svc;
				if (conn_svc->conn_dataup.state != SSA_CONN_IDLE)
					ssa_log(SSA_LOG_DEFAULT,
						"upstream connection state not idle\n");
				if (conn_svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER) {
					conn_svc->conn_dataup.dbtype = SSA_CONN_PRDB_TYPE;
					port = prdb_port;
				} else {
					conn_svc->conn_dataup.dbtype = SSA_CONN_SMDB_TYPE;
					port = smdb_port;
				}
				fds[UPSTREAM_DATA_FD_SLOT].fd = ssa_upstream_initiate_conn(conn_svc, port);
				/* Change when more than 1 data connection supported !!! */
				if (fds[UPSTREAM_DATA_FD_SLOT].fd >= 0) {
					if (conn_svc->conn_dataup.state != SSA_CONN_CONNECTED) {
						/* 1 msec timeout used for polling for connection completed */
						timeout = 1;
						fds[UPSTREAM_DATA_FD_SLOT].events = POLLOUT;
					} else {
						ssa_set_runtime_counter_time(COUNTER_ID_TIME_LAST_UPSTR_CONN);
						if (port == prdb_port)
							fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_query(svc, SSA_MSG_DB_PUBLISH_EPOCH_BUF, fds[UPSTREAM_DATA_FD_SLOT].events);
						else {
							conn_svc->conn_dataup.ssa_db = calloc(1, sizeof(*conn_svc->conn_dataup.ssa_db));
							if (!conn_svc->conn_dataup.ssa_db)
								ssa_log_err(SSA_LOG_DEFAULT,
									    "could not allocate ssa_db struct for SMDB on rsock %d\n",
									    fds[UPSTREAM_DATA_FD_SLOT].fd);
						}
					}
				} else {
					ssa_upstream_reconnect(svc, fds);
				}
				break;
			case SSA_CONN_DONE:
				break;
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

check_fd1:
		if (fds[1].revents) {
			fds[1].revents = 0;
			ret = read(svc->sock_accessup[0], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from access\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_accessup[0],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from access\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE_READY:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_READY from access with outstanding count %d\n", outstanding_count);
				if (outstanding_count > 0) {
					if (--outstanding_count == 0) {
						db_previous = svc->conn_dataup.ssa_db;
						svc->conn_dataup.ssa_db = calloc(1, sizeof(*svc->conn_dataup.ssa_db));
						if (svc->conn_dataup.ssa_db)
							fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_update_conn(svc, fds[UPSTREAM_DATA_FD_SLOT].events);
						else
							ssa_log_err(SSA_LOG_DEFAULT,
								    "could not allocate ssa_db struct for new SMDB\n");
					}
				}
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from access\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[2].revents) {
			fds[2].revents = 0;
			ret = read(svc->sock_upmain[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from main\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_upmain[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from main\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_QUERY:
				if (svc->conn_dataup.rsock >= 0) {
					if (svc->conn_dataup.epoch !=
					    ntohll(svc->conn_dataup.prdb_epoch)) {
						if (svc->conn_dataup.ssa_db)
							db_previous = svc->conn_dataup.ssa_db;
						svc->conn_dataup.ssa_db = calloc(1, sizeof(*svc->conn_dataup.ssa_db));
ssa_log(SSA_LOG_DEFAULT, "PRDB ssa_db new %p old %p\n", svc->conn_dataup.ssa_db, db_previous);
						if (!svc->conn_dataup.ssa_db)
							ssa_log_err(SSA_LOG_DEFAULT,
								    "could not allocate ssa_db struct for new PRDB\n");
						/* Should response (and epoch update) be after DB is pulled successfully ??? */
						ssa_upstream_query_db_resp(svc, SSA_DB_QUERY_EPOCH_CHANGED);
						svc->conn_dataup.epoch = ntohll(svc->conn_dataup.prdb_epoch);
ssa_log(SSA_LOG_DEFAULT, "updating upstream connection rsock %d in phase %d due to updated epoch 0x%" PRIx64 "\n", svc->conn_dataup.rsock, svc->conn_dataup.phase, svc->conn_dataup.epoch);
						/* Check connection state ??? */
						fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_update_conn(svc, fds[UPSTREAM_DATA_FD_SLOT].events);
					} else {
						/* No epoch change */
						ssa_upstream_query_db_resp(svc, -SSA_DB_QUERY_EPOCH_NOT_CHANGED);
					}
				} else {
					/* No upstream connection */
					ssa_upstream_query_db_resp(svc, -SSA_DB_QUERY_NO_UPSTREAM_CONN);
				}
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from main\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[3].revents) {
			fds[3].revents = 0;
			ret = read(svc->sock_updown[0], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from downstream\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_updown[0],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from downstream\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE_READY:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_READY from downstream with outstanding count %d\n", outstanding_count);
				if (outstanding_count > 0) {
					if (--outstanding_count == 0) {
						db_previous = svc->conn_dataup.ssa_db;
						svc->conn_dataup.ssa_db = calloc(1, sizeof(*svc->conn_dataup.ssa_db));
						if (svc->conn_dataup.ssa_db)
							fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_update_conn(svc, fds[UPSTREAM_DATA_FD_SLOT].events);
						else
							ssa_log_err(SSA_LOG_DEFAULT,
								    "could not allocate ssa_db struct for new SMDB\n");
					}
				}
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from downstream\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[UPSTREAM_RECONNECT_TIMER_SLOT].revents & POLLIN) {
			ssize_t s;
			uint64_t exp;

			s = read(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd, &exp, sizeof(uint64_t));
			if (s != sizeof(uint64_t)) {
				ssa_log_err(SSA_LOG_DEFAULT,
					    "%" PRId64 " bytes read\n", s);
			} else if (svc->port->state != IBV_PORT_ACTIVE) {
				ssa_upstream_stop_reconnection(svc, fds);
				ssa_log(SSA_LOG_DEFAULT,
					"port is not active. stopped reconnection\n");
			} else {
				ssa_log(SSA_LOG_DEFAULT,
					"reconnect timer expiration %" PRIu64
					", conn state %d, conn phase %d, svc state %d\n",
					exp, svc->conn_dataup.state,
					svc->conn_dataup.phase, svc->state);

				switch (svc->conn_dataup.state) {
				case SSA_CONN_CONNECTED:
					ssa_upstream_stop_reconnection(svc, fds);
					ssa_log(SSA_LOG_DEFAULT,
						"upstream connection established. stopped reconnection\n");
					break;
				case SSA_CONN_CONNECTING:
					ssa_log(SSA_LOG_DEFAULT,
						"upstream connection is being established\n");
					break;
				case SSA_CONN_IDLE:
					if (svc->state == SSA_STATE_HAVE_PARENT) {
						if (++svc->conn_dataup.reconnect_count <= reconnect_max_count) {
							ssa_log(SSA_LOG_DEFAULT,
								"reconnection attempt %d of %d\n",
								svc->conn_dataup.reconnect_count,
								reconnect_max_count);
							ssa_ctrl_conn(svc->port->dev->ssa, svc);
						} else {
							if (svc->conn_dataup.rsock >= 0)
								ssa_close_ssa_conn(&svc->conn_dataup);
							fds[UPSTREAM_DATA_FD_SLOT].fd = -1;
							fds[UPSTREAM_DATA_FD_SLOT].events = 0;
							fds[UPSTREAM_DATA_FD_SLOT].revents = 0;
							svc->state = SSA_STATE_IDLE;
							ssa_upstream_stop_reconnection(svc, fds);
							ssa_log(SSA_LOG_DEFAULT,
								"reconnection failed. start rejoin indicating bad parent\n");
							ssa_svc_join(svc, 1);
						}
					}
					break;
				default:
					break;
				}
			}
			fds[UPSTREAM_RECONNECT_TIMER_SLOT].revents = 0;
		}

		if (fds[UPSTREAM_JOIN_TIMER_SLOT].revents & POLLIN) {
			ssize_t s;
			uint64_t exp;

			if (fds[UPSTREAM_JOIN_TIMER_SLOT].fd != svc->join_timer_fd ||
			    svc->join_timer_fd < 0) {
				/* ssa_svc_schedule_join could close > join_timer_fd */
				ssa_log_err(SSA_LOG_CTRL,
					    "rejoin timerfd closed\n");
				fds[UPSTREAM_JOIN_TIMER_SLOT].fd = -1;
				fds[UPSTREAM_JOIN_TIMER_SLOT].events = 0;
			} else {
				s = read(fds[UPSTREAM_JOIN_TIMER_SLOT].fd, &exp, sizeof(uint64_t));
				if (s != sizeof(uint64_t)) {
					ssa_log_err(SSA_LOG_DEFAULT,
						    "%" PRId64 " bytes read\n", s);
				} else if (svc->port->state != IBV_PORT_ACTIVE) {
					ssa_log(SSA_LOG_DEFAULT,
						"port is not active. stopped rejoin\n");
				} else if (svc->state != SSA_STATE_IDLE) {
					ssa_log(SSA_LOG_DEFAULT,
						"svc state is not IDLE. stopped rejoin\n");
				} else {
					ssa_log(SSA_LOG_DEFAULT,
						"join timer expiration %" PRIu64 "\n",
						exp);
					ssa_svc_join(svc, 0);
				}
			}

			fds[UPSTREAM_JOIN_TIMER_SLOT].revents = 0;
		}

		/* Only 1 upstream data connection currently */
		if (fds[UPSTREAM_DATA_FD_SLOT].revents) {
			if (fds[UPSTREAM_DATA_FD_SLOT].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				char event_str[128] = {};

				ssa_format_event(event_str, sizeof(event_str),
						 fds[UPSTREAM_DATA_FD_SLOT].revents);
				ssa_log_err(SSA_LOG_DEFAULT,
					    "error event 0x%x (%s) on rsock %d\n",
					    fds[UPSTREAM_DATA_FD_SLOT].revents,
					    event_str,
					    fds[UPSTREAM_DATA_FD_SLOT].fd);
				ssa_upstream_reconnect(svc, fds);
			}
			if (fds[UPSTREAM_DATA_FD_SLOT].revents & POLLOUT) {
				/* Check connection state for fd */
				if (svc->conn_dataup.state != SSA_CONN_CONNECTED) {
					ret = ssa_upstream_svc_client(svc);
					if (ret && (errno != EINPROGRESS)) {
						ssa_log(SSA_LOG_DEFAULT,
							"ssa_upstream_svc_client error on rsock %d\n",
							fds[UPSTREAM_DATA_FD_SLOT].fd);
						ssa_upstream_reconnect(svc, fds);
					} else if (ret == 0) {
						timeout = -1;	/* infinite */
						if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER)
							fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_query(svc, SSA_MSG_DB_PUBLISH_EPOCH_BUF, fds[UPSTREAM_DATA_FD_SLOT].events);
						else {
							svc->conn_dataup.ssa_db = calloc(1, sizeof(*svc->conn_dataup.ssa_db));
							if (!svc->conn_dataup.ssa_db)
								ssa_log_err(SSA_LOG_DEFAULT,
									    "could not allocate SMDB ssa_db struct for rsock %d\n",
									    fds[UPSTREAM_DATA_FD_SLOT].fd);
						}
					}
				} else {
					fds[UPSTREAM_DATA_FD_SLOT].events = ssa_rsend_continue(&svc->conn_dataup, fds[UPSTREAM_DATA_FD_SLOT].events);
				}
			}
			if (fds[UPSTREAM_DATA_FD_SLOT].revents & POLLIN) {
				if (!svc->conn_dataup.rbuf) {
					svc->conn_dataup.rbuf = malloc(sizeof(struct ssa_msg_hdr));
					if (svc->conn_dataup.rbuf) {
						svc->conn_dataup.rsize = sizeof(struct ssa_msg_hdr);
						svc->conn_dataup.roffset = 0;
						svc->conn_dataup.rhdr = NULL;
					} else
						ssa_log_err(SSA_LOG_CTRL,
							    "failed to allocate ssa_msg_hdr for rrecv on rsock %d\n",
							    fds[UPSTREAM_DATA_FD_SLOT].fd);
				}
else ssa_log(SSA_LOG_DEFAULT, "reusing rbuf %p rsize %d roffset %d rhdr %p\n", svc->conn_dataup.rbuf, svc->conn_dataup.rsize, svc->conn_dataup.roffset, svc->conn_dataup.rhdr);
				if (svc->conn_dataup.rbuf)
					fds[UPSTREAM_DATA_FD_SLOT].events = ssa_upstream_rrecv(svc, fds[UPSTREAM_DATA_FD_SLOT].events, &outstanding_count, fds);
			}
			if (fds[UPSTREAM_DATA_FD_SLOT].revents & ~(POLLOUT | POLLIN)) {
				char event_str[128] = {};

				ssa_format_event(event_str, sizeof(event_str),
						 fds[UPSTREAM_DATA_FD_SLOT].revents & ~(POLLOUT | POLLIN));
				ssa_log(SSA_LOG_DEFAULT,
					"unexpected event 0x%x (%s) on upstream rsock %d\n",
					fds[UPSTREAM_DATA_FD_SLOT].revents & ~(POLLOUT | POLLIN),
					event_str,
					fds[UPSTREAM_DATA_FD_SLOT].fd);
			}

			fds[UPSTREAM_DATA_FD_SLOT].revents = 0;
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif
		}

	}
out:
	if (fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd >= 0)
		close(fds[UPSTREAM_RECONNECT_TIMER_SLOT].fd);

	return NULL;
}

static void ssa_conn_msg_init(const struct ssa_conn *conn, struct ssa_conn_done_msg *msg)
{
	msg->data.rsock = conn->rsock;
	msg->data.type = conn->type;
	msg->data.dbtype = conn->dbtype;
	memcpy(&msg->data.remote_gid.raw, &conn->remote_gid.raw, sizeof(msg->data.remote_gid.raw));
	msg->data.remote_lid = conn->remote_lid;
}

static void ssa_downstream_conn(struct ssa_svc *svc, struct ssa_conn *conn,
				int gone)
{
	int ret;
	struct ssa_conn_done_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	if (gone)
		msg.hdr.type = SSA_CONN_GONE;
	else
		msg.hdr.type = SSA_CONN_DONE;
	msg.hdr.len = sizeof(msg);
	ssa_conn_msg_init(conn, &msg);
	if (conn->dbtype == SSA_CONN_PRDB_TYPE) {
		ret = write(svc->sock_accessdown[0], (char *) &msg, sizeof msg);
		if (ret != sizeof msg)
			ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
				    ret, sizeof msg);
	}
	ret = write(svc->sock_admindown[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
}

static short ssa_downstream_send(struct ssa_conn *conn, uint16_t op,
				 uint16_t flags, uint32_t id, uint64_t rdma_addr,
				 void *buf, size_t len, short events)
{
	int ret;

	conn->sbuf = malloc(sizeof(struct ssa_msg_hdr));
	conn->sbuf2 = buf;
	if (conn->sbuf) {
		conn->ssize = sizeof(struct ssa_msg_hdr);
		conn->ssize2 = len;
		conn->soffset = 0;
		ssa_init_ssa_msg_hdr(conn->sbuf, op, conn->ssize + len,
				     flags, id, 0, rdma_addr);
		ret = rsend(conn->rsock, conn->sbuf, conn->ssize, MSG_DONTWAIT);
		if (ret >= 0) {
			conn->soffset += ret;
			if (conn->soffset == conn->ssize) {
				free(conn->sbuf);
				if (!conn->sbuf2 || conn->ssize2 == 0) {
					conn->sbuf = NULL;
					return POLLIN;
				}
				conn->sbuf = conn->sbuf2;
				conn->ssize = conn->ssize2;
				conn->soffset = 0;
				ret = rsend(conn->rsock, conn->sbuf,
					    conn->ssize, MSG_DONTWAIT);
				if (ret >= 0) {
					conn->soffset += ret;
					if (conn->soffset == conn->ssize) {
						conn->sbuf = NULL;
						return POLLIN;
					} else
						return POLLOUT | POLLIN;
				} else {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						return POLLOUT | POLLIN;
					ssa_log_err(SSA_LOG_CTRL,
						    "rsend failed: %d (%s) for "
						    "op %u flags 0x%x on rsock %d\n",
						    errno, strerror(errno),
						    op, flags, conn->rsock);
				}
			} else
				return POLLOUT | POLLIN;
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return POLLOUT | POLLIN;
			ssa_log_err(SSA_LOG_CTRL,
				    "rsend failed: %d (%s) for op %u "
				    "flags 0x%x on rsock %d\n",
				    errno, strerror(errno), op, flags, conn->rsock);
		}
	} else
		ssa_log_err(SSA_LOG_CTRL,
			    "failed to allocate ssa_msg_hdr for op %u "
			    "flags 0x%x on rsock %d\n",
			    op, flags, conn->rsock);
	return events;
}

static struct ssa_db *ssa_downstream_db(struct ssa_conn *conn)
{
	/* Use SSA DB if available; otherwise use preloaded DB */
	if (conn->ssa_db)
		return conn->ssa_db;
	if (conn->dbtype == SSA_CONN_SMDB_TYPE)
		return smdb;
	return NULL;
}

static short ssa_downstream_handle_query_defs(struct ssa_conn *conn,
					      struct ssa_msg_hdr *hdr,
					      short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (!ssadb) {
ssa_log(SSA_LOG_DEFAULT, "No ssa_db or prdb as yet\n");
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn, SSA_MSG_DB_QUERY_DEF,
					      SSA_MSG_FLAG_END | SSA_MSG_FLAG_RESP,
					      conn->rid, 0, NULL, 0, events);
		return revents;
	}

	if (conn->phase == SSA_DB_IDLE) {
		if (conn->dbtype == SSA_CONN_SMDB_TYPE) {
			smdb_refcnt++;
ssa_log(SSA_LOG_DEFAULT, "SMDB %p ref count was just incremented to %u\n", ssadb, smdb_refcnt);
		}
		conn->phase = SSA_DB_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_DEF,
					      SSA_MSG_FLAG_RESP,
					      conn->rid, 0,
					      &ssadb->db_def,
					      sizeof(ssadb->db_def),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_IDLE "
			     "for SSA_MSG_DB_QUERY_DEF\n",
			     conn->rsock, conn->phase);

	return revents;
}

static short ssa_downstream_handle_query_tbl_def(struct ssa_conn *conn,
						 struct ssa_msg_hdr *hdr,
						 short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_DEFS) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_TBL_DEF,
					      SSA_MSG_FLAG_RESP,
					      conn->rid, 0,
					      &ssadb->db_table_def,
					      sizeof(ssadb->db_table_def),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS "
			     "for SSA_MSG_DB_QUERY_TBL_DEF\n",
			     conn->rsock, conn->phase);

	return revents;
}

static short ssa_downstream_handle_query_tbl_defs(struct ssa_conn *conn,
						  struct ssa_msg_hdr *hdr,
						  short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_DEFS) {
		conn->phase = SSA_DB_TBL_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_TBL_DEF_DATASET,
					      SSA_MSG_FLAG_RESP,
					      conn->rid, 0,
					      ssadb->p_def_tbl,
					      ntohll(ssadb->db_table_def.set_size),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS "
			     "for SSA_MSG_DB_QUERY_TBL_DEF_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_query_field_defs(struct ssa_conn *conn,
						    struct ssa_msg_hdr *hdr,
						    short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_TBL_DEFS) {
		conn->phase = SSA_DB_FIELD_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					      SSA_MSG_FLAG_RESP,
					      conn->rid, 0,
					      ssadb->p_db_field_tables,
					      ssadb->data_tbl_cnt * sizeof(*ssadb->p_db_field_tables),
					      events);
		conn->sindex = 0;
	} else if (conn->phase == SSA_DB_FIELD_DEFS) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		if (conn->sindex < ssadb->data_tbl_cnt) {
ssa_log(SSA_LOG_DEFAULT, "pp_field_tables index %d %p len %d rsock %d\n", conn->sindex, ssadb->pp_field_tables[conn->sindex], ntohll(ssadb->p_db_field_tables[conn->sindex].set_size), conn->rsock);
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
						      SSA_MSG_FLAG_RESP,
						      conn->rid, 0,
						      ssadb->pp_field_tables[conn->sindex],
						      ntohll(ssadb->p_db_field_tables[conn->sindex].set_size),
						      events);

			conn->sindex++;
		} else {
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
						      SSA_MSG_FLAG_END | SSA_MSG_FLAG_RESP,
						      conn->rid, 0, NULL, 0, events);
		}
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_TBL_DEFS "
			     "for SSA_MSG_DB_QUERY_FIELD_DEF_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_query_data(struct ssa_conn *conn,
					      struct ssa_msg_hdr *hdr,
					      short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_FIELD_DEFS) {
		conn->phase = SSA_DB_DATA;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_DATA_DATASET,
					      SSA_MSG_FLAG_RESP,
					      conn->rid, 0,
					      ssadb->p_db_tables,
					      ssadb->data_tbl_cnt * sizeof(*ssadb->p_db_tables),
					      events);
		conn->sindex = 0;
	} else if (conn->phase == SSA_DB_DATA) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		if (conn->sindex < ssadb->data_tbl_cnt) {
ssa_log(SSA_LOG_DEFAULT, "pp_tables index %d epoch 0x%" PRIx64 " %p len %d rsock %d\n", conn->sindex, ntohll(ssadb->p_db_tables[conn->sindex].epoch), ssadb->pp_tables[conn->sindex], ntohll(ssadb->p_db_tables[conn->sindex].set_size), conn->rsock);
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_DATA_DATASET,
						      SSA_MSG_FLAG_RESP,
						      conn->rid, 0,
						      ssadb->pp_tables[conn->sindex],
						      ntohll(ssadb->p_db_tables[conn->sindex].set_size),
						      events);
			conn->sindex++;
		} else {
			if (conn->dbtype == SSA_CONN_SMDB_TYPE) {
				smdb_refcnt--;
ssa_log(SSA_LOG_DEFAULT, "SMDB %p ref count was just decremented to %u\n", ssadb, smdb_refcnt);
			}
			conn->phase = SSA_DB_IDLE;
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_DATA_DATASET,
						      SSA_MSG_FLAG_END | SSA_MSG_FLAG_RESP,
						      conn->rid, 0, NULL, 0, events);
		}
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS "
			     "for SSA_MSG_DB_QUERY_DATA_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_epoch_publish(struct ssa_conn *conn,
						 struct ssa_svc *svc,
						 struct ssa_msg_hdr *hdr,
						 short events)
{
	short revents = events;

	ssa_log_func(SSA_LOG_CTRL);
	conn->epoch_len = ntohl(hdr->rdma_len);
	if (conn->epoch_len != sizeof(conn->prdb_epoch))
		ssa_log(SSA_LOG_DEFAULT,
			"published epoch buffer length is %d but should be %d\n",
			conn->epoch_len, sizeof(conn->prdb_epoch));
	conn->roffset = 0;
	free(conn->rbuf);
	conn->rhdr = NULL;
	conn->rbuf = NULL;
	if (conn->ssa_db && conn->epoch_len == sizeof(conn->prdb_epoch)) {
		epoch = ssa_db_get_epoch(conn->ssa_db, DB_DEF_TBL_ID);
		if (epoch != DB_EPOCH_INVALID) {
			/* RDMA write current epoch for the connnection/DB so (limited) ACM restart will work */
			revents = ssa_riowrite(conn, events);
		}
	}

	ssa_downstream_conn(svc, conn, 0);
	return revents;
}

static short ssa_downstream_handle_op(struct ssa_conn *conn,
				      struct ssa_msg_hdr *hdr, short events,
				      struct ssa_svc *svc, struct pollfd **fds)
{
	uint16_t op;
	short revents = events;

	op = ntohs(hdr->op);
	if (ntohs(hdr->flags) & SSA_MSG_FLAG_RESP)
		ssa_log(SSA_LOG_DEFAULT,
			"Ignoring SSA_MSG_FLAG_RESP set in op %u request "
			"in phase %d on rsock %d\n",
			op, conn->phase, conn->rsock);
	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
		revents = ssa_downstream_handle_query_defs(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF:
		revents = ssa_downstream_handle_query_tbl_def(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		revents = ssa_downstream_handle_query_tbl_defs(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		revents = ssa_downstream_handle_query_field_defs(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		revents = ssa_downstream_handle_query_data(conn, hdr, events);
		if (conn->phase == SSA_DB_IDLE &&
		    conn->dbtype == SSA_CONN_SMDB_TYPE) {
			if (update_pending) {
ssa_log(SSA_LOG_DEFAULT, "rsock %d in SSA_DB_IDLE phase with update pending\n", conn->rsock);
				ssa_downstream_smdb_update_ready(conn, svc, fds);
			}
		}
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		revents = ssa_downstream_handle_epoch_publish(conn, svc, hdr, events);
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "unknown op %u on rsock %d\n",
			     op, conn->rsock);
		break;
	}
	return revents;
}

static short ssa_downstream_rrecv(struct ssa_conn *conn, short events,
				  struct ssa_svc *svc, struct pollfd **fds)
{
	struct ssa_msg_hdr *hdr;
	int ret;
	short revents = events;

	ret = rrecv(conn->rsock, conn->rbuf + conn->roffset,
		    conn->rsize - conn->roffset, MSG_DONTWAIT);
	if (ret > 0) {
		conn->roffset += ret;
		if (conn->roffset == conn->rsize) {
			hdr = conn->rbuf;
			if (validate_ssa_msg_hdr(hdr)) {
				revents = ssa_downstream_handle_op(conn, hdr, events, svc, fds);
			} else
				ssa_log_warn(SSA_LOG_CTRL,
					     "validate_ssa_msg_hdr failed: "
					     "version %d class %d op %u "
					     "id 0x%x on rsock %d\n",
					     hdr->version, hdr->class,
					     ntohs(hdr->op), ntohl(hdr->id),
					     conn->rsock);
		}
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return revents;
	      ssa_log_err(SSA_LOG_CTRL, "rrecv failed: %d (%s) on rsock %d\n",
			  errno, strerror(errno), conn->rsock);
	} else if (ret == 0) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "rrecv 0 out of %d bytes on rsock %d\n",
			    conn->rsize - conn->roffset, conn->rsock);
ssa_log(SSA_LOG_DEFAULT, "rbuf %p rsize %d roffset %d state %d phase %d\n", conn->rbuf, conn->rsize, conn->roffset, conn->state, conn->phase);
		return 0;
	}

	return revents;
}

static short ssa_downstream_handle_rsock_revents(struct ssa_conn *conn,
						 short events,
						 struct ssa_svc *svc,
						 struct pollfd **fds)
{
	short revents = events;

	if (events & ~(POLLOUT | POLLIN)) {
		char event_str[128] = {};

		ssa_format_event(event_str, sizeof(event_str),
				 events & ~(POLLOUT | POLLIN));
		ssa_log(SSA_LOG_DEFAULT,
			"unexpected event 0x%x (%s) on data rsock %d\n",
			events & ~(POLLOUT | POLLIN), event_str, conn->rsock);
	}
	if (events & POLLIN) {
		if (!conn->rbuf) {
			conn->rbuf = malloc(sizeof(struct ssa_msg_hdr));
			if (conn->rbuf) {
				conn->rsize = sizeof(struct ssa_msg_hdr);
				conn->roffset = 0;
			} else
				ssa_log_err(SSA_LOG_CTRL,
					    "failed to allocate ssa_msg_hdr "
					    "for rrecv on data rsock %d\n",
					    conn->rsock);
		}
		if (conn->rbuf) {
			revents = ssa_downstream_rrecv(conn, events, svc, fds);
			if (!revents)
				return 0;
		}
	}
	if (events & POLLOUT) {
		if (!conn->rdma_write)
			revents = ssa_rsend_continue(conn, events);
		else
			revents = ssa_riowrite_continue(conn, events);
	}

	return revents;
}

static short ssa_downstream_notify_db_update(struct ssa_conn *conn,
					     uint64_t epoch)
{
	usleep(1000);	/* 1 msec delay is a temporary workaround so rsend does not indicate EAGAIN/EWOULDBLOCK !!! */

	return ssa_downstream_send(conn, SSA_MSG_DB_UPDATE, SSA_MSG_FLAG_END,
				   0, epoch, NULL, 0, POLLIN);
}

static int ssa_find_pollfd_slot(struct pollfd *fds, int nfds)
{
	int slot;

	for (slot = FIRST_DATA_FD_SLOT; slot < nfds; slot++)
		if (fds[slot].fd == -1)
			return slot;
	return -1;
}


static int ssa_downstream_smdb_xfer_in_progress(struct ssa_svc *svc,
						struct pollfd *fds, int nfds)
{
	struct ssa_conn *conn;
	int slot;

	for (slot = FIRST_DATA_FD_SLOT; slot < nfds; slot++) {
		if (fds[slot].fd == -1)
			continue;
		conn = svc->fd_to_conn[fds[slot].fd];
		if (conn && conn->dbtype == SSA_CONN_SMDB_TYPE) {
			if (conn->phase != SSA_DB_IDLE)
				return 1;
		}
	}

	return 0;
}

static void ssa_downstream_smdb_update_ready(struct ssa_conn *conn,
					     struct ssa_svc *svc,
					     struct pollfd **fds)
{
	int sock;

if (update_waiting) ssa_log(SSA_LOG_DEFAULT, "unexpected update waiting!\n");
	if (!ssa_downstream_smdb_xfer_in_progress(svc, (struct pollfd *)fds,
						  FD_SETSIZE)) {
ssa_log(SSA_LOG_DEFAULT, "No SMDB transfer currently in progress\n");
		if (svc->port->dev->ssa->node_type & SSA_NODE_CORE)
			sock = svc->sock_extractdown[0];
		else if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION)
			sock = svc->sock_updown[1];
		else
			sock = -1;
		update_waiting = 1;
		update_pending = 0;
		if (sock >= 0)
			ssa_send_db_update_ready(sock);
else ssa_log(SSA_LOG_DEFAULT, "No socket for update ready message\n");
	}
else ssa_log(SSA_LOG_DEFAULT, "SMDB transfer currently in progress\n");
}


static void ssa_downstream_close_ssa_conn(struct ssa_conn *conn,
					  struct ssa_svc *svc,
					  struct pollfd **fds)
{
ssa_log(SSA_LOG_DEFAULT, "conn %p phase %d dbtype %d\n", conn, conn->phase, conn->dbtype);

	if (conn->phase != SSA_DB_IDLE) {
		if (conn->dbtype == SSA_CONN_PRDB_TYPE) {
			ssa_downstream_conn(svc, conn, 1);
			ssa_close_ssa_conn(conn);
		} else if (conn->dbtype == SSA_CONN_SMDB_TYPE) {
			smdb_refcnt--;
ssa_log(SSA_LOG_DEFAULT, "SMDB %p ref count was just decremented to %u\n", ssa_downstream_db(conn), smdb_refcnt);
			ssa_downstream_conn(svc, conn, 1);
			ssa_close_ssa_conn(conn);
ssa_log(SSA_LOG_DEFAULT, "SMDB transfer in progress %d update pending %d\n", ssa_downstream_smdb_xfer_in_progress(svc, (struct pollfd *)fds, FD_SETSIZE), update_pending);
			if (update_pending)
				ssa_downstream_smdb_update_ready(conn, svc, fds);
		}
	} else {
		ssa_downstream_conn(svc, conn, 1);
		ssa_close_ssa_conn(conn);
	}
}

static void ssa_check_listen_events(struct ssa_svc *svc, struct pollfd **fds,
				    int conn_dbtype)
{
	struct ssa_conn *conn_data;
	struct pollfd *pfd;
	int fd, slot, i;

	conn_data = malloc(sizeof(*conn_data));
	if (conn_data) {
		ssa_init_ssa_conn(conn_data, SSA_CONN_TYPE_DOWNSTREAM,
				  conn_dbtype);
		fd = ssa_downstream_svc_server(svc, conn_data);
		if (fd >= 0) {
			ssa_set_runtime_counter_time(COUNTER_ID_TIME_LAST_DOWNSTR_CONN);
			if (!svc->fd_to_conn[fd]) {
				svc->fd_to_conn[fd] = conn_data;
				pfd = (struct  pollfd *)fds;
				slot = ssa_find_pollfd_slot(pfd, FD_SETSIZE);
				if (slot >= 0) {
					pfd = (struct  pollfd *)(fds + slot);
					pfd->fd = fd;
					pfd->events = POLLIN;
					pfd->revents = 0;
					if (conn_dbtype == SSA_CONN_PRDB_TYPE)
						ssa_log(SSA_LOG_DEFAULT,
							"PRDB connection accepted, but access notification is deferred until RDMA epoch buffer is published\n");
					else if (conn_dbtype == SSA_CONN_SMDB_TYPE) {
						ssa_downstream_conn(svc, conn_data, 0);
						if (!update_pending && !update_waiting && smdb)
							pfd->events = ssa_downstream_notify_db_update(conn_data, epoch);
else ssa_log(SSA_LOG_DEFAULT, "SMDB connection accepted but notify DB update deferred since update is pending %d or waiting %d or no SMDB\n", update_pending, update_waiting);
					} else {
						ssa_close_ssa_conn(conn_data);
						free(conn_data);
						conn_data = NULL;
						svc->fd_to_conn[fd] = NULL;
						ssa_log_warn(SSA_LOG_CTRL,
							     "connection db type %d not PRDB or SMDB\n",
							     conn_dbtype);
					}
				} else {
					ssa_close_ssa_conn(conn_data);
					free(conn_data);
					conn_data = NULL;
					svc->fd_to_conn[fd] = NULL;
					ssa_log_warn(SSA_LOG_CTRL,
						     "no pollfd slot available for rsock %d\n",
						     fd);
				}
			} else {
				ssa_close_ssa_conn(conn_data);
				free(conn_data);
				conn_data = NULL;
				ssa_log_warn(SSA_LOG_CTRL,
					     "rsock %d in fd_to_conn array already occupied\n",
					     fd);
			}
		}
	} else
		ssa_log_err(SSA_LOG_DEFAULT, "struct ssa_conn allocation failed\n");

	if (conn_data) {
		for (i = 0; i < FD_SETSIZE; i++) {
			if (svc->fd_to_conn[i] &&
			    svc->fd_to_conn[i]->rsock >= 0 &&
			    svc->fd_to_conn[i] != conn_data &&
			    !memcmp(svc->fd_to_conn[i]->remote_gid.raw,
				    conn_data->remote_gid.raw, 16)) {
				ssa_sprint_addr(SSA_LOG_CTRL, log_data,
						sizeof log_data, SSA_ADDR_GID,
						conn_data->remote_gid.raw,
						sizeof conn_data->remote_gid.raw);
				ssa_log_warn(SSA_LOG_CTRL,
					     "removing old connection for "
					     "rsock %d GID %s LID %u\n",
					     i, log_data, conn_data->remote_lid);

				ssa_downstream_close_ssa_conn(svc->fd_to_conn[i],
							      svc, fds);
				svc->fd_to_conn[i] = NULL;

				for (slot = FIRST_DATA_FD_SLOT; slot < FD_SETSIZE; slot++) {
					pfd = (struct pollfd *)(fds + slot);
					if (pfd->fd == i) {
						pfd->fd = -1;
						pfd->events = 0;
						pfd->revents = 0;
						break;
					}
				}

				if (slot == FD_SETSIZE)
					ssa_log_err(SSA_LOG_DEFAULT,
						    "unable to find rsock %d in fd_to_conn struct\n", i);
			}
		}
	}
}

static void ssa_downstream_notify_smdb_conns(struct ssa_svc *svc,
					     struct pollfd *fds, int nfds,
					     uint64_t epoch)
{
	struct ssa_conn *conn;
	struct pollfd *pfd;
	int slot;

	for (slot = FIRST_DATA_FD_SLOT; slot < nfds; slot++) {
		if (fds[slot].fd == -1)
			continue;
		conn = svc->fd_to_conn[fds[slot].fd];
		if (conn && conn->dbtype == SSA_CONN_SMDB_TYPE) {
			pfd = (struct pollfd *)(fds + slot);
			pfd->events = ssa_downstream_notify_db_update(conn, epoch);
		}
	}
}

static void ssa_send_db_update_ready(int fd)
{
	int ret;
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_UPDATE_READY;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = NULL;
	msg.db_upd.svc = NULL;
	msg.db_upd.flags = 0;
	memset(&msg.db_upd.remote_gid, 0, sizeof(msg.db_upd.remote_gid));
	msg.db_upd.remote_lid = 0;
	msg.db_upd.epoch = DB_EPOCH_INVALID;
	ret = write(fd, (char *) &msg, sizeof(msg));
	if (ret != sizeof(msg))
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof(msg));
}

static void ssa_downstream_dev_event(struct ssa_svc *svc,
				     struct ssa_ctrl_msg_buf *msg,
				     struct pollfd **fds)
{
	struct pollfd *pfd;
	int i, slot;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", svc->name,
		ibv_event_type_str(msg->data.event));
	switch (msg->data.event) {
	case IBV_EVENT_SM_CHANGE:
		/* Do nothing for non-core node */
		if ((svc->port->dev->ssa->node_type & SSA_NODE_CORE) == 0)
			break;
		/* Core node became MASTER */
		if (svc->port->state == IBV_PORT_ACTIVE &&
		    svc->port->sm_lid == svc->port->lid) {
			ssa_downstream_start_listen(svc, fds);
			break;
		}
		/*
		 * In case of SM handover/failover, core node (old SM master)
		 * closes all downstream connections
		 */
	case IBV_EVENT_PORT_ERR:
#if 0
		/* Listening rsockets are not closed, due to RDMA CM library limitation */
		if (svc->conn_listen_smdb.rsock >= 0) {
			ssa_close_ssa_conn(&svc->conn_listen_smdb);
			pfd = (struct pollfd *)(fds + SMDB_LISTEN_FD_SLOT);
			pfd->fd = -1;
			pfd->events = 0;
			pfd->revents = 0;
		}
		if (svc->conn_listen_prdb.rsock >= 0) {
			ssa_close_ssa_conn(&svc->conn_listen_prdb);
			pfd = (struct pollfd *)(fds + PRDB_LISTEN_FD_SLOT);
			pfd->fd = -1;
			pfd->events = 0;
			pfd->revents = 0;
		}
#endif
		for (i = 0; i < FD_SETSIZE; i++) {
			if (svc->fd_to_conn[i] &&
			    svc->fd_to_conn[i]->rsock >= 0) {
				ssa_downstream_close_ssa_conn(svc->fd_to_conn[i],
							      svc, fds);
				svc->fd_to_conn[i] = NULL;

				for (slot = FIRST_DATA_FD_SLOT; slot < FD_SETSIZE; slot++) {
					pfd = (struct pollfd *)(fds + slot);
					if (pfd->fd == i) {
						pfd->fd = -1;
						pfd->events = 0;
						pfd->revents = 0;
						break;
					}
				}

				if (slot == FD_SETSIZE)
					ssa_log_err(SSA_LOG_DEFAULT,
						    "unable to find rsock %d in fd_to_conn struct\n", i);
			}
		}
		break;
	case IBV_EVENT_PORT_ACTIVE:
		ssa_downstream_start_listen(svc, fds);
		break;
	default:
		break;
	}
}

static void ssa_downstream_start_listen(struct ssa_svc *svc, struct pollfd **fds)
{
	struct pollfd *pfd;

	if (svc->port->dev->ssa->node_type &
	    (SSA_NODE_CORE | SSA_NODE_DISTRIBUTION)) {
		pfd = (struct pollfd *)(fds + SMDB_LISTEN_FD_SLOT);
		pfd->fd = ssa_downstream_listen(svc, &svc->conn_listen_smdb, smdb_port);
	}

	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		pfd = (struct pollfd *)(fds + PRDB_LISTEN_FD_SLOT);
		pfd->fd = ssa_downstream_listen(svc, &svc->conn_listen_prdb, prdb_port);
	}
}

static void *ssa_downstream_handler(void *context)
{
	struct ssa_svc *svc = context;
	struct pollfd **fds;
	struct pollfd *pfd, *pfd2;
	struct ssa_conn *conn;
	int ret, i, count;
	struct ssa_ctrl_msg_buf msg;

	SET_THREAD_NAME(svc->downstream, "DN_%s", svc->name);

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	ret = write(svc->sock_downctrl[1], (char *) &msg, sizeof msg.hdr);
	if (ret != sizeof msg.hdr)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg.hdr);

	fds = calloc(FD_SETSIZE, sizeof(**fds));
	if (!fds)
		goto out;
	pfd = (struct pollfd *)fds;
	pfd->fd = svc->sock_downctrl[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 1);
	pfd->fd = svc->sock_accessdown[0];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 2);
	pfd->fd = svc->sock_updown[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 3);
	pfd->fd = svc->sock_extractdown[0];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + SMDB_LISTEN_FD_SLOT);
	pfd->fd = -1;	/* placeholder for SMDB listen rsock */
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + PRDB_LISTEN_FD_SLOT);
	pfd->fd = -1;	/* placeholder for PRDB listen rsock */
	pfd->events = POLLIN;
	pfd->revents = 0;
	for (i = FIRST_DATA_FD_SLOT; i < FD_SETSIZE; i++) {
		pfd = (struct pollfd *)(fds + i);
		pfd->fd = -1;	/* placeholder for downstream connections */
		pfd->events = 0;
		pfd->revents = 0;
	}
	update_pending = 0;
	update_waiting = 0;

	for (;;) {
		ret = rpoll((struct pollfd *)fds, FD_SETSIZE, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}
		pfd = (struct pollfd *)fds;
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(svc->sock_downctrl[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from ctrl\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_downctrl[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from ctrl\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_LISTEN:
				ssa_downstream_start_listen(svc, fds);
				break;
			case SSA_CTRL_EXIT:
				goto out;
			case SSA_CTRL_DEV_EVENT:
				ssa_downstream_dev_event(svc, &msg, fds);
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 1);
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(svc->sock_accessdown[0], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from access\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_accessdown[0],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from access\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE:
				ssa_sprint_addr(SSA_LOG_DEFAULT, log_data,
						sizeof log_data, SSA_ADDR_GID,
						msg.data.db_upd.remote_gid.raw,
						sizeof msg.data.db_upd.remote_gid.raw);
				ssa_log(SSA_LOG_DEFAULT,
					"SSA DB update from access: rsock %d GID %s LID %u ssa_db %p\n",
					msg.data.db_upd.rsock, log_data,
					msg.data.db_upd.remote_lid,
					msg.data.db_upd.db);
				conn = NULL;
				/* Use rsock in DB update msg as hint */
				i = msg.data.db_upd.rsock;
				if (svc->fd_to_conn[i] &&
				    svc->fd_to_conn[i]->rsock == i &&
				    !memcmp(svc->fd_to_conn[i]->remote_gid.raw,
					    msg.data.db_upd.remote_gid.raw, 16)) {
					conn = svc->fd_to_conn[i];
				} else {
					/* Full search when rsock hint doesn't work */
					for (i = 0; i < FD_SETSIZE; i++) {
						if (svc->fd_to_conn[i] &&
						    svc->fd_to_conn[i]->rsock >= 0 &&
						    !memcmp(svc->fd_to_conn[i]->remote_gid.raw,
							    msg.data.db_upd.remote_gid.raw, 16)) {
							conn = svc->fd_to_conn[i];
							break;
						}
					}
				}

				if (conn && conn->rsock != msg.data.db_upd.rsock)
					ssa_log_warn(SSA_LOG_DEFAULT,
						     "client %s reconnected from rsock %d to rsock %d\n",
						     log_data,
						     msg.data.db_upd.rsock,
						     conn->rsock);

				/* Now ready to rsend to downstream client upon request */
				if (conn && conn->state == SSA_CONN_CONNECTED) {
					if (conn->phase == SSA_DB_IDLE &&
					    conn->epoch_len > 0) {
						uint64_t prdb_epoch;
						struct ssa_db *prdb_destroy = NULL;

						prdb_epoch = ssa_db_get_epoch(msg.data.db_upd.db, DB_DEF_TBL_ID);

						if (prdb_epoch > conn->epoch ||
						    conn->epoch == DB_EPOCH_INVALID) {
							if (conn->ssa_db)
								prdb_destroy = conn->ssa_db;
							conn->ssa_db = msg.data.db_upd.db;
							conn->epoch = prdb_epoch;
							conn->prdb_epoch = htonll(conn->epoch);
							ssa_log(SSA_LOG_DEFAULT, "PRDB %p epoch 0x%" PRIx64 " epoch length %d\n", conn->ssa_db, ntohll(conn->prdb_epoch), conn->epoch_len);
							if (conn->epoch_len ==
							    sizeof(conn->prdb_epoch)) {
								pfd2 = (struct pollfd *)(fds + i);
								pfd2->events = ssa_riowrite(conn, POLLIN);
							} else
								ssa_log(SSA_LOG_DEFAULT,
									"epoch length is %d but should be %d\n",
									conn->epoch_len,
									sizeof(conn->prdb_epoch));
						} else
							prdb_destroy = msg.data.db_upd.db;

						ssa_db_destroy(prdb_destroy);

#ifdef ACCESS
					} else {
						struct ssa_db_update db_upd;

						ssa_db_update_init(svc, msg.data.db_upd.db,
								   msg.data.db_upd.remote_lid,
								   &msg.data.db_upd.remote_gid,
								   conn->rsock,
								   0, 0, &db_upd);
						ssa_push_db_update(&update_queue,
								   &db_upd);
#endif
					}
				} else {
					ssa_sprint_addr(SSA_LOG_CTRL, log_data,
							sizeof log_data, SSA_ADDR_GID,
							msg.data.db_upd.remote_gid.raw,
							sizeof msg.data.db_upd.remote_gid.raw);
					ssa_log_warn(SSA_LOG_CTRL,
						     "DB update for GID %s currently not connected\n",
						     log_data);
					ssa_db_destroy(msg.data.db_upd.db);
				}
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from access\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 2);
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(svc->sock_updown[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from upstream\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_updown[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from upstream\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE_PREPARE:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_PREPARE from upstream\n");
if (update_waiting) ssa_log(SSA_LOG_DEFAULT, "unexpected update waiting!\n");
				if (ssa_downstream_smdb_xfer_in_progress(svc,
									 (struct pollfd *)fds,
									 FD_SETSIZE)) {
ssa_log(SSA_LOG_DEFAULT, "SMDB transfer currently in progress\n");
					update_pending = 1;
				} else {
ssa_log(SSA_LOG_DEFAULT, "No SMDB transfer currently in progress\n");
					update_waiting = 1;
if (update_pending) ssa_log(SSA_LOG_DEFAULT, "unexpected update pending!\n");
					ssa_send_db_update_ready(svc->sock_updown[1]);
				}
				break;
			case SSA_DB_UPDATE:
				ssa_log(SSA_LOG_DEFAULT,
					"SSA DB update (SMDB) from upstream: ssa_db %p epoch 0x%" PRIx64 "\n",
					msg.data.db_upd.db, msg.data.db_upd.epoch);
				smdb = msg.data.db_upd.db;
				update_waiting = 0;
				epoch = msg.data.db_upd.epoch;
				ssa_downstream_notify_smdb_conns(svc,
								 (struct pollfd *)fds,
								 FD_SETSIZE,
								 epoch);
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from upstream\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 3);
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(svc->sock_extractdown[0], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from extract\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(svc->sock_extractdown[0],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from extract\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif
			switch (msg.hdr.type) {
			case SSA_DB_UPDATE_PREPARE:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_PREPARE from extract\n");
if (update_waiting) ssa_log(SSA_LOG_DEFAULT, "unexpected update waiting!\n");
				if (ssa_downstream_smdb_xfer_in_progress(svc,
									 (struct pollfd *)fds,
									 FD_SETSIZE)) {
ssa_log(SSA_LOG_DEFAULT, "SMDB transfer currently in progress\n");
					update_pending = 1;
				} else {
ssa_log(SSA_LOG_DEFAULT, "No SMDB transfer currently in progress\n");
					update_waiting = 1;
if (update_pending) ssa_log(SSA_LOG_DEFAULT, "unexpected update pending!\n");
					ssa_send_db_update_ready(svc->sock_extractdown[0]);
				}
				break;
			case SSA_DB_UPDATE:
				ssa_log(SSA_LOG_DEFAULT,
					"SSA DB update (SMDB) from extract: ssa_db %p flags 0x%x epoch 0x%" PRIx64 "\n",
					msg.data.db_upd.db, msg.data.db_upd.flags,
					msg.data.db_upd.epoch);
				smdb = msg.data.db_upd.db;
				update_waiting = 0;
				epoch = msg.data.db_upd.epoch;
				if (msg.data.db_upd.flags & SSA_DB_UPDATE_CHANGE)
					ssa_downstream_notify_smdb_conns(svc,
									 (struct pollfd *)fds,
									 FD_SETSIZE,
									 epoch);
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from extract\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + SMDB_LISTEN_FD_SLOT);
		if (pfd->revents & (POLLERR | POLLHUP | POLLNVAL)) {
			char event_str[128] = {};

			ssa_format_event(event_str, sizeof(event_str),
					 pfd->revents);
			ssa_log_err(SSA_LOG_DEFAULT,
				    "error event 0x%x (%s) on SMDB listen rsock %d\n",
				    pfd->revents, event_str, pfd->fd);
#if 0
			/* TODO: uncomment when RDMA CM library limitations will be understood better */
			if (svc->conn_listen_smdb.rsock >= 0)
				ssa_close_ssa_conn(&svc->conn_listen_smdb);
			pfd->fd = -1;
#else
			pfd->revents = 0;
#endif
		} else if (pfd->revents) {
			pfd->revents = 0;
			ssa_check_listen_events(svc, fds, SSA_CONN_SMDB_TYPE);
		}

		pfd = (struct pollfd *)(fds + PRDB_LISTEN_FD_SLOT);
		if (pfd->revents & (POLLERR | POLLHUP | POLLNVAL)) {
			char event_str[128] = {};

			ssa_format_event(event_str, sizeof(event_str),
					 pfd->revents);
			ssa_log_err(SSA_LOG_DEFAULT,
				    "error event 0x%x (%s) on PRDB listen rsock %d\n",
				    pfd->revents, event_str, pfd->fd);
#if 0
			/* TODO: uncomment when RDMA CM library limitations are better understood */
			if (svc->conn_listen_prdb.rsock >= 0)
				ssa_close_ssa_conn(&svc->conn_listen_prdb);
			pfd->fd = -1;
#else
			pfd->revents = 0;
#endif
		} else if (pfd->revents) {
			pfd->revents = 0;
			ssa_check_listen_events(svc, fds, SSA_CONN_PRDB_TYPE);
		}

		count = 0;
		for (i = FIRST_DATA_FD_SLOT; i < FD_SETSIZE; i++) {
			pfd = (struct pollfd *)(fds + i);
			if (pfd->fd >= 0)
				count++;
			if (pfd->revents) {
				if (pfd->revents & (POLLERR | POLLHUP | POLLNVAL)) {
					char event_str[128] = {};

					ssa_format_event(event_str,
							 sizeof(event_str),
							 pfd->revents);
					ssa_log_err(SSA_LOG_DEFAULT,
						    "error event 0x%x (%s) on rsock %d\n",
						    pfd->revents, event_str, pfd->fd);
					/* Update distribution tree (at least when core) ? */
					/* Also, when not core, need to notify core via SSA MAD */
					if (svc->fd_to_conn[pfd->fd])
						ssa_downstream_close_ssa_conn(svc->fd_to_conn[pfd->fd], svc, fds);
					svc->fd_to_conn[pfd->fd] = NULL;
					pfd->fd = -1;
					pfd->events = 0;
					pfd->revents = 0;
				} else {
					if (svc->fd_to_conn[pfd->fd]) {
						pfd->events = ssa_downstream_handle_rsock_revents(svc->fd_to_conn[pfd->fd], pfd->revents, svc, fds);
						if (!pfd->events) {
							ssa_downstream_close_ssa_conn(svc->fd_to_conn[pfd->fd], svc, fds);
							svc->fd_to_conn[pfd->fd] = NULL;
							pfd->fd = -1;
						}
					} else {
						char event_str[128] = {};

						ssa_format_event(event_str,
								 sizeof(event_str),
								 pfd->revents);
						ssa_log_warn(SSA_LOG_CTRL,
							     "event 0x%x (%s) on data rsock %d pollfd slot %d but fd_to_conn slot is empty\n",
							     pfd->revents,
							     event_str,
							     pfd->fd, i);
					}
				}
			}
			pfd->revents = 0;
		}
		ssa_set_runtime_counter(COUNTER_ID_NUM_CHILDREN, count);
	}

out:
	if (fds)
		free(fds);
	return NULL;
}

#ifdef ACCESS
static void ssa_access_send_db_update(struct ssa_svc *svc, struct ssa_db *db,
				      int rsock, int flags, uint16_t remote_lid,
				      union ibv_gid *remote_gid)
{
	int ret;
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_UPDATE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = db;
	msg.db_upd.svc = NULL;
	msg.db_upd.rsock = rsock;
	msg.db_upd.flags = flags;
	if (remote_gid)
		memcpy(&msg.db_upd.remote_gid, remote_gid, 16);
	else
		memset(&msg.db_upd.remote_gid, 0, 16);
	msg.db_upd.remote_lid = remote_lid;
	msg.db_upd.epoch = DB_EPOCH_INVALID;	/* not used */
	ret = write(svc->sock_accessdown[1], (char *) &msg, sizeof(msg));
	if (ret != sizeof(msg))
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof(msg));
}

static struct ssa_db *ssa_calculate_prdb(struct ssa_svc *svc,
					 struct ssa_access_member *consumer)
{
	struct ssa_db *prdb = NULL;
	struct ssa_db *prdb_copy = NULL;
	int n, ret;
	uint64_t epoch, prdb_epoch, actual_epoch;
	char dump_dir[1024];
	struct stat dstat;

	epoch = ssa_db_get_epoch(access_context.smdb, DB_DEF_TBL_ID);
	prdb_epoch = ssa_db_get_epoch(consumer->prdb_current, DB_DEF_TBL_ID);

	/* Call below "pulls" in access layer for any node type (if ACCESS defined) !!! */
	ret = ssa_pr_compute_half_world(access_context.smdb,
					access_context.context,
					consumer->gid.global.interface_id,
					&prdb);
	if (ret == SSA_PR_PORT_ABSENT) {
		ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
				SSA_ADDR_GID, consumer->gid.raw,
				sizeof consumer->gid.raw);
		if (consumer->smdb_epoch == DB_EPOCH_INVALID)
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "GID %s not found in SMDB with epoch 0x%" PRIx64 "\n",
				     log_data, epoch);
		else
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "GID %s not found in SMDB with epoch 0x%" PRIx64
				     ". Last used epoch 0x%" PRIx64 "\n",
				     log_data, epoch, consumer->smdb_epoch);
	} else if (ret == SSA_PR_SUCCESS) {
		if (consumer->prdb_current) {
			ret = ssa_db_cmp(prdb, consumer->prdb_current);
			if (!ret) {
				ssa_sprint_addr(SSA_LOG_CTRL, log_data, sizeof log_data,
						SSA_ADDR_GID, consumer->gid.raw,
						sizeof consumer->gid.raw);
				ssa_log(SSA_LOG_CTRL,
					"PRDB calculated for GID %s is equal to "
					"previous PRDB with epoch 0x%" PRIx64 "\n",
					log_data, prdb_epoch);
			} else if (ret == -1) {
				ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
						SSA_ADDR_GID, consumer->gid.raw,
						sizeof consumer->gid.raw);
				ssa_log_err(SSA_LOG_DEFAULT,
					    "invalid PRDB structure for GID %s (new "
					    "prdb %p or previously calculated one %p)\n",
					    log_data, prdb, consumer->prdb_current);
			}
			/*
			 * Destroy new PRDB and don't send PRDB update,
			 * if it's equal to the previous PRDB (ret == 0), or
			 * it's structure is wrong (ret == -1). If
			 * structure is wrong, assumption is it's new PRDB
			 * structure.
			 */
			if (ret != 1) {
				ssa_db_destroy(prdb);
				return NULL;
			}
		}

		prdb_copy = ssa_db_copy(prdb);
		if (!prdb_copy) {
			ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
					SSA_ADDR_GID, consumer->gid.raw,
					sizeof consumer->gid.raw);
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "PRDB copy not created for GID %s for SMDB with epoch 0x%" PRIx64 "\n",
				     log_data, epoch);
		}

		if (prdb_dump) {
			n = snprintf(dump_dir, sizeof(dump_dir),
				     "%s.", prdb_dump_dir);
			snprintf(dump_dir + n, sizeof(dump_dir) - n,
				 "0x%" PRIx64,
				 ntohll(consumer->gid.global.interface_id));
			if (lstat(dump_dir, &dstat)) {
				if (mkdir(dump_dir, 0755)) {
					ssa_sprint_addr(SSA_LOG_CTRL, log_data,
							sizeof log_data,
							SSA_ADDR_GID,
							consumer->gid.raw,
							sizeof consumer->gid.raw);
					ssa_log_err(SSA_LOG_CTRL,
						    "prdb dump to %s for GID %s: %d (%s)\n",
						    dump_dir, log_data,
						    errno, strerror(errno));
					goto skip_db_save;
				}
			}
			ssa_db_save(dump_dir, prdb, prdb_dump);
		}
	} else {
		ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
				SSA_ADDR_GID, consumer->gid.raw,
				sizeof consumer->gid.raw);
		ssa_log(SSA_LOG_DEFAULT,
			"PRDB calculation for GID %s failed for SMDB with epoch 0x%" PRIx64 "\n",
			log_data, epoch);

		if (err_smdb_dump) {
			n = snprintf(dump_dir, sizeof(dump_dir),
				     "%s.0x%" PRIx64, smdb_dump_dir, epoch);
			if (lstat(dump_dir, &dstat)) {
				if (mkdir(dump_dir, 0755)) {
					ssa_sprint_addr(SSA_LOG_CTRL, log_data,
							sizeof log_data,
							SSA_ADDR_GID, consumer->gid.raw,
							sizeof consumer->gid.raw);
					ssa_log_err(SSA_LOG_CTRL,
						    "SMDB error dump to %s for GID %s: %d (%s)\n",
						    dump_dir, log_data,
						    errno, strerror(errno));
					goto skip_db_save;
				}
				ssa_db_save(dump_dir, access_context.smdb,
					    err_smdb_dump);
			}
			ssa_log(SSA_LOG_DEFAULT, "SMDB dump %s\n", dump_dir);
		}
	}

skip_db_save:
	if (prdb != NULL) {
		if (++prdb_epoch == DB_EPOCH_INVALID)
			prdb_epoch++;
		actual_epoch = ssa_db_set_epoch(prdb, DB_DEF_TBL_ID, prdb_epoch);
		if (actual_epoch == DB_EPOCH_INVALID)
			ssa_log(SSA_LOG_VERBOSE, "PRDB epoch set failed\n");
		actual_epoch = ssa_db_set_epoch(prdb_copy, DB_DEF_TBL_ID, prdb_epoch);
		if (actual_epoch == DB_EPOCH_INVALID)
			ssa_log(SSA_LOG_VERBOSE, "PRDB copy epoch set failed\n");
		consumer->smdb_epoch = epoch;
		ssa_db_destroy(consumer->prdb_current);
		consumer->prdb_current = prdb;
	}
	return prdb_copy;
}

static void
ssa_db_update_init(struct ssa_svc *svc, struct ssa_db *db,
		   uint16_t remote_lid, union ibv_gid *remote_gid,
		   int rsock, int flags, uint64_t epoch,
		   struct ssa_db_update *p_db_upd)
{
	p_db_upd->db = db;
	p_db_upd->svc = svc;
	p_db_upd->rsock = rsock;
	p_db_upd->flags = flags;
	if (remote_gid)
		memcpy(&p_db_upd->remote_gid, remote_gid, 16);
	else
		memset(&p_db_upd->remote_gid, 0, 16);
	p_db_upd->remote_lid = remote_lid;
	p_db_upd->epoch = epoch;
}

static void ssa_wait_db_update(struct ssa_db_update_queue *p_queue)
{
	pthread_mutex_lock(&p_queue->cond_lock);
	while (DListEmpty(&p_queue->list))
		pthread_cond_wait(&p_queue->cond_var, &p_queue->cond_lock);
	pthread_mutex_unlock(&p_queue->cond_lock);
}

static int ssa_push_db_update(struct ssa_db_update_queue *p_queue,
			      struct ssa_db_update *db_upd)
{
	struct ssa_db_update_record *p_rec;

	p_rec = (struct ssa_db_update_record *) malloc(sizeof(*p_rec));
	if (!p_rec) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to allocate ssa_db_update queue record\n");
		return -1;
	}

	p_rec->db_upd = *db_upd;

	pthread_mutex_lock(&p_queue->lock);
	DListInsertTail(&p_rec->list_entry, &p_queue->list);
	pthread_mutex_unlock(&p_queue->lock);

	/* send signal for start processing the queue */
	pthread_mutex_lock(&p_queue->cond_lock);
	pthread_cond_signal(&p_queue->cond_var);
	pthread_mutex_unlock(&p_queue->cond_lock);

	return 0;
}

static int ssa_pull_db_update(struct ssa_db_update_queue *p_queue,
			      struct ssa_db_update *p_db_upd)
{
	struct ssa_db_update_record *p_rec;
	DLIST_ENTRY *head;

	pthread_mutex_lock(&p_queue->lock);
	if (!DListEmpty(&p_queue->list)) {
		head = p_queue->list.Next;
		DListRemove(head);
		pthread_mutex_unlock(&p_queue->lock);
		p_rec = container_of(head, struct ssa_db_update_record,
				     list_entry);
		*p_db_upd = p_rec->db_upd;
		free(p_rec);
		return 1;
	}
	pthread_mutex_unlock(&p_queue->lock);

	return 0;
}

static void g_al_callback(gpointer task, gpointer user_data)
{
	struct ssa_access_task *al_task;
	struct ssa_svc *svc;
	struct ssa_access_member *consumer;
	struct ssa_db *prdb;
	struct ssa_db_update db_upd;
	long num_tasks;

	(void) user_data;

	if (task == NULL)
		return;

	al_task = (struct ssa_access_task *) task;
	consumer = al_task->consumer;
	svc = al_task->svc;

	ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
			SSA_ADDR_GID, consumer->gid.raw,
			sizeof consumer->gid.raw);
	ssa_log(SSA_LOG_DEFAULT,
		"calculating PRDB for GID %s LID %u client\n",
		log_data, consumer->lid);
	prdb = ssa_calculate_prdb(svc, consumer);
	ssa_log(SSA_LOG_DEFAULT,
		"GID %s LID %u rsock %d PRDB %p calculation complete\n",
		log_data, consumer->lid, consumer->rsock, prdb);
#ifdef SIM_SUPPORT_FAKE_ACM
	if (ACM_FAKE_RSOCKET_ID == consumer->rsock) {
		if (prdb)
			ssa_db_destroy(prdb);
		goto out;
	}
#endif
	if (prdb) {
		if (consumer->rsock >= 0) {
			ssa_db_update_init(svc, prdb, consumer->lid,
					   &consumer->gid, consumer->rsock,
					   0, 0, &db_upd);
			ssa_push_db_update(&update_queue, &db_upd);
		} else
			ssa_db_destroy(prdb);
	} else
		ssa_log(SSA_LOG_DEFAULT, "No new PRDB calculated\n");
#ifdef SIM_SUPPORT_FAKE_ACM
out:
#endif
	pthread_mutex_lock(&access_context.th_pool_mtx);
	num_tasks = atomic_dec(&access_context.num_tasks);
	ssa_set_runtime_counter(COUNTER_ID_NUM_ACCESS_TASKS, num_tasks);
	pthread_cond_signal(&access_context.th_pool_cond);
	pthread_mutex_unlock(&access_context.th_pool_mtx);
	free(task);
}

static void ssa_access_map_callback(const void *nodep, const VISIT which,
				    const void *priv)
{
	struct ssa_access_member *consumer;
	struct ssa_svc *svc = (struct ssa_svc *) priv;
	const char *node_type = NULL;
	struct ssa_access_task *task;
	short update_prdb = 0;

	switch (which) {
	case preorder:
		break;
	case postorder:
		node_type = "Internal";
		update_prdb = 1;
		break;
	case endorder:
		break;
	case leaf:
		node_type = "Leaf";
		update_prdb = 1;
		break;
	}

	if (update_prdb) {
		consumer = container_of(* (struct ssa_access_member **) nodep,
					struct ssa_access_member, gid);
		ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data,
				SSA_ADDR_GID, consumer->gid.raw,
				sizeof consumer->gid.raw);
		ssa_log(SSA_LOG_DEFAULT,
			"%s GID %s LID %u rsock %d pushing task to access thread pool\n",
			node_type, log_data, consumer->lid, consumer->rsock);
		task = calloc(1, sizeof(*task));
		task->svc = svc;
		task->consumer = consumer;
		atomic_inc(&access_context.num_tasks);
		ssa_access_process_task(task);
	}
}

static void prdb_handler_cleanup(void *context)
{
	DLIST_ENTRY *list, *entry, *entry_tmp;
	struct ssa_db_update_record *p_rec;
	struct ssa_db_update_queue *p_queue =
		(struct ssa_db_update_queue *) context;

	pthread_mutex_lock(&p_queue->lock);
	list = &p_queue->list;
	if (!DListEmpty(list)) {
		entry = list->Next;
		while (entry != list) {
			p_rec = container_of(entry, struct ssa_db_update_record,
					     list_entry);
			entry_tmp = entry;
			entry = entry->Next;
			DListRemove(entry_tmp);
			free(p_rec);
		}
	}
	pthread_mutex_unlock(&p_queue->lock);
}

static void *ssa_access_prdb_handler(void *context)
{
	struct ssa_db_update db_upd;

	SET_THREAD_NAME(*access_prdb_handler, "ACCESS_PRDB");

	ssa_log_func(SSA_LOG_CTRL);

	pthread_cleanup_push(prdb_handler_cleanup, &update_queue);

	while (1) {
		ssa_wait_db_update(&update_queue);
		while (ssa_pull_db_update(&update_queue, &db_upd) > 0) {
			ssa_access_send_db_update(db_upd.svc, db_upd.db,
						  db_upd.rsock, db_upd.flags,
						  db_upd.remote_lid,
						  &db_upd.remote_gid);
		}
	}

	pthread_cleanup_pop(0);
	return NULL;
}

void ssa_twalk(const struct node_t *root,
	       void (*callback)(const void *nodep, const VISIT which,
				const void *priv),
	       const void *priv)
{
	if (root->left == NULL && root->right == NULL)
		callback(root, leaf, priv);
	else {
		callback(root, preorder, priv);
		if (root->left != NULL)
			ssa_twalk(root->left, callback, priv);
		callback(root, postorder, priv);
		if (root->right != NULL)
			ssa_twalk(root->right, callback, priv);
		callback(root, endorder, priv);
	}
}

static struct ssa_access_member *ssa_add_access_consumer(struct ssa_svc *svc,
							 union ibv_gid *remote_gid,
							 uint16_t remote_lid,
							 int rsock)
{
	struct ssa_access_member *consumer;
	uint8_t **tgid;

	tgid = tfind(remote_gid->raw, &svc->access_map, ssa_compare_gid);
	if (!tgid) {
		consumer = calloc(1, sizeof *consumer);
		if (!consumer) {
			ssa_log(SSA_LOG_DEFAULT,
				"no memory for ssa_access_member struct\n");
			return NULL;
		}
		memcpy(&consumer->gid, remote_gid, 16);
		consumer->smdb_epoch = DB_EPOCH_INVALID;
		if (!tsearch(&consumer->gid, &svc->access_map, ssa_compare_gid)) {
			free(consumer);
			ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					log_data, sizeof log_data, SSA_ADDR_GID,
					remote_gid->raw, sizeof remote_gid->raw);
			ssa_log(SSA_LOG_DEFAULT,
				"failed to insert consumer GID %s into access map\n",
				log_data);
			return NULL;
		}
	} else
		consumer = container_of(*tgid, struct ssa_access_member, gid);

	if (consumer) {
		consumer->rsock = rsock;
		consumer->lid = remote_lid;
	}

	return consumer;
}

#ifdef SIM_SUPPORT_FAKE_ACM
void ssa_access_insert_fake_clients(struct ssa_svc **svc_arr, int svc_cnt,
				    struct ssa_db *smdb)
{
	int i, j, count, n;
	const struct smdb_guid2lid *tbl;
	const struct smdb_subnet_opts *opt_rec;

	if (!fake_acm_num)
		return;

	opt_rec = (const struct smdb_subnet_opts *)smdb->pp_tables[SMDB_TBL_ID_SUBNET_OPTS];
	tbl = (struct smdb_guid2lid *)smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	n = ntohll(smdb->p_db_tables[SMDB_TBL_ID_GUID2LID].set_count);
	if (fake_acm_num < 0)
		fake_acm_num = n;


	for (j = 0; j < svc_cnt; j++) {
		count = 0;
		for (i = 0; i < n && count <= fake_acm_num; i++) {
			if (!tbl[i].is_switch) {
				union ibv_gid gid;	/* consumer GID */

				gid.global.interface_id = tbl[i].guid;
				gid.global.subnet_prefix = opt_rec->subnet_prefix;
				if (ssa_add_access_consumer(svc_arr[j], &gid,
							    tbl[i].lid,
							    ACM_FAKE_RSOCKET_ID)) {
					count++;

					ssa_sprint_addr(SSA_LOG_DEFAULT,
							log_data, sizeof log_data,
							SSA_ADDR_GID, gid.raw,sizeof gid.raw);
					ssa_log(SSA_LOG_DEFAULT,
						"added fake consumer GID %s into %s\n",
						log_data, svc_arr[j]->name);
				} else
					ssa_log_err(SSA_LOG_DEFAULT,
						    "adding fake consumer failed\n");
			}
		}
	}
}
#endif
#endif

static void *ssa_access_handler(void *context)
{
	struct ssa_class *ssa = context;
	struct ssa_device *dev;
	struct ssa_port *port;
	struct ssa_svc **svc_arr = NULL;
	struct pollfd **fds = NULL;
	struct pollfd *pfd;
	struct ssa_ctrl_msg_buf msg;
	struct ssa_db *prdb = NULL;
	int i, ret, d, p, s, svc_cnt = 0;
#ifdef ACCESS
	int j;
	struct ssa_access_member *consumer;
	struct ssa_db_update db_upd;
#endif

	SET_THREAD_NAME(*access_thread, "ACCESS");

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	ret = write(sock_accessctrl[1], (char *) &msg, sizeof msg.hdr);
	if (ret != sizeof msg.hdr)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg.hdr);

	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			svc_cnt += port->svc_cnt;
		}
	}

	fds = calloc(ACCESS_FIRST_SERVICE_FD_SLOT + svc_cnt * ACCESS_FDS_PER_SERVICE,
		     sizeof(**fds));
	if (!fds) {
		ssa_log_err(SSA_LOG_CTRL, "unable to allocate fds\n");
		goto out;
	}

	svc_arr = malloc(svc_cnt * sizeof(*svc_arr));
	if (!svc_arr) {
		ssa_log_err(SSA_LOG_CTRL, "unable to allocate svc lookup table\n");
		goto out;
	}

	i = 0;
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < port->svc_cnt; s++) {
				svc_arr[i++] = port->svc[s];
			}
		}
	}

	if (!access_context.context) {
		ssa_log_err(SSA_LOG_CTRL, "access context is empty\n");
		goto out;
	}

	pfd = (struct pollfd  *)fds;
	pfd->fd = sock_accessctrl[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd  *)(fds + 1);
	pfd->fd = sock_accessextract[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	for (i = 0; i < svc_cnt; i++) {
		pfd = (struct pollfd  *)(fds + ACCESS_FIRST_SERVICE_FD_SLOT +
					 i * ACCESS_FDS_PER_SERVICE);
		pfd->fd = svc_arr[i]->sock_accessup[1];
		pfd->events = POLLIN;
		pfd->revents = 0;
		pfd = (struct pollfd  *)(fds + ACCESS_FIRST_SERVICE_FD_SLOT +
					 i * ACCESS_FDS_PER_SERVICE + 1);
		pfd->fd = svc_arr[i]->sock_accessdown[1];
		pfd->events = POLLIN;
		pfd->revents = 0;
	}
	update_waiting = 0;

	for (;;) {
		ret = poll((struct pollfd *)fds,
			   ACCESS_FIRST_SERVICE_FD_SLOT + svc_cnt * ACCESS_FDS_PER_SERVICE,
			   -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}

		pfd = (struct pollfd *)fds;
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(sock_accessctrl[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from ctrl\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(sock_accessctrl[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from ctrl\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}

			switch (msg.hdr.type) {
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 1);
		if (pfd->revents) {
			pfd->revents = 0;
			ret = read(sock_accessextract[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from extract\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(sock_accessextract[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from extract\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE_PREPARE:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_PREPARE from extract\n");
if (update_waiting) ssa_log(SSA_LOG_DEFAULT, "unexpected update waiting!\n");
				update_waiting = 1;
				ssa_send_db_update_ready(sock_accessextract[1]);
				break;
			case SSA_DB_UPDATE:
				ssa_log(SSA_LOG_DEFAULT,
					"SSA DB update from extract: ssa_db %p flags 0x%x epoch 0x%" PRIx64 "\n",
					msg.data.db_upd.db, msg.data.db_upd.flags,
					msg.data.db_upd.epoch);
				update_waiting = 0;
				if (!(msg.data.db_upd.flags & SSA_DB_UPDATE_CHANGE))
					break;
#ifdef ACCESS
#ifdef SIM_SUPPORT_FAKE_ACM
				if (NULL == access_context.smdb)
					ssa_access_insert_fake_clients(svc_arr,
								       svc_cnt,
								       msg.data.db_upd.db);
#endif
#endif
				/* Should epoch be added to access context ? */
				access_context.smdb = msg.data.db_upd.db;
#ifdef ACCESS
				/* Reinit context should be based on DB update flags indicating full update */
				ssa_pr_reinit_context(access_context.context,
						      access_context.smdb);
				/* Recalculate PRDBs for all downstream ACMs!!! */
				/* Then cause RDMA write of the PRDB epochs */
				atomic_set(&access_context.num_tasks, 0);
				for (j = 0; j < svc_cnt; j++) {
					if (svc_arr[j]->access_map)
						ssa_twalk(svc_arr[j]->access_map,
							  ssa_access_map_callback,
							  svc_arr[j]);
				}
				ssa_access_wait_for_tasks_completion();
#endif
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from extract\n",
					     msg.hdr.type);
				break;
			}
		}

		for (i = 0; i < svc_cnt; i++) {
			pfd = (struct pollfd *)(fds +
						ACCESS_FIRST_SERVICE_FD_SLOT +
						i * ACCESS_FDS_PER_SERVICE);
			if (pfd->revents) {
				pfd->revents = 0;
				ret = read(svc_arr[i]->sock_accessup[1],
					   (char *) &msg, sizeof msg.hdr);
				if (ret != sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d header bytes read from upstream\n",
						    ret, sizeof msg.hdr);
				if (msg.hdr.len > sizeof msg.hdr) {
					ret = read(svc_arr[i]->sock_accessup[1],
						   (char *) &msg.hdr.data,
						   msg.hdr.len - sizeof msg.hdr);
					if (ret != msg.hdr.len - sizeof msg.hdr)
						ssa_log_err(SSA_LOG_CTRL,
							    "%d out of %d additional bytes read from upstream\n",
							    ret,
							    msg.hdr.len - sizeof msg.hdr);
				}
#if 0
				if (svc_arr[i]->process_msg &&
				    svc_arr[i]->process_msg(svc_arr[i], &msg))
					continue;
#endif

				switch (msg.hdr.type) {
				case SSA_DB_UPDATE_PREPARE:
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_UPDATE_PREPARE from upstream\n");
if (update_waiting) ssa_log(SSA_LOG_DEFAULT, "unexpected update waiting!\n");
					update_waiting = 1;
					ssa_send_db_update_ready(svc_arr[i]->sock_accessup[1]);
					break;
				case SSA_DB_UPDATE:
					ssa_log(SSA_LOG_DEFAULT,
						"SSA DB update from upstream thread: ssa_db %p\n",
						msg.data.db_upd.db);
					update_waiting = 0;
#ifdef ACCESS
#ifdef SIM_SUPPORT_FAKE_ACM
					if (NULL == access_context.smdb)
						ssa_access_insert_fake_clients(svc_arr,
									       svc_cnt,
									       msg.data.db_upd.db);
#endif
#endif
					/* Should epoch be added to access context ? */
					access_context.smdb = msg.data.db_upd.db;
#ifdef ACCESS
					/* Reinit context should be based on DB update flags indicating full update */
					ssa_pr_reinit_context(access_context.context,
							      access_context.smdb);
					/* Recalculate PRDBs for all downstream ACMs!!! */
					/* Then cause RDMA write of the PRDB epochs */
					atomic_set(&access_context.num_tasks, 0);
					if (svc_arr[i]->access_map)
						ssa_twalk(svc_arr[i]->access_map,
							  ssa_access_map_callback,
							  svc_arr[i]);
					ssa_access_wait_for_tasks_completion();
#endif
					break;
				default:
					ssa_log_warn(SSA_LOG_CTRL,
						     "ignoring unexpected msg "
						     "type %d from upstream\n",
						     msg.hdr.type);
					break;
				}
			}

			pfd = (struct pollfd *)(fds +
						ACCESS_FIRST_SERVICE_FD_SLOT +
						i * ACCESS_FDS_PER_SERVICE + 1);
			if (pfd->revents) {
				pfd->revents = 0;
				ret = read(svc_arr[i]->sock_accessdown[1],
					   (char *) &msg, sizeof msg.hdr);
				if (ret != sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d header bytes read from downstream\n",
						    ret, sizeof msg.hdr);
				if (msg.hdr.len > sizeof msg.hdr) {
					ret = read(svc_arr[i]->sock_accessdown[1],
						   (char *) &msg.hdr.data,
						   msg.hdr.len - sizeof msg.hdr);
					if (ret != msg.hdr.len - sizeof msg.hdr)
						ssa_log_err(SSA_LOG_CTRL,
							    "%d out of %d additional bytes read from downstream\n",
							    ret,
							    msg.hdr.len - sizeof msg.hdr);
				}
#if 0
				if (svc_arr[i]->process_msg &&
				    svc_arr[i]->process_msg(svc_arr[i], &msg))
					continue;
#endif

				switch (msg.hdr.type) {
				case SSA_CONN_DONE:
					ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE | SSA_LOG_CTRL,
							log_data, sizeof log_data,
							SSA_ADDR_GID,
							msg.data.conn_data.remote_gid.raw,
							sizeof msg.data.conn_data.remote_gid.raw);
					ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
						"connection done on rsock %d from GID %s LID %u\n",
						msg.data.conn_data.rsock, log_data,
						msg.data.conn_data.remote_lid);
					/* First, see if consumer GID in access map */
					/* Then, calculate half world PathRecords for GID if needed */
					/* Finally, "tell" downstream where this ssa_db struct is */
#ifdef ACCESS
					consumer = ssa_add_access_consumer(svc_arr[i],
									   &msg.data.conn_data.remote_gid,
									   msg.data.conn_data.remote_lid,
									   msg.data.conn_data.rsock);
					if (NULL == consumer) {
						ssa_log_err(SSA_LOG_DEFAULT,
							    "adding access consumer failed\n");
						continue;
					}

					if (update_waiting) {
						ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
							"access update waiting %d "
							"PRDB will be calculated after the update\n",
							update_waiting);
						continue;
					}

					if (access_context.smdb) {
						if (consumer->prdb_current) {
							if (consumer->smdb_epoch ==
							    ssa_db_get_epoch(access_context.smdb,
									     DB_DEF_TBL_ID)) {
								prdb = ssa_db_copy(consumer->prdb_current);
								goto skip_prdb_calc;
							}
						}

						ssa_log(SSA_LOG_DEFAULT,
							"calculating PRDB for GID %s LID %u client\n",
							log_data, consumer->lid);
						prdb = ssa_calculate_prdb(svc_arr[i], consumer);
						if (!prdb && consumer->prdb_current)
							 prdb = ssa_db_copy(consumer->prdb_current);
#endif
						if (!prdb)
							continue;
#ifdef ACCESS
						ssa_log(SSA_LOG_DEFAULT,
							"GID %s LID %u rsock %d PRDB %p calculation complete\n",
							log_data, msg.data.conn_data.remote_lid, msg.data.conn_data.rsock, prdb);
skip_prdb_calc:
						if (msg.data.conn_data.rsock >= 0) {
							ssa_db_update_init(svc_arr[i],
									   prdb,
									   msg.data.conn_data.remote_lid,
									   &msg.data.conn_data.remote_gid,
									   msg.data.conn_data.rsock,
									   0, 0,
									   &db_upd);
							ssa_push_db_update(&update_queue,
									   &db_upd);
						} else
							consumer->rsock = -1;
#endif
						/*
						 * TODO: destroy prdb database
						 * ssa_db_destroy(prdb);
						 */
#ifdef ACCESS
					} else
						ssa_log(SSA_LOG_CTRL,
							"smdb database is empty\n");
#endif
					break;
				case SSA_CONN_GONE:
					ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE | SSA_LOG_CTRL,
							log_data, sizeof log_data,
							SSA_ADDR_GID,
							msg.data.conn_data.remote_gid.raw,
							sizeof msg.data.conn_data.remote_gid.raw);
					ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
						"connection gone from GID %s LID %u\n",
						log_data, msg.data.conn_data.remote_lid);
					break;
				default:
					ssa_log_warn(SSA_LOG_CTRL,
						     "ignoring unexpected msg "
						     "type %d from downstream\n",
						     msg.hdr.type);
					break;
				}
			}
		}
	}

out:
	if (svc_arr)
		free(svc_arr);
	if (fds)
		free(fds);
	return NULL;
}

static void ssa_ctrl_port_send(struct ssa_port *port, struct ssa_ctrl_msg *msg)
{
	int i, ret;
	for (i = 0; i < port->svc_cnt; i++) {
		ret = write(port->svc[i]->sock_upctrl[0], msg, msg->len);
		if (ret != msg->len)
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to upstream\n",
				    ret, msg->len);
		if (port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
			ret = write(port->svc[i]->sock_downctrl[0],
				    msg, msg->len);
			if (ret != msg->len)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d bytes written to downstream\n",
					    ret, msg->len);
		}
	}
}

/*
static void ssa_ctrl_dev_send(struct ssa_device *dev, struct ssa_ctrl_msg *msg)
{
	struct ssa_port *port;
	int i;

	for (i = 1; i <= dev->port_cnt; i++) {
		port = ssa_dev_port(dev, i);
		if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
			continue;
		ssa_ctrl_port_send(port, msg);
	}
}
*/

static void ssa_ctrl_send_event(struct ssa_port *port, enum ibv_event_type event)
{
	struct ssa_ctrl_dev_event_msg msg;

	msg.hdr.len = sizeof msg;
	msg.hdr.type = SSA_CTRL_DEV_EVENT;
	msg.event = event;
	ssa_ctrl_port_send(port, &msg.hdr);
}

static void ssa_ctrl_update_port(struct ssa_port *port)
{
	struct ibv_port_attr attr;
#ifdef ACM
	union ibv_gid gid;
	uint16_t pkey;
	int i;
#endif
	int ret;

	ret = ibv_query_port(port->dev->verbs, port->port_num, &attr);
	if (ret) {
		ssa_log_err(0, "unable to get port state ERROR %d (%s)\n",
			    errno, strerror(errno));
		return;
	}

	if (attr.state == IBV_PORT_ACTIVE) {
		port->sm_lid = attr.sm_lid;
		port->sm_sl = attr.sm_sl;
		if (port->state != IBV_PORT_ACTIVE) {
			port->lid = attr.lid;
			port->lid_mask = 0xffff - ((1 << attr.lmc) - 1);
		}
		ibv_query_gid(port->dev->verbs, port->port_num, 0, &port->gid);
#ifdef ACM
		if (port->state != IBV_PORT_ACTIVE) {
			port->mtu = attr.active_mtu;
			port->rate = acm_get_rate(attr.active_width,
						  attr.active_speed);
			if (attr.subnet_timeout >= 8)
				port->subnet_timeout =
					1 << (attr.subnet_timeout - 8);
			for (port->gid_cnt = 0;; port->gid_cnt++) {
				ret = ibv_query_gid(port->dev->verbs,
						    port->port_num,
						    port->gid_cnt, &gid);
				if (ret || !gid.global.interface_id)
					break;
			}

			for (port->pkey_cnt = 0;; port->pkey_cnt++) {
				ret = ibv_query_pkey(port->dev->verbs,
						     port->port_num,
						     port->pkey_cnt, &pkey);
				if (ret || !pkey)
					break;
			}

			port->sa_dest.av.src_path_bits = 0;
			port->sa_dest.av.dlid = attr.sm_lid;
			port->sa_dest.av.sl = attr.sm_sl;
			port->sa_dest.av.port_num = port->port_num;
			port->sa_dest.remote_qpn = 1;
			attr.sm_lid = htons(attr.sm_lid);
			acm_set_dest_addr(&port->sa_dest, ACM_ADDRESS_LID,
					  (uint8_t *) &attr.sm_lid,
					  sizeof(attr.sm_lid));

			port->sa_dest.ah = ibv_create_ah(port->dev->pd,
							 &port->sa_dest.av);
			if (!port->sa_dest.ah)
				return;

			atomic_set(&port->sa_dest.refcnt, 1);
			for (i = 0; i < port->pkey_cnt; i++)
				 acm_ep_up(port, (uint16_t) i);
		}
	} else {
		if (port->state == IBV_PORT_ACTIVE) {
			/*
			 * We wait for the SA destination to be released.  We could use an
			 * event instead of a sleep loop, but it's not worth it given how
			 * infrequently we should be processing a port down event in practice.
			 */
			atomic_dec(&port->sa_dest.refcnt);
			while (atomic_get(&port->sa_dest.refcnt))
				sleep(0);
			ibv_destroy_ah(port->sa_dest.ah);
			ssa_log(SSA_LOG_VERBOSE, "%s %d is down\n",
				port->dev->verbs->device->name,
				port->port_num);
		}
#endif
	}
	port->state = attr.state;
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s state %s SM LID %d\n",
		port->name, ibv_port_state_str(port->state), port->sm_lid);
}

static void ssa_ctrl_device(struct ssa_device *dev)
{
	struct ibv_async_event event;
	struct ssa_port *port;
	int ret;
	uint16_t old_SM_lid;
	int forward_event_type;

	ssa_log(SSA_LOG_CTRL, "%s\n", dev->name);
	ret = ibv_get_async_event(dev->verbs, &event);
	if (ret)
		return;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
		"async event %s\n", ibv_event_type_str(event.event_type));
	switch (event.event_type) {
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_PORT_ERR:
		port = ssa_dev_port(dev, event.element.port_num);
		if (port->link_layer != IBV_LINK_LAYER_INFINIBAND) {
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "%s:%d link layer %d is not IB\n",
				     dev->name, event.element.port_num,
				     port->link_layer);
			break;
		}

		old_SM_lid = port->sm_lid;
		forward_event_type = event.event_type;

		ssa_ctrl_update_port(port);
		if (old_SM_lid != port->sm_lid &&
		    event.event_type == IBV_EVENT_CLIENT_REREGISTER)
			forward_event_type = IBV_EVENT_SM_CHANGE;
		ssa_ctrl_send_event(port, forward_event_type);
		break;
	default:
		break;
	}

	ibv_ack_async_event(&event);
}

static void ssa_ctrl_send_listen(struct ssa_svc *svc)
{
	int ret;
	struct ssa_listen_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_LISTEN;
	msg.hdr.len = sizeof(msg);
	msg.svc = svc;
	ret = write(svc->sock_downctrl[0], (char *) &msg, sizeof(msg));
	if (ret != sizeof(msg))
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof(msg));
}

static void ssa_ctrl_port(struct ssa_port *port)
{
	struct ssa_svc *svc;
	struct ssa_ctrl_umad_msg msg;
	struct ssa_member_record *member_rec;
	struct ssa_info_record *info_rec;
	int len, ret, parent = 0;

	ssa_log(SSA_LOG_CTRL, "%s receiving MAD\n", port->name);
	len = sizeof msg.umad;
	ret = umad_recv(port->mad_portid, (void *) &msg.umad, &len, 0);
	if (ret < 0) {
		ssa_log_warn(SSA_LOG_CTRL, "receive MAD failure\n");
		return;
	}

	if ((msg.umad.packet.mad_hdr.method & UMAD_METHOD_RESP_MASK) ||
	     msg.umad.umad.status) {
		svc = ssa_svc_from_tid(port, msg.umad.packet.mad_hdr.tid);
		if (msg.umad.packet.mad_hdr.mgmt_class == UMAD_CLASS_SUBN_ADM)
			msg.hdr.type = SSA_SA_MAD;
		else
			msg.hdr.type = SSA_CTRL_MAD;
	} else {
		switch (ntohs(msg.umad.packet.mad_hdr.attr_id)) {
		case SSA_ATTR_INFO_REC:
			parent = 1;
			info_rec = &msg.umad.packet.ssa_mad.info;
			svc = ssa_find_svc(port, ntohll(info_rec->database_id));
			break;
		case SSA_ATTR_MEMBER_REC:
			member_rec = &msg.umad.packet.ssa_mad.member;
			svc = ssa_find_svc(port, ntohll(member_rec->database_id));
			break;
		default:
			svc = NULL;
			break;
		}
		msg.hdr.type = SSA_CTRL_MAD;
	}

	if (!svc) {
		ssa_log_err(SSA_LOG_CTRL, "no matching service for received MAD\n");
		return;
	}

	msg.hdr.len = sizeof msg;
	/* set qkey for possible response */
	msg.umad.umad.addr.qkey = htonl(UMAD_QKEY);
	ret = write(svc->sock_upctrl[0], (void *) &msg, msg.hdr.len);
	if (ret != msg.hdr.len)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, msg.hdr.len);

	if (parent && port->dev->ssa->node_type != SSA_NODE_CONSUMER)
		ssa_ctrl_send_listen(svc);
}

static void ssa_upstream_conn(struct ssa_svc *svc, struct ssa_conn *conn,
			      int gone)
{
	int ret;
	struct ssa_conn_done_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	if (gone)
		msg.hdr.type = SSA_CONN_GONE;
	else
		msg.hdr.type = SSA_CONN_DONE;
	msg.hdr.len = sizeof(msg);
	ssa_conn_msg_init(conn, &msg);
	ret = write(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
	ret = write(svc->sock_adminup[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
}

static int ssa_upstream_svc_client(struct ssa_svc *svc)
{
	int ret, err;
	socklen_t len;

	if (svc->conn_dataup.state != SSA_CONN_CONNECTING) {
		ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			    "Unexpected consumer event in state %d on rsock %d\n",
			    svc->conn_dataup.state, svc->conn_dataup.rsock);
		return 1;
	}

	len = sizeof err;
	ret = rgetsockopt(svc->conn_dataup.rsock, SOL_SOCKET, SO_ERROR,
			  &err, &len);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			    "rgetsockopt rsock %d ERROR %d (%s)\n",
			    svc->conn_dataup.rsock, errno, strerror(errno));
		return ret;
	}
	if (err) {
		errno = err;
		ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			    "async rconnect rsock %d ERROR %d (%s)\n",
			    svc->conn_dataup.rsock, errno, strerror(errno));
		return err;
	}

	memcpy(&svc->conn_dataup.remote_gid, &svc->primary.path.dgid,
	       sizeof(union ibv_gid));
	svc->conn_dataup.state = SSA_CONN_CONNECTED;
	svc->state = SSA_STATE_CONNECTED;

	ssa_log(SSA_LOG_DEFAULT, "rsock %d now connected\n",
		svc->conn_dataup.rsock);

	if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER) {
		ret = riomap(svc->conn_dataup.rsock,
			     (void *) &svc->conn_dataup.prdb_epoch,
			     sizeof svc->conn_dataup.prdb_epoch,
			     PROT_WRITE, 0, 0); 
		if (ret) {
			ssa_log_err(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				    "riomap epoch rsock %d ret %d ERROR %d (%s)\n",
				    svc->conn_dataup.rsock, ret, errno, strerror(errno));
		} else
			svc->conn_dataup.epoch_len = sizeof svc->conn_dataup.prdb_epoch;

		return ret;
	}

	svc->conn_dataup.reconnect_count = 0;
	ssa_upstream_conn(svc, &svc->conn_dataup, 0);
	ssa_set_runtime_counter_time(COUNTER_ID_TIME_LAST_UPSTR_CONN);

	return 0;
}

static int ssa_rsock_enable_keepalive(int rsock, int keepalive)
{
	int val, ret = 0;

	if (keepalive) {
		val = 1;
		ret = rsetsockopt(rsock, SOL_SOCKET, SO_KEEPALIVE,
				  (void *) &val, sizeof(val));
		if (ret)
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"rsetsockopt rsock %d SO_KEEPALIVE ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
		else {
			val = keepalive;
			ret = rsetsockopt(rsock, IPPROTO_TCP, TCP_KEEPIDLE,
					  (void *) &val, sizeof(val));
			if (ret)
				ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
					"rsetsockopt rsock %d TCP_KEEPIDLE ERROR %d (%s)\n",
					rsock, errno, strerror(errno));
		}
	}
	return ret;
}

static int ssa_downstream_svc_server(struct ssa_svc *svc, struct ssa_conn *conn)
{
	struct ssa_conn *conn_listen;
	int fd, val, ret;
	struct sockaddr_ib peer_addr;
	struct ibv_path_data route;
	socklen_t peer_len, route_len;

	if (conn->dbtype == SSA_CONN_SMDB_TYPE)
		conn_listen = &svc->conn_listen_smdb;
	else
		conn_listen = &svc->conn_listen_prdb;
	fd = raccept(conn_listen->rsock, NULL, 0);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return -1;	/* ignore these errors */
		ssa_log_err(SSA_LOG_CTRL,
			    "raccept rsock %d ERROR %d (%s)\n",
			    conn_listen->rsock, errno, strerror(errno));
		return -1;
	}

	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
		"new connection accepted on rsock %d dbtype %d\n",
		fd, conn->dbtype);

	peer_len = sizeof(peer_addr);
	if (!rgetpeername(fd, (struct sockaddr *) &peer_addr, &peer_len)) {
		if (peer_addr.sib_family == AF_IB) {
			ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
					log_data, sizeof log_data, SSA_ADDR_GID,
					(uint8_t *) &peer_addr.sib_addr,
					peer_len);
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"peer GID %s\n", log_data);
		} else {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"rgetpeername fd %d family %d not AF_IB\n",
				fd, peer_addr.sib_family);
			ssa_close_rsocket(fd);
			return -1;
		}
	} else {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rgetpeername rsock %d ERROR %d (%s)\n",
			fd, errno, strerror(errno));
		ssa_close_rsocket(fd);
		return -1;
	}

	if (conn->dbtype == SSA_CONN_SMDB_TYPE &&
	    (update_pending || update_waiting)) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"update pending %d or waiting %d; closing rsock %d\n",
			update_pending, update_waiting, fd);
		ssa_close_rsocket(fd);
		return -1;
	}

	ssa_rsock_enable_keepalive(fd, keepalive);

	val = 1;
	ret = rsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt rsock %d TCP_NODELAY ERROR %d (%s)\n",
			fd, errno, strerror(errno));
		ssa_close_rsocket(fd);
		return -1;
	}
	ret = rfcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl rsock %d ERROR %d (%s)\n",
			fd, errno, strerror(errno));
		ssa_close_rsocket(fd);
		return -1;
	}

	route_len = sizeof(route);
	if (!rgetsockopt(fd, SOL_RDMA, RDMA_ROUTE, &route, &route_len)) {
		conn->remote_lid = ntohs(route.path.dlid);
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"peer LID %u\n", conn->remote_lid);
	} else
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rgetsockopt RDMA_ROUTE rsock %d ERROR %d (%s)\n",
			fd, errno, strerror(errno));

	conn->rsock = fd;
	memcpy(&conn->remote_gid, &peer_addr.sib_addr, sizeof(union ibv_gid));
	conn->state = SSA_CONN_CONNECTED;

	return fd;
}

static int ssa_upstream_initiate_conn(struct ssa_svc *svc, short dport)
{
	struct sockaddr_ib dst_addr;
	int ret, val;

	svc->conn_dataup.rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (svc->conn_dataup.rsock < 0) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	val = 1;
	ret = rsetsockopt(svc->conn_dataup.rsock, SOL_SOCKET, SO_REUSEADDR,
			  &val, sizeof val);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt rsock %d SO_REUSEADDR ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		goto close;
	}

	ssa_rsock_enable_keepalive(svc->conn_dataup.rsock, keepalive);

	ret = rsetsockopt(svc->conn_dataup.rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt rsock %d TCP_NODELAY ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		goto close;
	}

	if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER) {
		ret = rsetsockopt(svc->conn_dataup.rsock, SOL_RDMA,
				  RDMA_IOMAPSIZE, (void *) &val, sizeof(val));
		if (ret) {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"rsetsockopt rsock %d RDMA_IOMAPSIZE ERROR %d (%s)\n",
				svc->conn_dataup.rsock, errno, strerror(errno));
		}
	}

	ret = rfcntl(svc->conn_dataup.rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl rsock %d ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		goto close;
	}

	ret = rsetsockopt(svc->conn_dataup.rsock, SOL_RDMA, RDMA_ROUTE,
			  &svc->primary, sizeof(svc->primary));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt rsock %d RDMA_ROUTE ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		goto close;
	}

	svc->conn_dataup.remote_lid = ntohs(svc->primary.path.dlid);

	dst_addr.sib_family = AF_IB;
	dst_addr.sib_pkey = 0xFFFF;
	dst_addr.sib_flowinfo = 0;
	dst_addr.sib_sid = htonll(((uint64_t) RDMA_PS_TCP << 16) + dport);
	dst_addr.sib_sid_mask = htonll(RDMA_IB_IP_PS_MASK);
	dst_addr.sib_scope_id = 0;
	ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, (uint8_t *) &svc->primary.path.sgid,
			sizeof svc->primary.path.sgid);
	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
		"source GID %s LID %u\n",
		log_data, ntohs(svc->primary.path.slid));
	memcpy(&dst_addr.sib_addr, &svc->primary.path.dgid,
	       sizeof(union ibv_gid));
	ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, (uint8_t *) &dst_addr.sib_addr,
			sizeof dst_addr.sib_addr);
	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
		"dest GID %s LID %u port %u type %s\n",
		log_data, ntohs(svc->primary.path.dlid), dport,
		ssa_node_type_str(svc->primary_type));

	ret = rconnect(svc->conn_dataup.rsock,
		       (const struct sockaddr *) &dst_addr, sizeof(dst_addr));
	if (ret && (errno != EINPROGRESS)) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rconnect rsock %d ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		goto close;
	}

	if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER) {
		svc->conn_dataup.prdb_epoch	= DB_EPOCH_INVALID;
		svc->conn_dataup.epoch		= DB_EPOCH_INVALID;
	}

	svc->conn_dataup.state = SSA_CONN_CONNECTING;
	svc->state = SSA_STATE_CONNECTING;

	if (ret == 0) {
		ret = ssa_upstream_svc_client(svc);
		if (ret && (errno != EINPROGRESS))
			goto close;
	}

	return svc->conn_dataup.rsock;

close:
	ssa_close_rsocket(svc->conn_dataup.rsock);
	svc->conn_dataup.rsock = -1;
	svc->conn_dataup.state = SSA_CONN_IDLE;
	return -1;
}

static int ssa_ctrl_init_fds(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	struct ssa_port *port;
	int d, p, i = 0;

	ssa->nfds = 1;			/* ssa socketpair */
	ssa->nfds += ssa->dev_cnt;	/* async device events */
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer == IBV_LINK_LAYER_INFINIBAND)
				ssa->nfds++;		/* mads */

			ssa->nsfds += port->svc_cnt;	/* service listen */
		}
	}
	ssa->nsfds++;

	ssa->fds = calloc(ssa->nfds + ssa->nsfds,
			  sizeof(*ssa->fds) + sizeof(*ssa->fds_obj));
	if (!ssa->fds)
		return seterr(ENOMEM);

	ssa->fds_obj = (struct ssa_obj *) (&ssa->fds[ssa->nfds + ssa->nsfds]);
	ssa->fds[i].fd = ssa->sock[1];
	ssa->fds[i].events = POLLIN;
	ssa->fds_obj[i++].type = SSA_OBJ_CLASS;
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		ssa->fds[i].fd = dev->verbs->async_fd;
		ssa->fds[i].events = POLLIN;
		ssa->fds_obj[i].type = SSA_OBJ_DEVICE;
		ssa->fds_obj[i++].dev = dev;

		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			ssa->fds[i].fd = umad_get_fd(port->mad_portid);
			ssa->fds[i].events = POLLIN;
			ssa->fds_obj[i].type = SSA_OBJ_PORT;
			ssa->fds_obj[i++].port = port;
		}
	}
	return 0;
}

static void ssa_ctrl_activate_ports(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	struct ssa_port *port;
	int d, p;

	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			ssa_ctrl_update_port(port);
			if (port->state == IBV_PORT_ACTIVE) {
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
				ssa_ctrl_send_event(port, IBV_EVENT_PORT_ACTIVE);
			}
		}
	}
}

int ssa_ctrl_run(struct ssa_class *ssa)
{
	struct ssa_ctrl_msg_buf msg;
	int i, ret;
	struct ssa_svc *conn_svc;

	ssa_log_func(SSA_LOG_CTRL);
	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, ssa->sock);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating socketpair\n");
		return ret;
	}

	ret = ssa_ctrl_init_fds(ssa);
	if (ret)
		goto err;

	ssa_ctrl_activate_ports(ssa);

	for (;;) {
		ret = poll(ssa->fds, ssa->nfds, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}

		for (i = 0; i < ssa->nfds; i++) {
			if (!ssa->fds[i].revents)
				continue;

			ssa->fds[i].revents = 0;
			switch (ssa->fds_obj[i].type) {
			case SSA_OBJ_CLASS:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"class event on fd %d\n", ssa->fds[i]);

				ret = read(ssa->sock[1], (char *) &msg,
					   sizeof msg.hdr);
				if (ret != sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d header bytes read\n",
						    ret, sizeof msg.hdr);
				if (msg.hdr.len > sizeof msg.hdr) {
					ret = read(ssa->sock[1],
						   (char *) &msg.hdr.data,
						   msg.hdr.len - sizeof msg.hdr);
					if (ret != msg.hdr.len - sizeof msg.hdr)
						ssa_log_err(SSA_LOG_CTRL,
							    "%d out of %d additional bytes read\n",
							    ret,
							    msg.hdr.len - sizeof msg.hdr);
				}

				switch (msg.hdr.type) {
				case SSA_CONN_REQ:
					conn_svc = msg.data.svc;
					ret = write(conn_svc->sock_upctrl[0],
						    (char *) &msg,
						    sizeof(struct ssa_conn_req_msg));
					if (ret != sizeof(struct ssa_conn_req_msg))
						ssa_log_err(SSA_LOG_CTRL,
							    "%d out of %d bytes written to upstream\n",
							    ret,
							    sizeof(struct ssa_conn_req_msg));
					break;
				case SSA_CTRL_EXIT:
					goto out;
				default:
					ssa_log_warn(SSA_LOG_CTRL,
						     "ignoring unexpected msg type %d\n",
						     msg.hdr.type);
					break;
				}
				break;
			case SSA_OBJ_DEVICE:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"device event on fd %d\n", ssa->fds[i].fd);
				ssa_ctrl_device(ssa->fds_obj[i].dev);
				break;
			case SSA_OBJ_PORT:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"port event on fd %d\n", ssa->fds[i].fd);
				ssa_ctrl_port(ssa->fds_obj[i].port);
				break;
			}
		}
	}
out:
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	ret = write(ssa->sock[1], (char *) &msg, sizeof msg.hdr);
	if (ret != sizeof msg.hdr)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg.hdr);
	free(ssa->fds);
	return 0;

err:
	close(ssa->sock[0]);
	close(ssa->sock[1]);
	return ret;
}

void ssa_ctrl_conn(struct ssa_class *ssa, struct ssa_svc *svc)
{
	int ret;
	struct ssa_conn_req_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_CONN_REQ;
	msg.hdr.len = sizeof msg;
	msg.svc = svc;
	ret = write(ssa->sock[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
}

void ssa_ctrl_stop(struct ssa_class *ssa)
{
	int ret;
	struct ssa_ctrl_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	if (ssa->sock[0] >= 0) {
		msg.len = sizeof msg;
		msg.type = SSA_CTRL_EXIT;
		ret = write(ssa->sock[0], (char *) &msg, sizeof msg);
		if (ret != sizeof msg)
			ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
				    ret, sizeof msg);
		else {
			ret = read(ssa->sock[0], (char *) &msg, sizeof msg);
			if (ret != sizeof msg)
				ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes read\n",
					    ret, sizeof msg);
		}
		close(ssa->sock[0]);
	}

	if (ssa->sock[1] >= 0)
		close(ssa->sock[1]);
}

struct ssa_svc *ssa_start_svc(struct ssa_port *port, uint64_t database_id,
			      size_t svc_size,
			      int (*process_msg)(struct ssa_svc *svc,
					         struct ssa_ctrl_msg_buf *msg),
			      int (*init_svc)(struct ssa_svc *svc),
			      void (*destroy_svc)(struct ssa_svc *svc))
{
	struct ssa_svc *svc, **list;
	struct ssa_ctrl_msg msg;
	int ret;

	if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
		return NULL;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s:%llu\n",
		port->name, database_id);
	list = realloc(port->svc, (port->svc_cnt + 1) * sizeof(svc));
	if (!list)
		return NULL;

	port->svc = list;
	svc = calloc(1, svc_size);
	if (!svc)
		return NULL;

	if (init_svc(svc))
		return NULL;

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_upctrl);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating upstream/ctrl socketpair\n");
		goto err1;
	}

	if (port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		svc->sock_upmain[0] = -1;
		svc->sock_upmain[1] = -1;
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_downctrl);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating downstream/ctrl socketpair\n");
			goto err2;
		}
	} else {
		svc->sock_downctrl[0] = -1;
		svc->sock_downctrl[1] = -1;
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_upmain);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating upstream/main socketpair\n");
			goto err2;
		}
	}

	if (port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_accessup);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating access/upstream socketpair\n");
			goto err3;
		}
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_accessdown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating access/downstream socketpair\n");
			goto err4;
		}
	} else {
		svc->sock_accessup[0] = -1;
		svc->sock_accessup[1] = -1;
		svc->sock_accessdown[0] = -1;
		svc->sock_accessdown[1] = -1;
	}

	if (port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_updown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating upstream/downstream socketpair\n");
			goto err5;
		}
	} else {
		svc->sock_updown[0] = -1;
		svc->sock_updown[1] = -1;
	}

	if (port->dev->ssa->node_type & SSA_NODE_CORE) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_extractdown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating extract/downstream socketpair\n");
			goto err6;
		}
	} else {
		svc->sock_extractdown[0] = -1;
		svc->sock_extractdown[1] = -1;
	}

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_adminup);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL,
			    "creating admin/upstream socketpair\n");
		goto err7;
	}

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_admindown);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL,
			    "creating admin/downstream socketpair\n");
		goto err8;
	}

	svc->index = port->svc_cnt;
	svc->port = port;
	snprintf(svc->name, sizeof svc->name, "%s:%llu", port->name,
		 (unsigned long long) database_id);
	svc->database_id = database_id;
	svc->conn_listen_smdb.rsock = -1;
	svc->conn_listen_smdb.type = SSA_CONN_TYPE_LISTEN;
	svc->conn_listen_smdb.dbtype = SSA_CONN_SMDB_TYPE;
	svc->conn_listen_smdb.state = SSA_CONN_IDLE;
	svc->conn_listen_smdb.phase = SSA_DB_IDLE;
	svc->conn_listen_prdb.rsock = -1;
	svc->conn_listen_prdb.type = SSA_CONN_TYPE_LISTEN;
	svc->conn_listen_prdb.dbtype = SSA_CONN_PRDB_TYPE;
	svc->conn_listen_prdb.state = SSA_CONN_IDLE;
	svc->conn_listen_prdb.phase = SSA_DB_IDLE;
	ssa_init_ssa_conn(&svc->conn_dataup, SSA_CONN_TYPE_UPSTREAM,
			  SSA_CONN_NODB_TYPE);
	svc->state = SSA_STATE_IDLE;
	svc->process_msg = process_msg;
	//pthread_mutex_init(&svc->lock, NULL);

	svc->join_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (svc->join_timer_fd < 0) {
		ssa_log_err(SSA_LOG_CTRL, "timerfd_create %d (%s)\n",
			    errno, strerror(errno));
		errno = ret;
		goto err9;
	}

	ret = pthread_create(&svc->upstream, NULL, ssa_upstream_handler, svc);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating upstream thread\n");
		errno = ret;
		goto err10;
	}

	ret = read(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
	if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
		ssa_log_err(SSA_LOG_CTRL, "with upstream thread\n");
		goto err11;
	}

	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		ret = pthread_create(&svc->downstream, NULL,
				     ssa_downstream_handler, svc);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL, "creating downstream thread\n");
			errno = ret;
			goto err11;
		}

		ret = read(svc->sock_downctrl[0], (char *) &msg, sizeof msg);
		if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
			ssa_log_err(SSA_LOG_CTRL, "with downstream thread\n");
			goto err12;
		}
	}


	port->svc[port->svc_cnt++] = svc;
	return svc;
err12:
	pthread_join(svc->downstream, NULL);
err11:
	pthread_join(svc->upstream, NULL);
err10:
	close(svc->join_timer_fd);
	svc->join_timer_fd = -1;
err9:
	close(svc->sock_admindown[0]);
	close(svc->sock_admindown[1]);
err8:
	close(svc->sock_adminup[0]);
	close(svc->sock_adminup[1]);
err7:
	if (svc->port->dev->ssa->node_type & SSA_NODE_CORE) {
		close(svc->sock_extractdown[0]);
		close(svc->sock_extractdown[1]);
	}
err6:
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		close(svc->sock_updown[0]);
		close(svc->sock_updown[1]);
	}
err5:
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessdown[0]);
		close(svc->sock_accessdown[1]);
	}
err4:
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessup[0]);
		close(svc->sock_accessup[1]);
	}
err3:
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		close(svc->sock_downctrl[0]);
		close(svc->sock_downctrl[1]);
	} else {
		close(svc->sock_upmain[0]);
		close(svc->sock_upmain[1]);
	}
err2:
	close(svc->sock_upctrl[0]);
	close(svc->sock_upctrl[1]);
err1:
	destroy_svc(svc);
	free(svc);
	return NULL;
}

#ifdef ACCESS
static int ssa_access_thread_pool_init()
{
	int ret;
	GError *g_error = NULL;

	atomic_set(&access_context.num_tasks, 0);

	ret = pthread_cond_init(&access_context.th_pool_cond, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to initialize al thread pool condition variable\n");
		goto err1;
	}

	ret = pthread_mutex_init(&access_context.th_pool_mtx, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to initialize al thread pool mutex\n");
		goto err2;
	}

	access_context.num_workers = ssa_sysinfo.nprocs > 3 ? ssa_sysinfo.nprocs - 3 : 1;
	access_context.num_workers = min(access_context.num_workers, MAX_ACCESS_POOL_WORKERS_NUM);
	ssa_log(SSA_LOG_DEFAULT, "Number of access workers %d\n",
		access_context.num_workers);

	access_context.g_th_pool = g_thread_pool_new((GFunc) g_al_callback,
						     NULL,
						     access_context.num_workers,
						     1, &g_error);
	if (!access_context.g_th_pool) {
		if (g_error != NULL) {
			ssa_log_err(SSA_LOG_CTRL,
				    "Glib thread pool initialization error: %s\n",
				    g_error->message);
			g_error_free(g_error);
		} else {
			ssa_log_err(SSA_LOG_CTRL,
				    "Glib thread pool initialization error\n");
		}
		ret = -1;
		goto err3;
	}
	return 0;

err3:
	pthread_mutex_destroy(&access_context.th_pool_mtx);
err2:
	pthread_cond_destroy(&access_context.th_pool_cond);
err1:
	return ret;
}

static void ssa_access_thread_pool_destroy()
{
	int unprocessed;

	if (access_context.g_th_pool != NULL) {
		unprocessed = g_thread_pool_unprocessed(access_context.g_th_pool);
		if (unprocessed)
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				"%d PR calculations still unprocessed\n",
				unprocessed);
		g_thread_pool_free(access_context.g_th_pool, TRUE, TRUE);
	}

	pthread_mutex_destroy(&access_context.th_pool_mtx);
	pthread_cond_destroy(&access_context.th_pool_cond);
}

static void ssa_access_wait_for_tasks_completion()
{
	if (access_context.num_workers > 1) {
		pthread_mutex_lock(&access_context.th_pool_mtx);
		while (atomic_get(&access_context.num_tasks) > 0)
			pthread_cond_wait(&access_context.th_pool_cond,
					  &access_context.th_pool_mtx);
		pthread_mutex_unlock(&access_context.th_pool_mtx);
	}
}

static void ssa_access_process_task(struct ssa_access_task *task)
{
	GError *g_error = NULL;

	if (access_context.num_workers > 1) {
		g_thread_pool_push(access_context.g_th_pool, task, &g_error);
		if (g_error != NULL) {
			ssa_log_err(SSA_LOG_CTRL,
				    "failed to push a task to access thread pool: %s\n",
				    g_error->message);
			g_error_free(g_error);
		}
	} else
		g_al_callback(task, NULL);
}

static int ssa_db_update_queue_init(struct ssa_db_update_queue *p_queue)
{
	int ret;

	ret = pthread_mutex_init(&p_queue->cond_lock, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to initialize DB queue condition lock\n");
		return ret;
	}

	ret = pthread_cond_init(&p_queue->cond_var, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT,
			    "unable to initialize DB queue condition variable\n");
		pthread_mutex_destroy(&p_queue->cond_lock);
		return ret;
	}

	ret = pthread_mutex_init(&p_queue->lock, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_DEFAULT, "unable to initialize DB queue lock\n");
		pthread_cond_destroy(&p_queue->cond_var);
		pthread_mutex_destroy(&p_queue->lock);
		return ret;
	}

	DListInit(&p_queue->list);
	return ret;
}

static void ssa_db_update_queue_destroy(struct ssa_db_update_queue *p_queue)
{
	if (access_prdb_handler) {
		pthread_cancel(*access_prdb_handler);
		pthread_join(*access_prdb_handler, NULL);
	}

	pthread_mutex_destroy(&p_queue->lock);
	pthread_cond_destroy(&p_queue->cond_var);
	pthread_mutex_destroy(&p_queue->cond_lock);
}
#endif

int ssa_start_access(struct ssa_class *ssa)
{
	struct ssa_ctrl_msg msg;
	int ret;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);

	sock_accessctrl[0] = -1;
	sock_accessctrl[1] = -1;
	sock_accessextract[0] = -1;
	sock_accessextract[1] = -1;

	if (!(ssa->node_type & SSA_NODE_ACCESS))
		return 0;

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sock_accessctrl);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating access layer socketpair\n");
		goto err1;
	}

	if (ssa->node_type & SSA_NODE_CORE) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sock_accessextract);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating extract/access socketpair\n");
			goto err2;
		}
	}

#ifdef ACCESS
	access_context.context = ssa_pr_create_context();
	if (!access_context.context) {
		ssa_log_err(SSA_LOG_CTRL,
			    "unable to create access layer context\n");
		goto err3;
	}

	if (ssa_db_update_queue_init(&update_queue)) {
		ssa_log_err(SSA_LOG_CTRL,
			    "unable to create access layer prdb updates queue\n");
		goto err4;
	}

	ret = ssa_access_thread_pool_init();
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating access thread pool\n");
		errno = ret;
		goto err5;
	}
#endif

	access_thread = calloc(1, sizeof(*access_thread));
	if (access_thread == NULL) {
		ssa_log_err(SSA_LOG_CTRL, "allocating access thread memory\n");
		goto err6;
	}

	ret = pthread_create(access_thread, NULL, ssa_access_handler, ssa);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating access thread\n");
		errno = ret;
		goto err6;
	}

	ret = read(sock_accessctrl[0], (char *) &msg, sizeof msg);
	if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
		ssa_log_err(SSA_LOG_CTRL, "with access thread\n");
		goto err7;
	}
#ifdef ACCESS
	access_prdb_handler = calloc(1, sizeof(*access_prdb_handler));
	if (access_prdb_handler == NULL) {
		ssa_log_err(SSA_LOG_CTRL,
			    "allocating access prdb handler thread memory\n");
		goto err8;
	}
	ret = pthread_create(access_prdb_handler, NULL, ssa_access_prdb_handler, NULL);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating access prdb handler thread\n");
		errno = ret;
		goto err8;
	}
#endif
	return 0;

#ifdef ACCESS
err8:
	free(access_prdb_handler);
	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	ret = write(sock_accessctrl[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
#endif
err7:
	pthread_join(*access_thread, NULL);
err6:
	free(access_thread);
#ifdef ACCESS
	ssa_access_thread_pool_destroy();
err5:
	ssa_db_update_queue_destroy(&update_queue);
err4:
	if (access_context.context) {
		ssa_pr_destroy_context(access_context.context);
		access_context.context = NULL;
		access_context.smdb = NULL;
	}
err3:
#endif
	if (sock_accessextract[0] >= 0)
		close(sock_accessextract[0]);
	if (sock_accessextract[1] >= 0)
		close(sock_accessextract[1]);
err2:
	close(sock_accessctrl[0]);
	close(sock_accessctrl[1]);
err1:
	return 1;
}

void ssa_stop_access(struct ssa_class *ssa)
{
	int ret;
	struct ssa_ctrl_msg msg;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);

	if (!(ssa->node_type & SSA_NODE_ACCESS))
		return;

	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	ret = write(sock_accessctrl[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
	if (access_thread) {
		pthread_join(*access_thread, NULL);
		free(access_thread);
	}

#ifdef ACCESS
	ssa_access_thread_pool_destroy();
	ssa_db_update_queue_destroy(&update_queue);

	if (access_context.context) {
		ssa_pr_destroy_context(access_context.context);
		access_context.context = NULL;
	}
	if (access_context.smdb != smdb)
		ssa_db_destroy(access_context.smdb);
	access_context.smdb = NULL;
#endif

	if (ssa->node_type & SSA_NODE_CORE) {
		close(sock_accessextract[0]);
		close(sock_accessextract[1]);
	}
	close(sock_accessctrl[0]);
	close(sock_accessctrl[1]);
#ifdef ACCESS
	free(access_prdb_handler);
#endif
}

static int set_bit(int nr, void *method_mask)
{
	long mask, *addr = method_mask;
	int retval;

	addr += nr / (8 * sizeof(long));
	mask = 1L << (nr % (8 * sizeof(long)));
	retval = (mask & *addr) != 0;
	*addr |= mask;
	return retval;
}

static int ssa_open_port(struct ssa_port *port, struct ssa_device *dev,
			 uint8_t port_num, uint8_t link_layer)
{
	long methods[16 / sizeof(long)];
	int ret;

	port->dev = dev;
	port->port_num = port_num;
	port->link_layer = link_layer;
	snprintf(port->name, sizeof port->name, "%s:%d", dev->name, port_num);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
	pthread_mutex_init(&port->lock, NULL);
#ifdef ACM
	DListInit(&port->ep_list);
	acm_init_dest(&port->sa_dest, ACM_ADDRESS_LID, NULL, 0);
#endif

	port->mad_portid = umad_open_port(dev->name, port->port_num);
	if (port->mad_portid < 0) {
		ssa_log_err(SSA_LOG_CTRL, "unable to open MAD port %s\n",
			    port->name);
		return -1;
	}

	ret = fcntl(umad_get_fd(port->mad_portid), F_SETFL, O_NONBLOCK);
	if (ret)
		ssa_log_warn(SSA_LOG_CTRL, "MAD fd is blocking\n");

	/* Only SSA Set method is unsolicited currently */
	memset(methods, 0, sizeof methods);
	set_bit(UMAD_METHOD_SET, &methods);

	port->mad_agentid = umad_register(port->mad_portid, SSA_CLASS,
					  SSA_CLASS_VERSION, 0, methods);
	if (port->mad_agentid < 0) {
		ssa_log_err(SSA_LOG_CTRL,
			    "unable to register SSA class on port %s\n",
			    port->name);
		ssa_log(SSA_LOG_CTRL,
			"Check that another SSA component is not already running\n");
		goto err;
	}

	/* Only registering for solicited SA MADs */
	port->sa_agentid = umad_register(port->mad_portid, UMAD_CLASS_SUBN_ADM,
					 UMAD_SA_CLASS_VERSION, 0, NULL);
	if (port->sa_agentid < 0) {
		ssa_log_err(SSA_LOG_CTRL, "unable to register SA class on port %s\n",
			    port->name);
		goto err2;
	}

	return 0;
err2:
	umad_unregister(port->mad_portid, port->mad_agentid);
err:
	umad_close_port(port->mad_portid);
	return -1;
}

static int ssa_open_dev(struct ssa_device *dev, struct ssa_class *ssa,
			struct ibv_device *ibdev)
{
	struct ssa_port *port;
	struct ibv_device_attr attr;
	struct ibv_port_attr port_attr;
	int i, ret, j;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", ibdev->name);

	if (ssa->node_type != SSA_NODE_CONSUMER) {
		if (strncmp(ibdev->name, "mthca", 5) == 0)
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "mthca doesn't support keepalives needed by SSA\n");
		if (keepalive == 0)
			ssa_log_warn(SSA_LOG_DEFAULT,
				     "keepalives disabled but SSA needs keepalives\n");
	}

	dev->verbs = ibv_open_device(ibdev);
	if (dev->verbs == NULL) {
		ssa_log_err(SSA_LOG_CTRL, "opening device %s\n", ibdev->name);
		return -1;
	}

	ret = ibv_query_device(dev->verbs, &attr);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "ibv_query_device (%s) %d\n",
			    ibdev->name, ret);
		goto err1;
	}

	ret = fcntl(dev->verbs->async_fd, F_SETFL, O_NONBLOCK);
	if (ret)
		ssa_log_warn(SSA_LOG_CTRL, "event fd is blocking\n");

	dev->port = (struct ssa_port *) calloc(attr.phys_port_cnt, ssa->port_size);
	if (!dev->port)
		goto err1;

	dev->ssa = ssa;
	dev->guid = ibv_get_device_guid(ibdev);
	ssa_log(SSA_LOG_CTRL, "Node GUID 0x%" PRIx64 "\n", ntohll(dev->guid));
	snprintf(dev->name, sizeof dev->name, "%s", ibdev->name);
	if (ibv_read_sysfs_file(ibdev->ibdev_path, "node_desc",
				dev->node_desc, sizeof dev->node_desc) < 0)
		ssa_log(SSA_LOG_DEFAULT,
			"Reading %s/node_desc file failed\n", ibdev->ibdev_path);
	dev->port_cnt = attr.phys_port_cnt;
	dev->port_size = ssa->port_size;

#ifdef ACM
	dev->pd = ibv_alloc_pd(dev->verbs);
	if (!dev->pd) {
		ssa_log_err(0, "unable to allocate PD\n");
		goto err2;
	}

	dev->channel = ibv_create_comp_channel(dev->verbs);
	if (!dev->channel) {
		ssa_log_err(0, "unable to create comp channel\n");
		goto err3;
	}
#endif

	for (i = 1; i <= dev->port_cnt; i++) {
		port = ssa_dev_port(dev, i);
		ret = ibv_query_port(dev->verbs, i, &port_attr);
		if (ret == 0) {
			if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
				ret = ssa_open_port(port, dev, i, port_attr.link_layer);
				if (ret < 0) {
					for (j = 1; j < i; j++) {
						ret = ibv_query_port(dev->verbs, j, &port_attr);
						if (ret == 0 &&
						    port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND)
							ssa_close_port(ssa_dev_port(dev, j));
					}
					goto err3;
				}
			} else {
				ssa_log(SSA_LOG_CTRL,
					"%s:%d link layer %d is not IB\n",
					dev->name, i, port_attr.link_layer);
				port->link_layer = port_attr.link_layer;
			}
		} else {
			ssa_log_err(0, "ibv_query_port (%s:%d) %d\n",
				    dev->name, i, ret);
			port->link_layer = IBV_LINK_LAYER_UNSPECIFIED;
		}
	}

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s opened\n", dev->name);
	return 0;

err3:
#ifdef ACM
	ibv_dealloc_pd(dev->pd);
err2:
	free(dev->port);
#endif
err1:
	ibv_close_device(dev->verbs);
	dev->verbs = NULL;
	seterr(ENOMEM);
	return -1;
}

int ssa_open_devices(struct ssa_class *ssa)
{
	struct ibv_device **ibdev;
	int i, j, ret = 0;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	ibdev = ibv_get_device_list(&ssa->dev_cnt);
	if (!ibdev) {
		ssa_log_err(SSA_LOG_CTRL, "unable to get device list ERROR %d (%s)\n",
			    errno, strerror(errno));
		return -1;
	}

	ssa->dev = (struct ssa_device *) calloc(ssa->dev_cnt, ssa->dev_size);
	if (!ssa->dev) {
		ssa_log_err(SSA_LOG_CTRL, "allocating memory for devices\n");
		ret = seterr(ENOMEM);
		goto free;
	}

	if (!ssa->dev_cnt) {
		ssa_log_err(SSA_LOG_CTRL, "no RDMA device\n");
		ret = seterr(ENODEV);
		goto free;
	}

	j = 0;
	for (i = 0; i < ssa->dev_cnt; i++) {
		if (ibdev[i]->transport_type != IBV_TRANSPORT_IB) {
			ssa_log(SSA_LOG_CTRL,
				"device %d transport type %d is not IB\n",
				i, ibdev[i]->transport_type);
			continue;
		}
		if (ibdev[i]->node_type != IBV_NODE_CA) {
			ssa_log(SSA_LOG_CTRL,
				"device %d node type %d is not CA\n",
				i, ibdev[i]->node_type);
			continue;
		}
		ret = ssa_open_dev(ssa_dev(ssa, i), ssa, ibdev[i]);
		if (ret)
			goto free;
		j++;
	}

	if (j == 0) {
		ssa_log_err(SSA_LOG_CTRL, "no IB device\n");
		ret = seterr(ENODEV);
	}

free:
	ibv_free_device_list(ibdev);
	return ret;
}

static void ssa_stop_svc(struct ssa_svc *svc)
{
	int i, ret;
	struct ssa_ctrl_msg msg;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	ret = write(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL,
			    "%d out of %d bytes written to upstream\n",
			    ret, sizeof msg);
	pthread_join(svc->upstream, NULL);
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		ret = write(svc->sock_downctrl[0], (char *) &msg, sizeof msg);
		if (ret != sizeof msg)
			ssa_log_err(SSA_LOG_CTRL,
				    "%d out of %d bytes written to downstream\n",
				    ret, sizeof msg);
		pthread_join(svc->downstream, NULL);
	}

	svc->port->svc[svc->index] = NULL;
	if (svc->conn_listen_smdb.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_listen_smdb);
	if (svc->conn_listen_prdb.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_listen_prdb);
	if (svc->port->dev->ssa->node_type & SSA_NODE_CORE) {
		close(svc->sock_extractdown[0]);
		close(svc->sock_extractdown[1]);
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		close(svc->sock_updown[0]);
		close(svc->sock_updown[1]);
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessdown[0]);
		close(svc->sock_accessdown[1]);
		close(svc->sock_accessup[0]);
		close(svc->sock_accessup[1]);
	}

	close(svc->sock_admindown[0]);
	close(svc->sock_admindown[1]);
	close(svc->sock_adminup[0]);
	close(svc->sock_adminup[1]);

	if (svc->conn_dataup.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_dataup);
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		for (i = 0; i < FD_SETSIZE; i++) {
			if (svc->fd_to_conn[i] &&
			    svc->fd_to_conn[i]->rsock >= 0) {
				ssa_close_ssa_conn(svc->fd_to_conn[i]);
				svc->fd_to_conn[i] = NULL;
			}
		}
	}
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		close(svc->sock_downctrl[0]);
		close(svc->sock_downctrl[1]);
	}
	close(svc->sock_upctrl[0]);
	close(svc->sock_upctrl[1]);
	if (svc->join_timer_fd >= 0)
		close(svc->join_timer_fd);
	free(svc);
}

static void ssa_close_port(struct ssa_port *port)
{
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
	while (port->svc_cnt)
		ssa_stop_svc(port->svc[--port->svc_cnt]);
	if (port->svc)
		free(port->svc);

	if (port->sa_agentid >= 0)
		umad_unregister(port->mad_portid, port->sa_agentid);
	if (port->mad_agentid >= 0)
		umad_unregister(port->mad_portid, port->mad_agentid);
	if (port->mad_portid >= 0)
		umad_close_port(port->mad_portid);
	pthread_mutex_destroy(&port->lock);
}

void ssa_close_devices(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	struct ssa_port *port;
	int d, p;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			ssa_close_port(port);
		}

		if (dev->verbs != NULL)
			ibv_close_device(dev->verbs);
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s closed\n", dev->name);
		free(dev->port);
	}
	free(ssa->dev);
	ssa->dev_cnt = 0;

	/*
	 * In case of CORE node, smdb is destroyed in core_destroy()
	 * call via its wrapper object destroy method.
	 */
	if (smdb && !(ssa->node_type & SSA_NODE_CORE)) {
		ssa_db_destroy(smdb);
		smdb = NULL;
	}
}

/*
 * Return value:
 * 0 - success
 * 1 - lock_file is locked
 * < 0 - error
 */
int ssa_open_lock_file(char *lock_file, char *msg, int n)
{
	char buf[16];
	int ret;
	int result = 0;
	pid_t pid, ppid;

	pid = getpid();
	ppid = getppid();

	lock_fd = open(lock_file, O_RDWR | O_CREAT, 0640);
	if (lock_fd < 0) {
		result = lock_fd;
		goto out;
	}

	if (lockf(lock_fd, F_TLOCK, 0)) {
		ssa_close_lock_file();
		if (errno == EACCES || errno == EAGAIN)
			result = 1;
		else
			result = -1;
		goto out;
	}

	ret = snprintf(buf, sizeof buf, "%d\n", getpid());
	if (ret <= 0) {
		result = -1;
		goto out;
	}
	ret = write(lock_fd, buf, strlen(buf));
	if (ret <= 0) {
		result = -1;
		goto out;
	}
out:
	if (result) {
		if (result == 1)
			snprintf(msg, n,
				 "Another instance of %s is already running. "
				 "Lock file: %s Our PID %d PPID %d",
				 program_invocation_short_name, lock_file,
				 pid, ppid);
		else
			snprintf(msg, n, "Could not open lock file. "
				 "Lock file: %s ERROR %d (%s) Our PID %d PPID %d",
				 lock_file, errno, strerror(errno),
				 pid, ppid);
	}
	return result;
}

void ssa_close_lock_file()
{
	if (lock_fd >= 0) {
		close(lock_fd);
		lock_fd = -1;
	}
}

void ssa_daemonize(void)
{
	pid_t pid, sid;

	pid = fork();
	if (pid)
		exit(pid < 0);

	sid = setsid();
	if (sid < 0)
		exit(1);

	if (chdir("/"))
		exit(1);

	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

int ssa_init(struct ssa_class *ssa, uint8_t node_type, size_t dev_size,
	     size_t port_size)
{
	int ret;
#if (RCLOSE_THREAD_POOL_WORKERS_NUM > 0)
	GError *g_error = NULL;
#endif /* RCLOSE_THREAD_POOL_WORKERS_NUM > 0 */

	memset(ssa, 0, sizeof *ssa);
	ssa->sock[0] = ssa->sock[1] = -1;
	ssa->node_type = node_type;
	ssa->dev_size = dev_size;
	ssa->port_size = port_size;
	ret = umad_init();
	if (ret)
		return ret;

	ssa_init_runtime_statistics();
	/*
	 * g_thread_init is not needed to be called starting with Glib 2.32
	 */
#if (!GLIB_CHECK_VERSION(2, 32, 0))
	g_thread_init(NULL);
#endif
#if (RCLOSE_THREAD_POOL_WORKERS_NUM > 0)
	thpool_rclose = g_thread_pool_new((GFunc) g_rclose_callback, NULL,
					  RCLOSE_THREAD_POOL_WORKERS_NUM, TRUE,
					  &g_error);
	if (!thpool_rclose) {
		if (g_error != NULL) {
			ssa_log_err(SSA_LOG_CTRL,
				    "Glib thread pool initialization error: %s\n",
				    g_error->message);
			g_error_free(g_error);
		} else {
			ssa_log_err(SSA_LOG_CTRL,
				    "Glib thread pool initialization error\n");
		}
		umad_done();
		return -1;
	}

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
		"rclose thread pool number of workers %d\n",
		RCLOSE_THREAD_POOL_WORKERS_NUM);
#endif /* RCLOSE_THREAD_POOL_WORKERS_NUM > 0 */
	ssa_get_sysinfo();
	ssa_log_sysinfo();

	return 0;
}

void ssa_cleanup(struct ssa_class *ssa)
{
	umad_done();

#if (RCLOSE_THREAD_POOL_WORKERS_NUM > 0)
	if (thpool_rclose != NULL) {
		int rclose_unprocessed;

		rclose_unprocessed = g_thread_pool_unprocessed(thpool_rclose);
		if (rclose_unprocessed)
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				"%d rsockets still waiting for rclose completion\n",
				rclose_unprocessed);
		ssa_log(SSA_LOG_DEFAULT,
			"closing opened rsockets. this may take a while\n");
		g_thread_pool_free(thpool_rclose, FALSE, TRUE);
		ssa_log(SSA_LOG_DEFAULT, "all rsockets are now closed\n");
	}
#endif /* RCLOSE_THREAD_POOL_WORKERS_NUM > 0 */
}

static int ssa_admin_listen(struct ssa_class *ssa, short port)
{
	struct sockaddr_ib src_addr;
	struct ssa_svc *svc = NULL;
	struct ssa_device *ssa_device;
	struct ssa_port *ssa_port;
	int rsock = -1;
	int ret, val, d, p, s;

	for (d = 0; d < ssa->dev_cnt && !svc; d++) {
		ssa_device = ssa_dev(ssa, d);
		for (p = 1; p <= ssa_device->port_cnt && !svc; p++) {
			ssa_port = ssa_dev_port(ssa_device, p);
			if (ssa_port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < ssa_port->svc_cnt && !svc; s++) {
				svc = ssa_port->svc[s];
			}
		}
	}

	if (!svc) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"admin can't find acceptable service\n");
		goto err;
	}

	rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (rsock < 0) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}

	val = 1;
	ret = rsetsockopt(rsock, SOL_SOCKET, SO_REUSEADDR,
			  &val, sizeof val);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt SO_REUSEADDR ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), rsock);
		goto err;
	}

	ret = rsetsockopt(rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt TCP_NODELAY ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), rsock);
		goto err;
	}

	src_addr.sib_family = AF_IB;
	src_addr.sib_pkey = 0xFFFF;
	src_addr.sib_flowinfo = 0;
	src_addr.sib_sid = htonll(((uint64_t) RDMA_PS_TCP << 16) + port);
	src_addr.sib_sid_mask = htonll(RDMA_IB_IP_PS_MASK | RDMA_IB_IP_PORT_MASK);
	src_addr.sib_scope_id = 0;
	memcpy(&src_addr.sib_addr, &svc->port->gid, 16);

	ret = rfcntl(rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), rsock);
		goto err;
	}

	ret = rbind(rsock, (const struct sockaddr *) &src_addr,
		    sizeof(src_addr));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rbind ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), rsock);
		goto err;
	}
	ret = rlisten(rsock, 1);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rlisten ERROR %d (%s) on rsock %d\n",
			errno, strerror(errno), rsock);
		goto err;
	}

	return rsock;

err:
	if (rsock >= 0)
		rclose(rsock);
	return -1;
}

static int ssa_admin_verify_message(struct ssa_admin_msg *admin_request)
{
	if (admin_request->hdr.version != SSA_ADMIN_PROTOCOL_VERSION) {
		ssa_log_err(SSA_LOG_CTRL,
			    "received SSA admin message with %d "
			    "version instead of %d \n",
			    admin_request->hdr.version, SSA_ADMIN_PROTOCOL_VERSION);
		return 0;
	}

	if (admin_request->hdr.method != SSA_ADMIN_METHOD_GET) {
		ssa_log_err(SSA_LOG_CTRL,
			    "received SSA admin message with %d "
			    "method specified instead of %d\n",
			    admin_request->hdr.method, SSA_ADMIN_METHOD_GET);
		return 0;
	}

	return 1;
}

struct ssa_admin_handler_context {
	struct ssa_class *ssa;;
	GHashTable *connections_hash;
	GHashTable *svcs_hash;
};

static void ssa_destroy_connection_info(gpointer data)
{
	free(data);
}

static struct ssa_admin_msg *ssa_admin_handle_counter_message(struct ssa_admin_msg *admin_request)
{
	int i, n;
	struct timeval epoch;
	struct ssa_admin_msg *response;
	struct ssa_admin_counter *counter_msg;

	response = (struct ssa_admin_msg *) malloc(sizeof(*response));
	if (!response) {
		ssa_log_err(SSA_LOG_CTRL, "admin response allocation failed\n");
		return NULL;
	}

	response->hdr = admin_request->hdr;
	response->hdr.status = SSA_ADMIN_STATUS_SUCCESS;
	response->hdr.method = SSA_ADMIN_METHOD_RESP;
	response->hdr.len = htons(sizeof(*response));

	counter_msg = (struct ssa_admin_counter *) &response->data.counter;

	n = min(COUNTER_ID_LAST, ntohs(admin_request->data.counter.n));

	for (i = 0; i < n; ++i) {
		counter_msg->vals[i] = htonll((uint64_t) ssa_get_runtime_counter(i));
	}

	ssa_get_runtime_counter_time(COUNTER_ID_NODE_START_TIME, &epoch);

	counter_msg->n = htons(COUNTER_ID_LAST);
	counter_msg->epoch_tv_sec = htonll((uint64_t) epoch.tv_sec);
	counter_msg->epoch_tv_usec = htonll((uint64_t) epoch.tv_usec);

	return response;
};

static struct ssa_admin_msg *ssa_admin_handle_node_info(struct ssa_admin_msg *admin_request,
							struct ssa_admin_handler_context *context)
{
	int i, n;
	struct ssa_admin_msg *response;
	struct ssa_admin_node_info *nodeinfo_msg = (struct ssa_admin_node_info *) &admin_request->data.counter;
	struct ssa_admin_connection_info *connections;
	GHashTableIter iter;
	gpointer key, value;
	uint64_t db_epoch;

	n = g_hash_table_size(context->connections_hash);
	response = (struct ssa_admin_msg *) malloc(sizeof(*response) + n * sizeof(struct ssa_admin_connection_info));
	if (!response) {
		ssa_log_err(SSA_LOG_CTRL, "admin response allocation failed\n");
		return NULL;
	}

	response->hdr = admin_request->hdr;
	response->hdr.status = SSA_ADMIN_STATUS_SUCCESS;
	response->hdr.method = SSA_ADMIN_METHOD_RESP;
	response->hdr.len = htons(sizeof(*response));

	nodeinfo_msg = (struct ssa_admin_node_info *) &response->data.counter;

	nodeinfo_msg->type = context->ssa->node_type;
	strncpy((char *) nodeinfo_msg->version, IB_SSA_VERSION,
		SSA_ADMIN_VERSION_LEN - 1);

	db_epoch = (uint64_t) ssa_get_runtime_counter(COUNTER_ID_DB_EPOCH);
	nodeinfo_msg->db_epoch = htonll(db_epoch);

	nodeinfo_msg->connections_num = htons(n);

	connections = (struct ssa_admin_connection_info *) nodeinfo_msg->connections;
	g_hash_table_iter_init(&iter, context->connections_hash);
	i = 0;
	while (g_hash_table_iter_next(&iter, &key, &value))
		connections[i++] = *(struct ssa_admin_connection_info *)value;

	return response;
}

static struct ssa_admin_msg *ssa_admin_handle_message(struct ssa_admin_msg *admin_request,
						      struct ssa_admin_handler_context *context)
{
	struct ssa_admin_msg *default_response;
	int error = 0;

	switch (ntohs(admin_request->hdr.opcode)) {
	case SSA_ADMIN_CMD_PING:
		break;
	case SSA_ADMIN_CMD_COUNTER:
		return ssa_admin_handle_counter_message(admin_request);
		break;
	case SSA_ADMIN_CMD_NODE_INFO:
		return ssa_admin_handle_node_info(admin_request, context);
		break;
	default:
		error = 1;
	};

	default_response = (struct ssa_admin_msg *) malloc(sizeof(*default_response));
	if (!default_response) {
		ssa_log_err(SSA_LOG_CTRL, "admin response allocation failed\n");
		return NULL;
	}

	default_response->hdr = admin_request->hdr;
	default_response->hdr.status = error ? SSA_ADMIN_STATUS_FAILURE:
				       SSA_ADMIN_STATUS_SUCCESS;
	default_response->hdr.method = SSA_ADMIN_METHOD_RESP;
	default_response->hdr.len = htons(sizeof(*default_response));

	return default_response;
}

static int ssa_admin_recv_buf(int rsock, char *buf, int *rcount, int rlen)
{
	int ret;

	while (*rcount < rlen) {
		ret = rrecv(rsock, buf + *rcount, rlen - *rcount, MSG_DONTWAIT);
		if (ret > 0)
			*rcount += ret;
		else if (!ret)
			return -ECONNRESET;
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
			return *rcount;
		else
			return ret;
	}
	return *rcount;

}

static int ssa_admin_read_msg(int rsock, struct ssa_admin_msg *msg,
			      int *rcount, int *rlen)
{
	int ret = 0, n = 0;

	if (*rcount < sizeof(msg->hdr)) {
		ret = ssa_admin_recv_buf(rsock, (char *) &msg->hdr,
					 rcount, sizeof(msg->hdr));
		if (ret == -ECONNRESET) {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"admin peer disconnected rsock %d\n", rsock);
			return ret;
		} else if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL,
				    "rrecv failed: %d (%s) on rsock %d\n",
				    errno, strerror(errno), rsock);
			return ret;
		}
		if (*rcount < sizeof(msg->hdr))
			return ret;
	}

	*rlen = ntohs(msg->hdr.len);
	if (*rlen > sizeof(*msg)) {
		ssa_log_err(SSA_LOG_CTRL,
			    "received admin message (%d bytes) longer than internal struct (%d bytes) on rsock %d\n",
			    *rlen, sizeof(*msg), rsock);
		return -1;
	}

	if (*rlen == ret)
		return ret;

	n = ret;

	ret = ssa_admin_recv_buf(rsock, (char *) msg, rcount, *rlen);
	if (ret == -ECONNRESET) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"admin peer disconnected rsock %d\n", rsock);
		return ret;
	} else if (ret < 0) {
		ssa_log_err(SSA_LOG_CTRL,
			    "rrecv failed: %d (%s) on rsock %d\n",
			    errno, strerror(errno), rsock);
		return ret;
	}
	n += ret;
	return n;
}

static int ssa_admin_send_msg(int rsock, struct ssa_admin_msg *msg,
			      int *sleft, int slen)
{
	int sent = slen - *sleft;
	int n;

	while (*sleft) {
		n = rsend(rsock, (char *) msg + sent, *sleft, MSG_DONTWAIT);
		if (n < 0)
			break;
		*sleft -= n;
		sent +=n;
	}

	if (*sleft) {
		return 0;
	} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	} else if (n < 0)
		return n;

	return 0;
}

#ifdef SSA_ADMIN_DEBUG
static void ssa_print_admin_msg(const struct ssa_admin_msg *msg)
{
	char buf[128] = {};

	ssa_format_admin_msg(buf, sizeof(buf), msg);
	ssa_log(SSA_LOG_DEFAULT, "%s \n", buf);
}
#define SSA_ADMIN_REPORT_MSG(msg) {ssa_print_admin_msg(msg);}
#else
#define SSA_ADMIN_REPORT_MSG(msg)
#endif

static void *ssa_admin_handler(void *context)
{
	struct ssa_class *ssa = context;
	struct ssa_device *dev;
	struct ssa_port *port;
	struct ssa_ctrl_msg_buf msg;
	struct ssa_admin_msg admin_request, *admin_response = NULL;
	struct pollfd *fds = NULL;
	int rsock = -1;
	int val, ret, svc_cnt = 0;
	int i, d, p, s;
	int rlen = 0, rcount;
	int slen = 0, sleft = 0;
	GHashTable *connections_hash = NULL;
	GHashTable *svcs_hash = NULL;
	gboolean gres;
	struct ssa_admin_handler_context handler_context;

	SET_THREAD_NAME(*admin_thread, "ADMIN");

	connections_hash = g_hash_table_new_full(NULL, NULL, NULL,
						 ssa_destroy_connection_info);
	if (!connections_hash) {
		ssa_log_err(SSA_LOG_CTRL, "unable to allocate connections hash\n");
		goto out;
	}

	svcs_hash = g_hash_table_new_full(NULL, NULL, NULL, NULL);
	if (!svcs_hash) {
		ssa_log_err(SSA_LOG_CTRL, "unable to allocate svc hash\n");
		goto out;
	}

	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			svc_cnt += port->svc_cnt;
		}
	}

	handler_context.connections_hash = connections_hash;
	handler_context.svcs_hash = svcs_hash;
	handler_context.ssa = ssa;

	fds = calloc(ADMIN_FIRST_SERVICE_FD_SLOT + svc_cnt * ADMIN_FDS_PER_SERVICE,
		     sizeof(*fds));
	if (!fds) {
		ssa_log_err(SSA_LOG_CTRL, "unable to allocate fds\n");
		goto out;
	}

	rsock = ssa_admin_listen(ssa, admin_port);

	fds[0].fd = sock_adminctrl[1];
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	fds[1].fd = rsock;
	fds[1].events = POLLIN;
	fds[1].revents = 0;

	fds[2].fd = -1;
	fds[2].events = 0;
	fds[2].revents = 0;

	i = ADMIN_FIRST_SERVICE_FD_SLOT;
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			if (port->link_layer != IBV_LINK_LAYER_INFINIBAND)
				continue;

			for (s = 0; s < port->svc_cnt; s++) {
				fds[i].fd = port->svc[s]->sock_adminup[1];
				fds[i].events = POLLIN;
				fds[i].revents = 0;
				g_hash_table_insert(svcs_hash, GINT_TO_POINTER(fds[i].fd), port->svc[s]);
				i++;
				fds[i].fd = port->svc[s]->sock_admindown[1];
				fds[i].events = POLLIN;
				fds[i].revents = 0;
				g_hash_table_insert(svcs_hash, GINT_TO_POINTER(fds[i].fd), port->svc[s]);
				i++;
			}
		}
	}

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = rsock >= 0 ? SSA_CTRL_ACK : SSA_CTRL_NACK;
	ret = write(sock_adminctrl[1], (char *) &msg, sizeof msg.hdr);
	if (ret != sizeof msg.hdr)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg.hdr);

	if (rsock < 0)
		goto out;

	for (;;) {
		ret = rpoll(fds,
			    ADMIN_FIRST_SERVICE_FD_SLOT + svc_cnt * ADMIN_FDS_PER_SERVICE,
			    -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}

		if (fds[0].revents) {
			fds[0].revents = 0;
			ret = read(sock_adminctrl[1], (char *) &msg,
				   sizeof msg.hdr);
			if (ret != sizeof msg.hdr)
				ssa_log_err(SSA_LOG_CTRL,
					    "%d out of %d header bytes read from ctrl\n",
					    ret, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				ret = read(sock_adminctrl[1],
					   (char *) &msg.hdr.data,
					   msg.hdr.len - sizeof msg.hdr);
				if (ret != msg.hdr.len - sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d additional bytes read from ctrl\n",
						    ret,
						    msg.hdr.len - sizeof msg.hdr);
			}

			switch (msg.hdr.type) {
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected msg type %d "
					     "from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[1].revents) {
			int revents = fds[1].revents;

			fds[1].revents = 0;
			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				char event_str[128] = {};

				ssa_format_event(event_str, sizeof(event_str),
						 fds[1].revents);

				ssa_log_err(SSA_LOG_CTRL,
					    "error event 0x%x (%s) on rsock %d\n",
					    revents, event_str, rsock);
				continue;
			}
			if (revents & POLLIN) {
				int rsock_data;
				struct sockaddr_ib peer_addr;
				socklen_t peer_len;

				rsock_data = raccept(rsock, NULL, 0);
				if (rsock_data < 0) {
					ssa_log_err(SSA_LOG_CTRL,
						    "raccept rsock %d ERROR %d (%s)\n",
						    rsock, errno, strerror(errno));
					continue;
				}
				if (fds[2].fd >= 0) {
					rclose(fds[2].fd);
					ssa_log_warn(SSA_LOG_CTRL,
						     "closed previous admin client connection on rsock %d\n",
						     fds[2].fd);
				}
				ssa_log(SSA_LOG_CTRL,
					"New admin connection accepted on rsock %d\n",
					rsock_data);

				peer_len = sizeof(peer_addr);
				if (!rgetpeername(rsock_data, (struct sockaddr *) &peer_addr, &peer_len)) {
					if (peer_addr.sib_family == AF_IB) {
						ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
								log_data, sizeof log_data, SSA_ADDR_GID,
								(uint8_t *) &peer_addr.sib_addr,
								peer_len);
						ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
								"peer GID %s\n", log_data);
					} else {
						ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
							"rgetpeername fd %d family %d not AF_IB\n",
							rsock_data, peer_addr.sib_family);
						rclose(rsock_data);
						continue;
					}
				} else {
					ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
						"rgetpeername rsock %d ERROR %d (%s)\n",
						rsock_data, errno, strerror(errno));
					rclose(rsock_data);
					continue;
				}

				val = 1;
				ret = rsetsockopt(rsock_data, IPPROTO_TCP, TCP_NODELAY,
						  (void *) &val, sizeof(val));
				if (ret) {
					ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
						"rsetsockopt TCP_NODELAY ERROR %d (%s) on rsock %d\n",
						errno, strerror(errno), rsock_data);
					rclose(rsock_data);
					continue;
				}

				ret = rfcntl(rsock_data, F_SETFL, O_NONBLOCK);
				if (ret) {
					ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
						"rfcntl ERROR %d (%s) on rsock %d\n",
						errno, strerror(errno), rsock_data);
					rclose(rsock_data);
					continue;
				}

				ssa_rsock_enable_keepalive(rsock_data, keepalive);

				rlen = 0;
				rcount = 0;
				slen = 0;
				sleft = 0;

				free(admin_response);
				admin_response = NULL;
				fds[2].fd = rsock_data;
				fds[2].events = POLLIN;
				fds[2].revents = 0;
			}
		}

		if (fds[2].revents) {
			if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				if (fds[2].revents & POLLHUP) {
					ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
						"admin peer disconnected rsock %d\n",
						fds[2].fd);
				} else {
					char event_str[128] = {};

					ssa_format_event(event_str,
							 sizeof(event_str),
							 fds[2].revents);
					ssa_log_err(SSA_LOG_CTRL,
						    "revent 0x%x (%s)on rsock %d\n",
						    fds[2].revents, event_str, fds[2].fd);
				}
				rclose(fds[2].fd);
				fds[2].fd = -1;
				fds[2].events = 0;
				fds[2].revents = 0;
				continue;
			}
			if (fds[2].revents & POLLOUT) {
				fds[2].revents = 0;

				if (admin_response && slen && sleft) {
					ret = ssa_admin_send_msg(fds[2].fd, admin_response, &sleft, slen);
					if (ret) {
						rclose(fds[2].fd);
						fds[2].fd = -1;
						fds[2].events = 0;
						fds[2].revents = 0;
						continue;
					}

					if (!sleft) {
						SSA_ADMIN_REPORT_MSG(admin_response);
						fds[2].events = POLLIN;
						slen = 0;
						rlen = 0;
						rcount = 0;
					}

				}
				continue;
			}
			if (fds[2].revents & POLLIN) {
				fds[2].revents = 0;

				ret = ssa_admin_read_msg(fds[2].fd, &admin_request, &rcount, &rlen);
				if (ret < 0) {
					rclose(fds[2].fd);
					fds[2].fd = -1;
					fds[2].events = 0;
					fds[2].revents = 0;
					continue;
				}

				if (rlen > 0 && rcount == rlen) {
					rcount = 0;
					rlen = 0;

					ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
						"new admin request received method %d opcode %d len %d\n",
						admin_request.hdr.method, ntohs(admin_request.hdr.opcode),
						ntohs(admin_request.hdr.len));

					SSA_ADMIN_REPORT_MSG(&admin_request);

					if (!ssa_admin_verify_message(&admin_request)) {
						ssa_log_warn(SSA_LOG_CTRL,
							     "admin request verification failed\n");
						admin_response = (struct ssa_admin_msg *) malloc(sizeof(*admin_response));
						if (!admin_response) {
							ssa_log_err(SSA_LOG_CTRL,
								    "admin response allocation failed\n");
							rclose(fds[2].fd);
							fds[2].fd = -1;
							fds[2].events = 0;
							fds[2].revents = 0;
							continue;
						}

						admin_response->hdr = admin_request.hdr;
						admin_response->hdr.status = SSA_ADMIN_STATUS_FAILURE;
						admin_response->hdr.method = SSA_ADMIN_METHOD_RESP;
						admin_response->hdr.len = htons(sizeof(*admin_response));
					} else {
						admin_response = ssa_admin_handle_message(&admin_request, &handler_context);
					}

					if (admin_response) {
						fds[2].events = POLLOUT;
						slen = ntohs(admin_response->hdr.len);
						sleft = slen;
					} else
						ssa_log_err(SSA_LOG_CTRL,
							    "admin failed to create response\n");
				}
			}
			fds[2].revents = 0;
		}


		for (i = ADMIN_FIRST_SERVICE_FD_SLOT; i < ADMIN_FIRST_SERVICE_FD_SLOT + svc_cnt * ADMIN_FDS_PER_SERVICE; i++) {
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				if (fds[i].revents & POLLERR) {
					char event_str[128] = {};

					ssa_format_event(event_str,
							 sizeof(event_str),
							 fds[i].revents);
					ssa_log_err(SSA_LOG_CTRL,
						    "revent 0x%x (%s) on sock %d\n",
						    fds[i].revents, event_str, fds[i].fd);
				}
				fds[i].fd = -1;
				fds[i].events = 0;
				fds[i].revents = 0;
				continue;
			}
			if (fds[i].revents & POLLIN) {
				fds[i].revents = 0;

				ret = read(fds[i].fd,
					   (char *) &msg, sizeof msg.hdr);
				if (ret != sizeof msg.hdr)
					ssa_log_err(SSA_LOG_CTRL,
						    "%d out of %d header bytes read\n",
						    ret, sizeof msg.hdr);
				if (msg.hdr.len > sizeof msg.hdr) {
					ret = read(fds[i].fd,
						   (char *) &msg.hdr.data,
						   msg.hdr.len - sizeof msg.hdr);
					if (ret != msg.hdr.len - sizeof msg.hdr)
						ssa_log_err(SSA_LOG_CTRL,
							    "%d out of %d additional bytes read\n",
							    ret,
							    msg.hdr.len - sizeof msg.hdr);
				}
				switch (msg.hdr.type) {
				case SSA_CONN_DONE:
				{
					struct ssa_svc *svc;
					struct ssa_admin_connection_info *connection_info;

					ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE | SSA_LOG_CTRL,
							log_data, sizeof log_data,
							SSA_ADDR_GID,
							msg.data.conn_data.remote_gid.raw,
							sizeof msg.data.conn_data.remote_gid.raw);
					ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
						"connection done on rsock %d from GID %s LID %u\n",
						msg.data.conn_data.rsock, log_data,
						msg.data.conn_data.remote_lid);

					svc = g_hash_table_lookup(svcs_hash, GINT_TO_POINTER(fds[i].fd));
					if (!svc) {
						ssa_log_err(SSA_LOG_CTRL, "failed get SSA service for fd %d\n", fds[i].fd);
						fds[i].fd = -1;
						fds[i].events = 0;
						fds[i].revents = 0;
						continue;
					}
					connection_info = (struct ssa_admin_connection_info *) malloc(sizeof(*connection_info));
					if (!connection_info) {
						ssa_log_err(SSA_LOG_CTRL, "failed allocate connection info\n");
					} else {
						struct timeval now;

						connection_info->connection_type = msg.data.conn_data.type;
						connection_info->dbtype = msg.data.conn_data.dbtype;
						connection_info->remote_type = connection_info->connection_type == SSA_CONN_TYPE_UPSTREAM ?
							svc->primary_type : 0;
						connection_info->remote_lid = htons(msg.data.conn_data.remote_lid);

						gettimeofday(&now, NULL);
						connection_info->connection_tv_sec = htonll(now.tv_sec);
						connection_info->connection_tv_usec = htonll(now.tv_usec);

						memcpy(&connection_info->remote_gid, &msg.data.conn_data.remote_gid.raw, sizeof(connection_info->remote_gid));
						g_hash_table_replace(connections_hash, GINT_TO_POINTER(msg.data.conn_data.rsock), connection_info);
					}
				}
					break;
				case SSA_CONN_GONE:
					ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_VERBOSE | SSA_LOG_CTRL,
							log_data, sizeof log_data,
							SSA_ADDR_GID,
							msg.data.conn_data.remote_gid.raw,
							sizeof msg.data.conn_data.remote_gid.raw);
					ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
						"connection gone from GID %s LID %u\n",
						log_data, msg.data.conn_data.remote_lid);
					gres = g_hash_table_remove(connections_hash, GINT_TO_POINTER(msg.data.conn_data.rsock));
					if (!gres)
						ssa_log_warn(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
								"connection from GID %s LID %u not found\n",
								log_data, msg.data.conn_data.remote_lid);
					break;
				default:
					ssa_log_warn(SSA_LOG_CTRL,
						     "ignoring unexpected msg "
						     "type %d\n",
						     msg.hdr.type);
					break;
				}
			}
		}
	}
out:
	free(fds);
	if (rsock >= 0)
		rclose(rsock);

	if (connections_hash)
		g_hash_table_destroy(connections_hash);

	if (svcs_hash)
		g_hash_table_destroy(svcs_hash);

	return NULL;
}

int ssa_start_admin(struct ssa_class *ssa)
{
	struct ssa_ctrl_msg msg;
	int ret;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);

	sock_adminctrl[0] = -1;
	sock_adminctrl[1] = -1;

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sock_adminctrl);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating admin socketpair\n");
		goto err1;
	}

	admin_thread = calloc(1, sizeof(*admin_thread));
	if (admin_thread  == NULL) {
		ssa_log_err(SSA_LOG_CTRL, "allocating admin thread memory\n");
		goto err2;
	}

	ret = pthread_create(admin_thread, NULL, ssa_admin_handler, ssa);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating admin thread\n");
		errno = ret;
		goto err3;
	}

	ret = read(sock_adminctrl[0], (char *) &msg, sizeof msg);
	if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
		ssa_log_err(SSA_LOG_CTRL, "with admin thread\n");
		goto err4;
	}

	return 0;
err4:
	pthread_join(*admin_thread, NULL);
err3:
	free(admin_thread);
err2:
	close(sock_adminctrl[0]);
	close(sock_adminctrl[1]);
err1:
	return 1;
}

void ssa_stop_admin()
{
	int ret;
	struct ssa_ctrl_msg msg;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);

	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	ret = write(sock_adminctrl[0], (char *) &msg, sizeof msg);
	if (ret != sizeof msg)
		ssa_log_err(SSA_LOG_CTRL, "%d out of %d bytes written\n",
			    ret, sizeof msg);
	if (admin_thread) {
		pthread_join(*admin_thread, NULL);
		free(admin_thread);
	}

	close(sock_adminctrl[0]);
	close(sock_adminctrl[1]);
}
