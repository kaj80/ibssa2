/*
 * Copyright (c) 2015 Mellanox Technologies LTD. All rights reserved.
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

#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <rdma/rsocket.h>
#include <infiniband/ib.h>
#include <infiniband/umad.h>
#include <infiniband/umad_sm.h>

#include "libadmin.h"
#include <osd.h>
#include <ssa_admin.h>
#include <common.h>
#include <infiniband/ssa_mad.h>


#define MAX_COMMAND_OPTS 20
#define GID_ADDRESS_WIDTH 21

struct cmd_exec_info {
	uint64_t stime, etime;
};

struct cmd_struct_impl;
struct admin_count_command {
	short print_all;
	short include_list[STATS_ID_LAST];
};

struct admin_disconnect_command {
	int type;
	union {
		uint16_t lid;
		union ibv_gid gid;
	} id;
};

enum nodeinfo_mode {
	NODEINFO_FULL = 0xFF,
	NODEINFO_SINGLELINE = 0x1,
	NODEINFO_UP_CONN = 0x2,
	NODEINFO_DOWN_CONN = 0x4
};

struct nodeinfo_format_option {
	const char *value;
	const char *description;
	uint16_t mode;
};

static const struct nodeinfo_format_option nodeinfo_format_options[] = {
	{ "full", "print full SSA node information", NODEINFO_FULL },
	{ "short", "print short SSA node information in single line", NODEINFO_SINGLELINE },
	{ "conn", "print SSA node connections", NODEINFO_UP_CONN | NODEINFO_DOWN_CONN },
	{ "up", "print upstream connection", NODEINFO_UP_CONN },
	{ "down", "print downstream connections", NODEINFO_DOWN_CONN }
};

static const struct nodeinfo_format_option disconnect_format_options[] = {
	{ "lid", "disconnect connection by lid", SSA_ADDR_LID},
	{ "gid", "disconnect connection by gid", SSA_ADDR_GID },
};

struct admin_nodeinfo_command {
	uint16_t mode;
};

struct admin_command {
	const struct cmd_struct_impl *impl;
	const struct cmd_struct *cmd;
	union {
		struct admin_count_command count_cmd;
		struct admin_nodeinfo_command nodeinfo_cmd;
		struct admin_disconnect_command disconnect_cmd;

	} data;
	short recursive;
};

struct cmd_struct_impl {
	int (*init)(struct admin_command *admin_cmd);
	int (*handle_option)(struct admin_command *admin_cmd,
			     char option, const char *optarg);
	int (*handle_param)(struct admin_command *admin_cmd,
			    const char *param);
	void (*destroy)(struct admin_command *admin_cmd);
	int (*create_request)(struct admin_command *admin_cmd,
			      struct ssa_admin_msg *msg);
	void (*handle_response)(struct admin_command *cmd,
				struct cmd_exec_info *exec_info,
				union ibv_gid remote_gid,
				const struct ssa_admin_msg *msg);
	struct cmd_opts opts[MAX_COMMAND_OPTS];
	struct cmd_help help;
};


static void default_destroy(struct admin_command *cmd);
static void default_print_usage(FILE *stream);
static struct admin_command *default_init(int cmd_id,
					  int argc, char **argv);
static int default_create_msg(struct admin_command *cmd,
			      struct ssa_admin_msg *msg);
static void ping_command_output(struct admin_command *cmd,
				struct cmd_exec_info *exec_info,
				union ibv_gid remote_gid,
				const struct ssa_admin_msg *msg);
static int stats_init(struct admin_command *cmd);
static int stats_handle_param(struct admin_command *cmd, const char *param);
static void stats_print_help(FILE *stream);
static int stats_command_create_msg(struct admin_command *cmd,
				    struct ssa_admin_msg *msg);
static void stats_command_output(struct admin_command *cmd,
				 struct cmd_exec_info *exec_info,
				 union ibv_gid remote_gid,
				 const struct ssa_admin_msg *msg);
static int nodeinfo_init(struct admin_command *cmd);
static void nodeinfo_command_output(struct admin_command *cmd,
				    struct cmd_exec_info *exec_info,
				    union ibv_gid remote_gid,
				    const struct ssa_admin_msg *msg);
static int nodeinfo_handle_option(struct admin_command *admin_cmd,
				  char option, const char *optarg);
static void nodeinfo_print_help(FILE *stream);

static int disconnect_init(struct admin_command *cmd);
static int disconnect_handle_option(struct admin_command *admin_cmd,
				  char option, const char *optarg);
static int disconnect_handle_param(struct admin_command *cmd, const char *param);
static int disconnect_command_create_msg(struct admin_command *cmd,
					 struct ssa_admin_msg *msg);
static void disconnect_command_output(struct admin_command *cmd,
				      struct cmd_exec_info *exec_info,
				      union ibv_gid remote_gid,
				      const struct ssa_admin_msg *msg);

static struct cmd_struct_impl admin_cmd_command_impls[] = {
	[SSA_ADMIN_CMD_STATS] = {
		stats_init,
		NULL, stats_handle_param,
		default_destroy,
		stats_command_create_msg,
		stats_command_output,
		{},
		{ stats_print_help, default_print_usage,
		  "Retrieve runtime statistics" },
	},
	[SSA_ADMIN_CMD_PING]	= {
		NULL,
		NULL, NULL,
		default_destroy,
		default_create_msg,
		ping_command_output,
		{},
		{ NULL, default_print_usage,
		  "Test ping between local node and SSA service on a specified target node" }
	},
	[SSA_ADMIN_CMD_NODEINFO] = {
		nodeinfo_init,
		nodeinfo_handle_option, NULL,
		default_destroy,
		default_create_msg,
		nodeinfo_command_output,
		{
			{ { "format", required_argument, 0, 'f' }, "[full|short|conn|up|down]" },
			{ { 0, 0, 0, 0 } }	/* Required at the end of the array */
		},
		{ NULL, nodeinfo_print_help,
		  "Retrieve basic node info" }
	},
	[SSA_ADMIN_CMD_DISCONNECT] = {
		disconnect_init,
		disconnect_handle_option, disconnect_handle_param,
		default_destroy,
		disconnect_command_create_msg,
		disconnect_command_output,
		{
			{ { "addr_type",required_argument, 0, 'x' }, "[lid|gid]" },
			{ { 0, 0, 0, 0 } }	/* Required at the end of the array */
		},
		{ NULL, default_print_usage,
		  "Break connection" }
	}
};

static atomic_t tid;
static short admin_port = 7477;
static uint16_t pkey_default = 0xffff;
static const char *local_gid = "::1";
static int timeout = 1000;
static  struct admin_opts global_opts;

static const char *short_opts_to_skip;
static struct option *long_opts_to_skip;
static int long_opts_num;

static uint64_t get_timestamp()
{
	uint64_t tstamp;
	struct timeval tv;

	gettimeofday(&tv ,0);

	/* Convert the time of day into a microsecond timestamp. */
	tstamp = ((uint64_t) tv.tv_sec * 1000000) + (uint64_t) tv.tv_usec;

	return tstamp;
}

#ifdef SSA_ADMIN_DEBUG
static void print_admin_msg(const struct ssa_admin_msg *msg)
{
	char buf[256] = {};

	ssa_format_admin_msg(buf, sizeof(buf), msg);
	fprintf(stderr, "%s \n", buf);
}

#define SSA_ADMIN_REPORT_MSG(msg) {print_admin_msg(msg);}
#else
#define SSA_ADMIN_REPORT_MSG(msg)
#endif

int admin_init(const char *short_opts, struct option *long_opts)
{
	int i = 0;

	srand(time(NULL));

	atomic_init(&tid);
	atomic_set(&tid, rand());

	short_opts_to_skip = short_opts;
	long_opts_to_skip = long_opts;

	while (long_opts_to_skip[i++].name)
		long_opts_num++;

	return 0;
}

void admin_cleanup()
{
	return;
}

static int open_port(const char *dev, int port)
{
	int port_id;

	if (umad_init() < 0) {
		fprintf(stderr, "ERROR - unable to init UMAD library\n");
		return -1;
	}

	if ((port_id = umad_open_port((char *) dev, (port < 0) ? 0 : port)) < 0) {
		fprintf(stderr, "ERROR - can't open UMAD port\n");
		return -1;
	}

	return port_id;
}

static void close_port(int port_id)
{
	umad_close_port(port_id);
	umad_done();
}

/*
 * If no port specified (port is -1), first physical port in active
 * state is queried for sm lid and sm sl.
 */
static int get_sm_info(const char *ca_name, int port,
		       uint16_t *sm_lid, uint8_t *sm_sl)
{
	struct ibv_device **dev_arr, *dev;
	struct ibv_context *verbs = NULL;
	struct ibv_port_attr port_attr;
	struct ibv_device_attr attr;
	int  d, p, ret, status = -1;
	int dev_cnt, port_cnt;

	dev_arr = ibv_get_device_list(&dev_cnt);
	if (!dev_arr) {
		fprintf(stderr, "ERROR - unable to get device list\n");
		return -1;
	}

	for (d = 0; d < dev_cnt; d++) {
		dev = dev_arr[d];

		if (ca_name && strncmp(ca_name, dev->name, IBV_SYSFS_NAME_MAX))
			continue;

		if (dev->transport_type != IBV_TRANSPORT_IB ||
		    dev->node_type != IBV_NODE_CA) {
			if (ca_name) {
				fprintf(stderr, "ERROR - invalid device (%s)\n",
					dev->name);
				goto out;
			} else {
				continue;
			}
		}

		verbs = ibv_open_device(dev);
		if (!verbs) {
			fprintf(stderr, "ERROR - unable to open device (%s)\n",
				dev->name);
			goto out;
		}

		ret = ibv_query_device(verbs, &attr);
		if (ret) {
			fprintf(stderr, "ERROR - ibv_query_device (%s) %d\n",
				dev->name, ret);
			goto out;
		}

		port_cnt = attr.phys_port_cnt;

		for (p = 1; p <= port_cnt; p++) {
			if (port >= 0 && port != p)
				continue;

			ret = ibv_query_port(verbs, p, &port_attr);
			if (ret) {
				fprintf(stderr, "ERROR - ibv_query_port (%s) %d\n",
					dev->name, ret);
				goto out;
			}

			if (port_attr.link_layer != IBV_LINK_LAYER_INFINIBAND ||
			    port_attr.state != IBV_PORT_ACTIVE) {
				if (port >= 0) {
					fprintf(stderr, "ERROR - invalid port %s:%d\n",
						dev->name, port);
					goto out;
				} else {
					continue;
				}
			}

			*sm_lid = port_attr.sm_lid;
			*sm_sl = port_attr.sm_sl;
			break;
		}

		if (p <= port_cnt)
			break;

		if (ca_name) {
			fprintf(stderr, "ERROR - no active port found for %s device\n",
				dev->name);
			goto out;
		}
	}

	if (d == dev_cnt)
		fprintf(stderr, "ERROR - no proper device with active port found\n");
	else
		status = 0;

out:
	if (verbs)
		ibv_close_device(verbs);

	ibv_free_device_list(dev_arr);

	return status;
}

static int get_gid(const char *dev, int port, int port_id,
		   uint16_t dlid, union ibv_gid *dgid)
{
	struct sa_path_record *mad;
	struct ibv_path_record *path;
	int ret, len, status = 0;
	int agent_id = -1;
	struct sa_umad umad;
	uint16_t sm_lid = 0;
	uint8_t sm_sl = 0;

	agent_id = umad_register(port_id, UMAD_CLASS_SUBN_ADM,
				 UMAD_SA_CLASS_VERSION, 0, NULL);
	if (agent_id < 0) {
		fprintf(stderr, "ERROR - unable to register SSA class on local port\n");
		status = -1;
		goto err;
	}

	if (get_sm_info(dev, port, &sm_lid, &sm_sl)) {
		status = -1;
		goto err;
	}

	memset(&umad, 0, sizeof umad);
	umad_set_addr(&umad.umad, sm_lid, 1, sm_sl, UMAD_QKEY);
	mad = &umad.sa_mad.path_rec;

	mad->mad_hdr.base_version	= UMAD_BASE_VERSION;
	mad->mad_hdr.mgmt_class		= UMAD_CLASS_SUBN_ADM;
	mad->mad_hdr.class_version	= UMAD_SA_CLASS_VERSION;
	mad->mad_hdr.method		= UMAD_METHOD_GET;
	mad->mad_hdr.tid		= htonll((uint64_t) atomic_inc(&tid));
	mad->mad_hdr.attr_id		= htons(UMAD_SA_ATTR_PATH_REC);

	mad->comp_mask = htonll(((uint64_t)1) << 4 |    /* DLID */
				((uint64_t)1) << 11 |   /* Reversible */
				((uint64_t)1) << 13);   /* P_Key */

	path = &mad->path;
	path->dlid = htons(dlid);
	path->reversible_numpath = IBV_PATH_RECORD_REVERSIBLE;
	path->pkey = 0xFFFF;    /* default partition */

	ret = umad_send(port_id, agent_id, (void *) &umad,
			sizeof umad.sa_mad.packet, -1 /* timeout */, 0);
	if (ret) {
		fprintf(stderr, "ERROR - failed to send path query to SA\n");
		status = -1;
		goto err;
	}

	len = sizeof umad.sa_mad.packet;
	ret = umad_recv(port_id, (void *) &umad, &len, -1 /* timeout */);
	if (ret < 0 || ret != agent_id) {
		fprintf(stderr, "ERROR - failed to receive path record from SA\n");
		status = -1;
		goto err;
	}

	if (umad.sa_mad.path_rec.mad_hdr.status == UMAD_SA_STATUS_SUCCESS) {
		path = &umad.sa_mad.path_rec.path;
		memcpy(dgid->raw, path->dgid.raw, 16);
	} else {
		fprintf(stderr, "ERROR - specified LID (%u) doesn't exist\n", dlid);
		status = -1;
	}

err:
	if (agent_id >= 0)
		umad_unregister(port_id, agent_id);

	return status;
}


static int admin_connect_init(void *dest, int type, struct admin_opts *opts)
{
	struct sockaddr_ib dst_addr;
	union ibv_gid dgid;
	int ret, val, port_id;
	int port = opts->admin_port ? opts->admin_port : admin_port;
	uint16_t pkey = opts->pkey ? opts->pkey : pkey_default;
	int rsock = -1;
	char dest_addr[64];

	timeout = opts->timeout;
	global_opts = *opts;

	rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (rsock < 0) {
		fprintf(stderr, "rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	val = 1;
	ret = rsetsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
	if (ret) {
		fprintf(stderr, "rsetsockopt rsock %d SO_REUSEADDR ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	ret = rsetsockopt(rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		fprintf(stderr, "rsetsockopt rsock %d TCP_NODELAY ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	ret = rfcntl(rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		fprintf(stderr, "rfcntl F_SETFL rsock %d ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	dst_addr.sib_family	= AF_IB;
	dst_addr.sib_pkey	= htons(pkey);
	dst_addr.sib_flowinfo	= 0;
	dst_addr.sib_sid	=
		htonll(((uint64_t) RDMA_PS_TCP << 16) + port);
	dst_addr.sib_sid_mask	= htonll(RDMA_IB_IP_PS_MASK);
	dst_addr.sib_scope_id	= 0;

	if (type == ADMIN_ADDR_TYPE_GID) {
		if (!strncmp((char *) dest, local_gid, strlen(local_gid))) {
			snprintf(dest_addr, 10, "localhost");
		} else {
			snprintf(dest_addr, max(64, strlen((char *) dest)),
				 "GID %s", (char *) dest);
		}

		ret = inet_pton(AF_INET6, dest ? (char *) dest : local_gid, &dgid);
		if (!ret)
			fprintf(stderr, "ERROR - wrong server GID specified\n");
		else if (ret < 0)
			fprintf(stderr, "ERROR - not supported address family\n");

		if (ret <= 0)
			goto err;
	} else if (type == ADMIN_ADDR_TYPE_LID) {
		port_id = open_port(opts->dev, opts->src_port);
		if (port_id < 0)
			goto err;

		ret = get_gid(opts->dev, opts->src_port,
			      port_id, *(uint16_t *) dest, &dgid);
		if (ret) {
			fprintf(stderr, "ERROR - unable to get GID for LID %u\n",
				*(uint16_t *) dest);
			close_port(port_id);
			goto err;
		}
		close_port(port_id);

		sprintf(dest_addr, "LID %u", *(uint16_t *) dest);
	}

	memcpy(&dst_addr.sib_addr, &dgid, sizeof(dgid));

	ret = rconnect(rsock, (const struct sockaddr *) &dst_addr,
		       sizeof(dst_addr));
	if (ret && (errno != EINPROGRESS)) {
		fprintf(stderr, "ERROR - rconnect rsock %d ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	return rsock;

err:
	rclose(rsock);
	return -1;
}

int admin_connect(void *dest, int type, struct admin_opts *opts)
{
	int ret, val, err;
	unsigned int len;
	struct pollfd fds;
	int rsock = -1;

	timeout = opts->timeout;
	global_opts = *opts;

	rsock = admin_connect_init(dest, type, opts);
	if (rsock < 0)
		return -1;

	if (rsock && (errno == EINPROGRESS)) {
		fds.fd = rsock;
		fds.events = POLLOUT;
		fds.revents = 0;
		ret = rpoll(&fds, 1, timeout);
		if (ret < 0) {
			fprintf(stderr, "ERROR - rpoll rsock %d ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
			goto err;
		} else if (ret == 0) {
			fprintf(stderr, "ERROR - rconnect rsock %d timeout expired\n",
				rsock);
			goto err;
		}

		len = sizeof(err);
		ret = rgetsockopt(rsock, SOL_SOCKET, SO_ERROR, &err, &len);
		if (ret) {
			fprintf(stderr, "rgetsockopt rsock %d ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
			goto err;
		}
		if (err) {
			ret = -1;
			errno = err;
			fprintf(stderr, "ERROR - async rconnect rsock %d ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
			goto err;
		}
	}

	val = rfcntl(rsock, F_GETFL, O_NONBLOCK);
	if (val < 0) {
		fprintf(stderr, "ERROR - rfcntl F_GETFL rsock %d ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	val = val & (~O_NONBLOCK);
	ret = rfcntl(rsock, F_SETFL, val);
	if (ret) {
		fprintf(stderr, "ERROR - rfcntl second F_SETFL rsock %d ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	return rsock;

err:
	rclose(rsock);
	return -1;
}

void admin_disconnect(int rsock)
{
	if (rsock == -1)
		return;

	rclose(rsock);
}

static int get_cmd_opts(struct cmd_opts *cmd_opts, struct option *long_opts,
			char *short_opts)
{
	int i = 0, j = 0, n = 0;

	while (cmd_opts[j].op.name) {
		long_opts[i] = cmd_opts[j].op;
		n += sprintf(short_opts + n, "%c",
			     cmd_opts[j].op.val);

		if (cmd_opts[j].op.has_arg)
			n += sprintf(short_opts + n, ":");
		i++;
		j++;
	}

	sprintf(short_opts + n, "%s", short_opts_to_skip);

	j = 0;
	while (long_opts_to_skip[j].name)
		long_opts[i++] = long_opts_to_skip[j++];

	/* copy last terminating record: { 0, 0, 0, 0} */
	long_opts[i] = long_opts_to_skip[j];

	return 0;
}

static void default_destroy(struct admin_command *cmd)
{
	free(cmd);
}

static void default_print_usage(FILE *stream)
{
	(void)(stream);
}

static struct admin_command *default_init(int cmd_id, int argc, char **argv)
{
	struct option *long_opts = NULL;
	char short_opts[256];
	int option, n;
	struct cmd_opts *opts;
	struct cmd_struct *cmd;
	struct cmd_struct_impl *impl;
	struct admin_command *admin_cmd = NULL;
	int i, ret;

	if (cmd_id <= SSA_ADMIN_CMD_NONE || cmd_id >= SSA_ADMIN_CMD_MAX) {
		fprintf(stderr, "ERROR - command index %d is out of range\n", cmd_id);
		return NULL;
	}

	cmd = &admin_cmds[cmd_id];
	impl = &admin_cmd_command_impls[cmd_id];
	opts = impl->opts;
	if (!opts)
		return NULL;

	admin_cmd = (struct admin_command *) malloc(sizeof(*admin_cmd));
	if (!admin_cmd) {
		fprintf(stderr,
			"ERROR - unable to allocate memory for %s command\n",
			cmd->cmd);
		return NULL;
	}

	admin_cmd->impl = impl;
	admin_cmd->cmd = cmd;
	admin_cmd->recursive = 0;

	if (impl->init) {
		ret = impl->init(admin_cmd);
		if (ret) {
			fprintf(stderr,
				"ERROR - unable init %s command\n",
				cmd->cmd);
			goto error;
		}
	}

	if (argc == 0)
	       return admin_cmd;

	n = ARRAY_SIZE(impl->opts) + long_opts_num;
	long_opts = calloc(1, n * sizeof(*long_opts));
	if (!long_opts) {
		fprintf(stderr,
			"ERROR - unable to allocate memory for %s command\n",
			cmd->cmd);
		goto error;
	}

	get_cmd_opts(opts, long_opts, short_opts);

	do {
		option = getopt_long(argc, argv, short_opts, long_opts, NULL);

		if (impl->handle_option) {
			ret = impl->handle_option(admin_cmd, option, optarg);
			if (ret)
				goto error;
		}

		switch (option) {
		case '?':
			goto error;
		default:
			break;
		}
	} while (option != -1);

	optind++;

	if (impl->handle_param)
		for (i = optind; i < argc; ++i) {
			ret = impl->handle_param(admin_cmd, argv[i]);
			if (ret)
				goto error;
		}

	free(long_opts);

	return admin_cmd;

error:
	if (admin_cmd)
		free(admin_cmd);
	if (long_opts)
		free(long_opts);

	return NULL;
}

static int default_create_msg(struct admin_command *cmd,
			      struct ssa_admin_msg *msg)
{
	(void)(cmd);
	(void)(msg);
	return 0;
}


struct ssa_admin_stats_descr {
	const char *name;
	const char *description;
};

static void ping_command_output(struct admin_command *cmd,
				struct cmd_exec_info *exec_info,
				union ibv_gid remote_gid,
				const struct ssa_admin_msg *msg)
{
	char addr_buf[128];

	(void)(cmd);

	ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
			remote_gid.raw, sizeof remote_gid.raw);
	printf("%lu bytes from \033[1m%s\033[0m : time=%g ms\n",
	       sizeof(*msg), addr_buf,
	       1e-3 * (exec_info->etime - exec_info->stime));
}

static const char *ssa_stats_type_names[] = {
	[ssa_stats_obsolete] = "Obsolete",
	[ssa_stats_numeric] = "Numeric",
	[ssa_stats_timestamp] = "Timestamp"
};

static struct ssa_admin_stats_descr stats_descr[] = {
	[STATS_ID_NODE_START_TIME] = {"NODE_START_TIME", "Starting time of the node" },
	[STATS_ID_DB_UPDATES_NUM] = {"DB_UPDATES_NUM", "Number of databases updates passed the node" },
	[STATS_ID_DB_LAST_UPDATE_TIME] = {"LAST_UPDATE_TIME", "Time of last database update" },
	[STATS_ID_DB_FIRST_UPDATE_TIME] = {"FIRST_UPDATE_TIME", "Time of first database update" },
	[STATS_ID_NUM_CHILDREN] = {"NUM_CHILDREN", "Number of connected downstream nodes" },
	[STATS_ID_NUM_ACCESS_TASKS] = {"NUM_ACCESS_TASKS", "Number of unprocessed Access tasks" },
	[STATS_ID_NUM_ERR] = {"NUM_ERR", "Number of errors" },
	[STATS_ID_LAST_ERR] = {"LAST_ERR", "Last error ID" },
	[STATS_ID_TIME_LAST_UPSTR_CONN] = {"TIME_LAST_UPSTR_CONN", "Time of last upstream connect" },
	[STATS_ID_TIME_LAST_DOWNSTR_CONN] = {"TIME_LAST_DOWNSTR_CONN", "Time of last downstream connect" },
	[STATS_ID_TIME_LAST_SSA_MAD_RCV] = {"TIME_LAST_SSA_MAD_RCV", "Time of last MAD received" },
	[STATS_ID_TIME_LAST_ERR] = {"TIME_LAST_ERR", "Time of last error" },
	[STATS_ID_DB_EPOCH] = {"DB_EPOCH", "DB epoch" },
	[STATS_ID_IPV4_TBL_EPOCH] = {"IPV4_EPOCH", "IPv4 epoch" },
	[STATS_ID_IPV6_TBL_EPOCH] = {"IPV6_EPOCH", "IPv6 epoch" },
	[STATS_ID_NAME_TBL_EPOCH] = {"NAME_EPOCH", "Name epoch" },
};


static int stats_init(struct admin_command *cmd)
{
	int j;
	struct admin_count_command *count_cmd;

	count_cmd = (struct admin_count_command *) &cmd->data.count_cmd;

	for (j = 0; j < STATS_ID_LAST; ++j)
		count_cmd->include_list[j] = 1;

	count_cmd->print_all = 1;

	return 0;
}

static int stats_handle_param(struct admin_command *cmd, const char *param)
{
	int j;
	struct admin_count_command *count_cmd;

	count_cmd = (struct admin_count_command *) &cmd->data.count_cmd;

	if (count_cmd->print_all) {
		memset(count_cmd->include_list, '\0', sizeof(count_cmd->include_list));
		count_cmd->print_all = 0;
	}

	for (j = 0; j < STATS_ID_LAST; ++j) {
		if (!strcmp(param, stats_descr[j].name)) {
			count_cmd->include_list[j] = 1;
			break;
		}
	}
	if (j == STATS_ID_LAST) {
		fprintf(stderr, "ERROR - Name %s isn't found in the stats list\n", param);
		return -1;
	}

	return 0;
}

static void stats_print_help(FILE *stream)
{
	unsigned int i;

	fprintf(stream, "stats is a command for gathering runtime information from a SSA node\n");
	fprintf(stream, "Supported statistics:\n");

	for (i = 0; i < ARRAY_SIZE(stats_descr); ++i) {
		if (ssa_admin_stats_type[i] != ssa_stats_obsolete)
			fprintf(stream, "%-25s %-10s %s\n",
				stats_descr[i].name,
				ssa_stats_type_names[ssa_admin_stats_type[i]],
				stats_descr[i].description);
	}

	fprintf(stream, "\n\n");
}

int stats_command_create_msg(struct admin_command *cmd,
			     struct ssa_admin_msg *msg)
{
	struct ssa_admin_stats *stats_msg = &msg->data.stats;
	uint16_t n;

	(void)(cmd);

	stats_msg->n = htons(STATS_ID_LAST);
	n = ntohs(msg->hdr.len) + sizeof(*stats_msg);
	msg->hdr.len = htons(n);

	return 0;
}

static void stats_command_output(struct admin_command *cmd,
				 struct cmd_exec_info *exec_info,
				 union ibv_gid remote_gid,
				 const struct ssa_admin_msg *msg)
{
	int i, n;
	struct ssa_admin_stats *stats_msg = (struct ssa_admin_stats *) &msg->data.stats;
	struct admin_count_command *count_cmd = (struct admin_count_command *) &cmd->data.count_cmd;
	struct timeval epoch, timestamp;
	time_t timestamp_time;
	long val;
	char addr_buf[128];

	(void)(exec_info);

	n = min(STATS_ID_LAST, ntohs(stats_msg->n));

	epoch.tv_sec = ntohll(stats_msg->epoch_tv_sec);
	epoch.tv_usec = ntohll(stats_msg->epoch_tv_usec);

	for (i = 0; i < n; ++i) {
		if (!count_cmd->include_list[i])
			continue;

		val = ntohll(stats_msg->vals[i]);

		if (val < 0 && ssa_admin_stats_type[i] != ssa_stats_signed_numeric)
			continue;

		if (cmd->recursive && ssa_admin_stats_type[i] != ssa_stats_obsolete) {
			ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
					remote_gid.raw, sizeof remote_gid.raw);
			printf("%s: ", addr_buf);
		}

		switch (ssa_admin_stats_type[i]) {
			case ssa_stats_obsolete:
				continue;
				break;
			case ssa_stats_numeric:
			case ssa_stats_signed_numeric:
				printf("%s %ld\n", stats_descr[i].name, val);
				break;
			case ssa_stats_timestamp:
				timestamp.tv_sec = epoch.tv_sec + val / 1000;
				timestamp.tv_usec = epoch.tv_usec + (val % 1000) * 1000;
				timestamp_time =  timestamp.tv_sec;
				printf("%s ", stats_descr[i].name);
				ssa_write_date(stdout, timestamp_time, timestamp.tv_usec);
				printf("\n");
				break;
			default:
				continue;
		};
	}
}

static int nodeinfo_init(struct admin_command *cmd)
{
	cmd->data.nodeinfo_cmd.mode = NODEINFO_FULL;
	return 0;
}

static const char *ssa_connection_type_names[] = {
	[SSA_CONN_TYPE_UPSTREAM] = "Upstream",
	[SSA_CONN_TYPE_DOWNSTREAM] = "Downstream",
	[SSA_CONN_TYPE_LISTEN] = "Listen"
};

static const char *ssa_database_type_names[] = {
	[SSA_CONN_NODB_TYPE] = "NODB",
	[SSA_CONN_SMDB_TYPE] = "SMDB",
	[SSA_CONN_PRDB_TYPE] = "PRDB",
};

static void nodeinfo_connections_output(const struct ssa_admin_connection_info *connections,
					const int n, const char *node_addr_buf,
					const int type)
{
	int i;
	char addr_buf[128];
	struct timeval timestamp;
	time_t timestamp_time;

	for (i = 0; i < n; ++i) {
		if (connections[i].connection_type != type)
			continue;

		ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
				connections[i].remote_gid,
				sizeof connections[i].remote_gid);
		if (connections[i].connection_type >=
		    ARRAY_SIZE(ssa_connection_type_names)) {
			fprintf(stderr, "ERROR - Unknown connection type\n");
			continue;
		}
		if (connections[i].dbtype >= ARRAY_SIZE(ssa_database_type_names)) {
			fprintf(stderr, "ERROR - Unknown database type\n");
			continue;
		}

		timestamp.tv_sec = ntohll(connections[i].connection_tv_sec);
		timestamp.tv_usec = ntohll(connections[i].connection_tv_usec);

		timestamp_time = timestamp.tv_sec;

		printf("%s%s %u %s %s %s ", node_addr_buf, addr_buf,
		       ntohs(connections[i].remote_lid),
		       ssa_connection_type_names[connections[i].connection_type],
		       ssa_database_type_names[connections[i].dbtype],
		       ssa_node_type_str(connections[i].remote_type));
		ssa_write_date(stdout, timestamp_time, timestamp.tv_usec);
		printf("\n");
	}
}

static void nodeinfo_command_output(struct admin_command *cmd,
				    struct cmd_exec_info *exec_info,
				    union ibv_gid remote_gid,
				    const struct ssa_admin_msg *msg)
{
	int n, db_type;
	char node_addr_buf[128];
	struct ssa_admin_nodeinfo *nodeinfo_msg = (struct ssa_admin_nodeinfo *) &msg->data.nodeinfo;
	struct ssa_admin_connection_info *connections =
		(struct ssa_admin_connection_info *) nodeinfo_msg->connections;

	(void)(exec_info);

	if (cmd->recursive) {
		int addr_len;

		ssa_format_addr(node_addr_buf, sizeof node_addr_buf, SSA_ADDR_GID,
				remote_gid.raw, sizeof remote_gid.raw);
		addr_len = strlen(node_addr_buf);
		snprintf(node_addr_buf + strlen(node_addr_buf),
			 sizeof(node_addr_buf) - addr_len, "%*c: ",
			 GID_ADDRESS_WIDTH - addr_len, ' ');

	} else {
		node_addr_buf[0] = '\0';
	}

	if (nodeinfo_msg->type == SSA_NODE_CONSUMER)
		db_type = SSA_CONN_PRDB_TYPE;
	else if (nodeinfo_msg->type &
		 (SSA_NODE_CORE | SSA_NODE_ACCESS | SSA_NODE_DISTRIBUTION))
		db_type = SSA_CONN_SMDB_TYPE;
	else
		db_type = SSA_CONN_NODB_TYPE;

	if (cmd->data.nodeinfo_cmd.mode & NODEINFO_SINGLELINE)
		printf("%s%s %s (0x%" PRIx64") %s\n", node_addr_buf,
		       ssa_node_type_str(nodeinfo_msg->type),
		       ssa_database_type_names[db_type],
		       ntohll(nodeinfo_msg->db_epoch), nodeinfo_msg->version);

	n = ntohs(nodeinfo_msg->connections_num);

	if (cmd->data.nodeinfo_cmd.mode & NODEINFO_UP_CONN)
		nodeinfo_connections_output(connections, n, node_addr_buf,
					    SSA_CONN_TYPE_UPSTREAM);
	if (cmd->data.nodeinfo_cmd.mode & NODEINFO_DOWN_CONN)
		nodeinfo_connections_output(connections, n, node_addr_buf,
					    SSA_CONN_TYPE_DOWNSTREAM);
}

static int nodeinfo_handle_option(struct admin_command *admin_cmd,
				  char option, const char *optarg)
{
	unsigned int i;
	int ret;

	if (option != 'f')
		return 0;

	for (i = 0; i < ARRAY_SIZE(nodeinfo_format_options); ++i)
		if (!strcmp(nodeinfo_format_options[i].value, optarg))
			break;

	if (i < ARRAY_SIZE(nodeinfo_format_options)) {
		admin_cmd->data.nodeinfo_cmd.mode = nodeinfo_format_options[i].mode;
		ret = 0;
	} else {
		fprintf(stderr, "ERROR - wrong value in format option\n");
		ret = 1;
	}

	return ret;
}

static void nodeinfo_print_help(FILE *stream)
{
	unsigned int i;

	fprintf(stream, "Supported formating modes:\n");

	for (i = 0; i < ARRAY_SIZE(nodeinfo_format_options); ++i)
		fprintf(stream, "\t%s\t - \t%s\n",
			nodeinfo_format_options[i].value,
			nodeinfo_format_options[i].description);

	fprintf(stream, "\n\n");
}

static int disconnect_init(struct admin_command *cmd)
{
	struct admin_disconnect_command *disconnect_cmd;

	disconnect_cmd = (struct admin_disconnect_command *) &cmd->data.disconnect_cmd;
	memset(disconnect_cmd, '\0', sizeof(*disconnect_cmd));
	disconnect_cmd->type = SSA_ADDR_LID;

	return 0;
}

static int disconnect_handle_option(struct admin_command *admin_cmd,
				  char option, const char *optarg)
{
	unsigned int i;

	if (option != 'x')
		return 0;

	for (i = 0; i < ARRAY_SIZE(disconnect_format_options); ++i)
		if (!strcmp(disconnect_format_options[i].value, optarg))
			break;

	if (i < ARRAY_SIZE(disconnect_format_options)) {
		admin_cmd->data.disconnect_cmd.type = disconnect_format_options[i].mode;
		return 0;
	} else {
		fprintf(stderr, "ERROR - wrong value in format option\n");
		return 1;
	}
}

static int disconnect_handle_param(struct admin_command *cmd, const char *param)
{
	struct admin_disconnect_command *disconnect_cmd;
	long int tmp;
	char *endptr;

	disconnect_cmd = (struct admin_disconnect_command *) &cmd->data.disconnect_cmd;
	if (disconnect_cmd->type == SSA_ADDR_GID) {
		if(1 != inet_pton(AF_INET6, param, &disconnect_cmd->id.gid)) {
			fprintf(stderr, "ERROR - param %s is not IB GID\n", param);
			return 1;
		}
	} else if (disconnect_cmd->type == SSA_ADDR_LID) {
		tmp = strtol(param, &endptr, 10);
		if (endptr == param) {
			fprintf(stderr, "ERROR - no digits were found in param -%s\n", param);
			return 1;
		}
		if (errno == ERANGE && (tmp == LONG_MAX || tmp == LONG_MIN) ) {
			fprintf(stderr, "ERROR - out of range in param -%s\n", param);
			return 1;
		}
		if (tmp <0 || tmp >= IB_LID_MCAST_START) {
			fprintf(stderr, "ERROR - invalid lid %ld in param %s\n", tmp, param);
			return 1;
		}

		disconnect_cmd->id.lid = tmp;
	} else {
		fprintf(stderr, "ERROR - Unsupported type of connection %d\n", disconnect_cmd->type);
		return 1;
	}

	return 0;
}

static int disconnect_command_create_msg(struct admin_command *cmd,
					 struct ssa_admin_msg *msg)
{
	struct ssa_admin_disconnect *disconnect_msg = &msg->data.disconnect;
	struct admin_disconnect_command *disconnect_cmd;
	uint16_t n;

	disconnect_cmd = (struct admin_disconnect_command *) &cmd->data.disconnect_cmd;
	disconnect_msg->type = disconnect_cmd->type;
	if (disconnect_msg->type == SSA_ADDR_LID)
		disconnect_msg->id.lid = htons(disconnect_cmd->id.lid);
	else
		memcpy(disconnect_msg->id.gid, disconnect_cmd->id.gid.raw, sizeof(disconnect_msg->id.gid));
	n = ntohs(msg->hdr.len) + sizeof(*disconnect_msg);
	msg->hdr.len = htons(n);

	return 0;
}

static void disconnect_command_output(struct admin_command *cmd,
				      struct cmd_exec_info *exec_info,
				      union ibv_gid remote_gid,
				      const struct ssa_admin_msg *msg)
{
	struct admin_disconnect_command *disconnect_cmd;
	char addr_buf[128];

	disconnect_cmd = (struct admin_disconnect_command *) &cmd->data.disconnect_cmd;
	if (disconnect_cmd->type == SSA_ADDR_GID)
		ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
				disconnect_cmd->id.gid.raw, sizeof disconnect_cmd->id.gid.raw);
	else
		ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_LID,
				(void *)&disconnect_cmd->id.lid, sizeof disconnect_cmd->id.lid);

	printf("Node %s was disconnected\n", addr_buf);

}

struct cmd_opts *admin_get_cmd_opts(int cmd)
{
	struct cmd_struct_impl *impl;

	if (cmd <= SSA_ADMIN_CMD_NONE || cmd >= SSA_ADMIN_CMD_MAX) {
		fprintf(stderr, "ERROR - command index %d is out of range\n", cmd);
		return NULL;
	}

	impl = &admin_cmd_command_impls[cmd];

	return impl->opts;
}

const struct cmd_help *admin_cmd_help(int cmd)
{
	struct cmd_struct_impl *impl;

	if (cmd <= SSA_ADMIN_CMD_NONE || cmd >= SSA_ADMIN_CMD_MAX) {
		fprintf(stderr, "ERROR - command index %d is out of range\n", cmd);
		return NULL;
	}

	impl = &admin_cmd_command_impls[cmd];

	return &impl->help;
}


enum admin_connection_state {
	ADM_CONN_CONNECTING,
	ADM_CONN_NODEINFO,
	ADM_CONN_COMMAND,
};

struct admin_connection {
	union ibv_gid remote_gid;
	uint16_t remote_lid;
	enum admin_connection_state state;
	uint64_t epoch;
	unsigned int slen, sleft;
	struct ssa_admin_msg *smsg;
	unsigned int rlen, rcount;
	struct ssa_admin_msg *rmsg;
	struct ssa_admin_msg_hdr rhdr;
	struct cmd_exec_info exec_info;
};

static int admin_recv_buff(int rsock, char *buf, unsigned int *rcount,
			   unsigned int rlen)
{
	int n;

	while (*rcount < rlen) {
		n = rrecv(rsock, buf + *rcount, rlen - *rcount, MSG_DONTWAIT);
		if (n > 0)
			*rcount += n;
		else if (!n)
			return -ECONNRESET;
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
			return *rcount;
		else
			return n;
	}
	return 0;
}

static int admin_recv_msg(struct pollfd *pfd, struct admin_connection *conn)
{
	int ret;

	if (conn->rcount < sizeof(conn->rhdr)) {
		ret = admin_recv_buff(pfd->fd, (char *) &conn->rhdr,
				      &conn->rcount, sizeof(conn->rhdr));
		if (ret == -ECONNRESET) {
			fprintf(stderr, "ERROR - SSA node closed admin connection\n");
			return -1;
		} else if (ret < 0) {
			fprintf(stderr,
				"ERROR - rrecv failed: %d (%s) on rsock %d\n",
				errno, strerror(errno), pfd->fd);
			return ret;
		}
		if (conn->rcount < sizeof(conn->rhdr))
			return 0;
	}

	if (conn->rcount == sizeof(conn->rhdr)) {
		if (conn->rhdr.status != SSA_ADMIN_STATUS_SUCCESS) {
			fprintf(stderr, "ERROR - target SSA node failed to process request\n");
			return -1;
		} else if (conn->rhdr.method != SSA_ADMIN_METHOD_RESP) {
			fprintf(stderr, "ERROR - response has wrong method\n");
			return -1;
		}

		conn->rlen = ntohs(conn->rhdr.len);
		conn->rmsg = (struct ssa_admin_msg *) malloc(max(sizeof(*conn->rmsg),
								 conn->rlen));
		if (!conn->rmsg) {
			fprintf(stderr, "ERROR - failed allocate message buffer\n");
			return -1;
		}

		conn->rmsg->hdr = conn->rhdr;
	}

	ret = admin_recv_buff(pfd->fd, (char *) conn->rmsg,
			      &conn->rcount, conn->rlen);
	if (ret == -ECONNRESET) {
		fprintf(stderr, "ERROR - SSA node closed admin connection\n");
		return -1;
	} else if (ret < 0) {
		fprintf(stderr, "ERROR - rrecv failed: %d (%s) on rsock %d\n",
			errno, strerror(errno), pfd->fd);
		return ret;
	}

	return 0;
}

static int admin_send_msg(struct pollfd *pfd, struct admin_connection *conn)
{
	int sent = conn->slen - conn->sleft;
	int n;

	while (conn->sleft) {
		n  = rsend(pfd->fd, (char *) conn->smsg + sent,
			   conn->sleft, MSG_DONTWAIT);
		if (n < 0)
			break;
		conn->sleft -= n;
		sent += n;
	}

	if (!conn->sleft) {
		return 0;
	} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	} else if (n < 0)
		return n;

	return 0;
}

static void admin_update_connection_state(struct admin_connection *conn,
					  enum admin_connection_state state,
					  struct ssa_admin_msg *msg)
{
	conn->state = state;
	conn->epoch  = get_timestamp();

	free(conn->rmsg);
	conn->rmsg = NULL;
	conn->rlen = 0;
	conn->rcount = 0;

	if (state == ADM_CONN_CONNECTING) {
		conn->smsg = NULL;
		conn->slen = 0;
		conn->sleft = 0;
	} else {
		conn->smsg = msg;
		conn->slen = ntohs(msg->hdr.len);
		conn->sleft = conn->slen;
	}

	conn->exec_info.stime = get_timestamp();
}

static void admin_close_connection(struct pollfd *pfd,
				   struct admin_connection *conn)
{
	if (pfd->fd > 0)
		rclose(pfd->fd);
	pfd->fd = -1;
	pfd->events = 0;
	pfd->revents = 0;

	free(conn->rmsg);
	conn->rmsg = NULL;
}

static int admin_connect_new_nodes(struct pollfd **fds,
				   struct admin_connection **admin_conns,
				   enum admin_recursion_mode mode,
				   int *admin_conns_num,
				   const struct ssa_admin_msg *rmsg)
{
	int i, slot, node_conns_num, rsock;
	char addr_buf[128];
	struct ssa_admin_nodeinfo *nodeinfo = (struct ssa_admin_nodeinfo *) &rmsg->data.nodeinfo;
	struct ssa_admin_connection_info *node_conns =
		(struct ssa_admin_connection_info *) nodeinfo->connections;
	void *tmp;
	int type;

	node_conns_num = ntohs(nodeinfo->connections_num);
	if (node_conns_num < 0) {
		fprintf(stderr, "ERROR - Negative number of SSA node's connections\n");
		return -1;
	} else if (node_conns_num == 0) {
		return 0;
	}

	if (mode == ADMIN_RECURSION_DOWN)
		type = SSA_CONN_TYPE_DOWNSTREAM;
	else
		type = SSA_CONN_TYPE_UPSTREAM;

	slot = 0;
	for (i = 0; i < node_conns_num; ++i) {
		if (node_conns[i].connection_type == type) {
			for (; slot < *admin_conns_num && (*fds)[slot].fd > 0; slot++);

			if (slot == *admin_conns_num) {
				tmp = realloc(*fds, 2 * *admin_conns_num * sizeof(**fds));
				if (!tmp) {
					fprintf(stderr, "ERROR - failed to reallocate pfds array\n");
					return -1;
				}

				*fds = (struct pollfd *) tmp;

				tmp = realloc(*admin_conns, 2 * *admin_conns_num * sizeof(**admin_conns));
				if (!tmp) {
					fprintf(stderr, "ERROR - failed to reallocate connections array\n");
					return -1;
				}

				*admin_conns_num *= 2;
			}

			ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
					node_conns[i].remote_gid,
					sizeof node_conns[i].remote_gid);
			rsock = admin_connect_init(addr_buf, ADMIN_ADDR_TYPE_GID,
						   &global_opts);
			if (rsock < 0 && (errno != EINPROGRESS)) {
				fprintf(stderr, "ERROR - Unable connect to %s\n",
					addr_buf);
				continue;
			}

			(*fds)[slot].fd = rsock;
			(*fds)[slot].events = POLLOUT;
			(*fds)[slot].revents = 0;

			admin_update_connection_state(*admin_conns + slot,
						      ADM_CONN_CONNECTING, NULL);
			(*admin_conns)[slot].remote_lid = ntohs(node_conns[i].remote_lid);
			memcpy((*admin_conns)[slot].remote_gid.raw,
			       &node_conns[i].remote_gid,
			       sizeof((*admin_conns)[slot].remote_gid.raw));
		}
	}
	return 0;
}

int admin_exec_recursive(int rsock, int cmd, enum admin_recursion_mode mode,
			 int argc, char **argv)
{
	struct cmd_struct_impl *nodeinfo_impl;
	struct admin_command *nodeinfo_cmd;
	struct ssa_admin_msg nodeinfo_msg, msg;
	struct admin_command *admin_cmd = NULL;
	struct cmd_struct_impl *cmd_impl = NULL;
	int n = 1024, ret, i, revents, err;
	struct pollfd *fds;
	unsigned int len;
	struct admin_connection *connections;
	struct sockaddr_ib peer_addr;
	socklen_t peer_len;
	char addr_buf[128];

	if (cmd <= SSA_ADMIN_CMD_NONE || cmd >= SSA_ADMIN_CMD_MAX) {
		fprintf(stderr, "ERROR - command index %d is out of range\n", cmd);
		return -1;
	}

	fds = (struct pollfd *) malloc(n * sizeof(*fds));
	if (!fds) {
		fprintf(stderr, "ERROR - failed to allocate pollfd array\n");
		return -1;
	}

	for (i = 0; i < n; ++i) {
		fds[i].fd = -1;
		fds[i].events = 0;
		fds[i].revents = 0;
	}

	connections = (struct admin_connection *) malloc(n * sizeof(*connections));
	if (!connections) {
		fprintf(stderr, "ERROR - failed to allocate admin connections array\n");
		free(fds);
		return -1;
	}

	memset(connections, 0, n * sizeof(*connections));

	nodeinfo_impl = &admin_cmd_command_impls[SSA_ADMIN_CMD_NODEINFO];
	nodeinfo_cmd = default_init(SSA_ADMIN_CMD_NODEINFO, 0, NULL);
	if (!nodeinfo_cmd) {
		fprintf(stderr, "ERROR - failed to create nodeinfo command\n");
		free(connections);
		free(fds);
		return -1;
	}

	memset(&nodeinfo_msg, 0, sizeof(nodeinfo_msg));
	nodeinfo_msg.hdr.version= SSA_ADMIN_PROTOCOL_VERSION;
	nodeinfo_msg.hdr.method	= SSA_ADMIN_METHOD_GET;
	nodeinfo_msg.hdr.opcode	= htons(SSA_ADMIN_CMD_NODEINFO);
	nodeinfo_msg.hdr.len	= htons(sizeof(nodeinfo_msg.hdr));

	ret = nodeinfo_impl->create_request(nodeinfo_cmd, &nodeinfo_msg);
	if (ret < 0) {
		fprintf(stderr, "ERROR - message creation error\n");
		goto err;
	}
	cmd_impl = &admin_cmd_command_impls[cmd];

	if (!cmd_impl->destroy ||
	    !cmd_impl->create_request || !cmd_impl->handle_response) {
		fprintf(stderr, "ERROR - command creation error\n");
		goto err;
	}

	admin_cmd = default_init(cmd, argc, argv);
	if (!admin_cmd) {
		fprintf(stderr, "ERROR - command creation error\n");
		goto err;
	}

	memset(&msg, 0, sizeof(msg));
	msg.hdr.version	= SSA_ADMIN_PROTOCOL_VERSION;
	msg.hdr.method	= SSA_ADMIN_METHOD_GET;
	msg.hdr.opcode	= htons(admin_cmd->cmd->id);
	msg.hdr.len	= htons(sizeof(msg.hdr));

	ret = admin_cmd->impl->create_request(admin_cmd, &msg);
	if (ret < 0) {
		fprintf(stderr, "ERROR - message creation error\n");
		goto err;
	}

	fds[0].fd = rsock;
	fds[0].events = POLLOUT;
	fds[0].revents = 0;

	if (mode == ADMIN_RECURSION_NONE) {
		admin_cmd->recursive = 0;
		admin_update_connection_state(&connections[0], ADM_CONN_COMMAND,
					      &msg);
	} else {
		admin_cmd->recursive = 1;
		admin_update_connection_state(&connections[0], ADM_CONN_NODEINFO,
					      &nodeinfo_msg);
	}

	peer_len = sizeof(peer_addr);
	if (!rgetpeername(rsock, (struct sockaddr *) &peer_addr, &peer_len)) {
		if (peer_addr.sib_family == AF_IB) {
			memcpy(&connections[0].remote_gid,
			       &peer_addr.sib_addr, sizeof(union ibv_gid));
		} else {
			fprintf(stderr, "ERROR - "
				"rgetpeername fd %d family %d not AF_IB\n",
				rsock, peer_addr.sib_family);
			goto err;
		}
	} else {
		fprintf(stderr, "ERROR - "
			"rgetpeername rsock %d ERROR %d (%s)\n",
			rsock, errno, strerror(errno));
		goto err;
	}

	ssa_format_addr(addr_buf, sizeof addr_buf, SSA_ADDR_GID,
			connections[0].remote_gid.raw,
			sizeof connections[0].remote_gid.raw);
	if (!strcmp(addr_buf, local_gid)) {
		struct ibv_path_data route;
		socklen_t route_len;

		route_len = sizeof(route);
		if (!rgetsockopt(rsock, SOL_RDMA, RDMA_ROUTE, &route, &route_len)) {
			memcpy(&connections[0].remote_gid,
			       &route.path.sgid, sizeof(union ibv_gid));
		} else {
			fprintf(stderr, "ERROR - "
				"rgetsockopt rsock %d ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
			goto err;
		}
	}

	for (;;) {
		for (i = 0; i < n && fds[i].fd < 0; ++i);
		if (i == n)
			break;

		ret = rpoll(fds, n, timeout);
		if (ret < 0) {
			fprintf(stderr, "ERROR - rpoll rsock %d ERROR %d (%s)\n",
				rsock, errno, strerror(errno));
			goto err;

		} else if (ret == 0) {
			uint64_t now_epoch = get_timestamp();

			for (i = 0; i < n; ++i) {
				if (fds[i].fd >= 0 && now_epoch -  connections[i].epoch >= timeout) {
					fprintf(stderr, "ERROR - timeout expired\n");
					admin_close_connection(&fds[i], &connections[i]);
				}
			}
			continue;
		}

		for (i = 0; i < n; ++i) {
			if (fds[i].fd < 0 || !fds[i].revents)
				continue;

			revents = fds[i].revents;
			fds[i].revents = 0;

			connections[i].epoch  = get_timestamp();

			if (revents & (POLLERR /*| POLLHUP */| POLLNVAL)) {
				char event_str[128] = {};

				ssa_format_event(event_str, sizeof(event_str), revents);
				fprintf(stderr,
					"ERROR - error event 0x%x (%s) on rsock %d\n",
					fds[i].revents, event_str, fds[i].fd);
				admin_close_connection(&fds[i], &connections[i]);
				continue;
			}

			if (revents & POLLIN) {
				ret = admin_recv_msg(&fds[i], &connections[i]);
				if (ret) {
					admin_close_connection(&fds[i], &connections[i]);
					continue;
				} else if (connections[i].rcount != connections[i].rlen) {
					continue;
				}

				SSA_ADMIN_REPORT_MSG(connections[i].rmsg);
				ret = 0;
				connections[i].exec_info.etime = get_timestamp();
				if (ntohs(connections[i].rmsg->hdr.opcode) == SSA_ADMIN_CMD_NODEINFO &&
				    connections[i].state == ADM_CONN_NODEINFO) {
					ret = admin_connect_new_nodes(&fds, &connections, mode, &n, connections[i].rmsg);
					if (ret) {
						fprintf(stderr, "WARNING - failed to connect downstream nodes\n");
						continue;
					}
					if (cmd != SSA_ADMIN_CMD_NODEINFO) {
						admin_update_connection_state(&connections[i], ADM_CONN_COMMAND, &msg);
						fds[i].events = POLLOUT;
						continue;
					}
				}
				cmd_impl->handle_response(admin_cmd, &connections[i].exec_info,
						connections[i].remote_gid, connections[i].rmsg);
				admin_close_connection(&fds[i], &connections[i]);
				continue;
			}
			if (revents & POLLOUT) {
				if (connections[i].state == ADM_CONN_CONNECTING) {
					len = sizeof(err);
					ret = rgetsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
					if (ret) {
						fprintf(stderr,
							"rgetsockopt rsock %d ERROR %d (%s)\n",
							fds[i].fd, errno,
							strerror(errno));
						admin_close_connection(&fds[i], &connections[i]);
						continue;
					}
					if (err) {
						ret = -1;
						errno = err;
						fprintf(stderr,
							"ERROR - async rconnect rsock %d ERROR %d (%s)\n",
							fds[i].fd, errno,
							strerror(errno));
						admin_close_connection(&fds[i], &connections[i]);
						continue;
					}

					admin_update_connection_state(&connections[i],
								      ADM_CONN_NODEINFO,
								      &nodeinfo_msg);
				}

				ret = admin_send_msg(&fds[i], &connections[i]);
				if (ret) {
					fprintf(stderr, "ERROR - response has wrong method\n");
					admin_close_connection(&fds[i],  &connections[i]);
				}

				SSA_ADMIN_REPORT_MSG(connections[i].smsg);
				if (!connections[i].sleft)
					fds[i].events = POLLIN;
			}
		}
	}
err:
	cmd_impl->destroy(admin_cmd);
	nodeinfo_impl->destroy(nodeinfo_cmd);
	free(connections);
	free(fds);

	return ret;
}
