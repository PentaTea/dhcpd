/* (c) 2013 Fritz Conrad Grimpen */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <error.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include <ev.h>
#include <sqlite3.h>

#include "array.h"
#include "dhcp.h"

#define RECV_BUF_LEN 4096
#define SEND_BUF_LEN 4096

sqlite3 *leasedb;

struct sockaddr_in server_id;
struct sockaddr_in broadcast = {
	.sin_family = AF_INET,
	.sin_addr = {INADDR_BROADCAST},
};

uint8_t recv_buffer[RECV_BUF_LEN];
uint8_t send_buffer[SEND_BUF_LEN];

bool debug = true;

static const char *BROKEN_SOFTWARE_NOTIFICATION = 
"#################################### ALERT ####################################\n"
"  BROKEN SOFTWARE NOTIFICATION - SOMETHING SENDS INVALID DHCP MESSAGES IN YOUR\n"
"                                    NETWORK\n";

#define MAC_ADDRSTRLEN 18

/* This function converts a L2 MAC address in binary format to a text format */
static int mac_ntop(char *addr, char *dst, size_t s)
{
	return snprintf(dst, s,
		"%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX", 
		addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/* Return netmask for specified prefix length */
static uint32_t netmask_from_prefixlen(uint8_t prefixlen)
{
	return htonl(0xFFFFFFFFU - (1 << (32 - prefixlen)) + 1);
}

static void discover_cb(EV_P_ ev_io *w, struct dhcp_msg *msg)
{
	int sqlerr, err;
	sqlite3_stmt *ldb_query;

	sqlerr = sqlite3_prepare_v2(leasedb,
		"SELECT address, routers, nameservers, prefixlen, leasetime "
		"FROM leases "
		"WHERE hwaddr = ?;", -1, &ldb_query, NULL);
	if (sqlerr != SQLITE_OK)
	{
		error(0, 0, "sqlite3: %s", sqlite3_errmsg(leasedb));
		return;
	}

	sqlerr = sqlite3_bind_text(ldb_query, 1, msg->chaddr, -1, NULL);
	if (sqlerr != SQLITE_OK)
		goto sql_error;

	sqlerr = sqlite3_step(ldb_query);

	if (sqlerr != SQLITE_ROW)
	{
		if (sqlerr != SQLITE_DONE)
			goto sql_error;
		sqlite3_finalize(ldb_query);
		return;
	}

	struct {
		const char *address;
		const char *routers;
		const char *nameservers;
		uint8_t prefixlen;
		uint32_t leasetime;
	} lease_entry;

	lease_entry.address = (char*)sqlite3_column_text(ldb_query, 0);
	lease_entry.routers = (char*)sqlite3_column_text(ldb_query, 1);
	lease_entry.nameservers = (char*)sqlite3_column_text(ldb_query, 2);
	lease_entry.prefixlen = sqlite3_column_int(ldb_query, 3);
	lease_entry.leasetime = sqlite3_column_int(ldb_query, 4);

	struct in_addr address, routers[4], nameservers[4];

	if (!inet_pton(AF_INET, lease_entry.address, &address))
		goto invalid_lease_entry;

	if (!inet_pton(AF_INET, lease_entry.routers, routers))
		goto invalid_lease_entry;

	if (!inet_pton(AF_INET, lease_entry.nameservers, nameservers))
		goto invalid_lease_entry;

	if (0)
	{
invalid_lease_entry:
		fprintf(stderr, "Invalid lease entry for %s:\n"
				"\tAddress: %s\n"
				"\tRouters: %s\n"
				"\tNameservers: %s\n"
				"\tPrefix Length: %hhu\n"
				"\tLease Time: %u\n",
				msg->chaddr,
				lease_entry.address,
				lease_entry.routers,
				lease_entry.nameservers,
				lease_entry.prefixlen,
				lease_entry.leasetime);
		sqlite3_finalize(ldb_query);
		return;
	}

	/* Calculate netmask value for the defined prefix length */
	uint32_t netmask = netmask_from_prefixlen(lease_entry.prefixlen);

	sqlite3_finalize(ldb_query);

	/* Prepare DHCP message to send */
	size_t send_len = DHCP_MSG_HDRLEN;
	memset(send_buffer, 0, DHCP_MSG_LEN);
	dhcp_msg_prepare(send_buffer, msg->data);

	COPY_ARRAY(DHCP_MSG_F_SIADDR(send_buffer), &server_id.sin_addr, 4);

	*((struct in_addr *)DHCP_MSG_F_YIADDR(send_buffer)) = address;

	uint8_t *options = DHCP_MSG_F_OPTIONS(send_buffer);

	options[0] = DHCP_OPT_MSGTYPE;
	options[1] = 1;
	options[2] = DHCPOFFER;
	options = DHCP_OPT_NEXT(options);
	send_len += 3;

	options[0] = DHCP_OPT_NETMASK;
	options[1] = 4;
	COPY_ARRAY((options + 2), (uint8_t*)&netmask, 4);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_ROUTER;
	options[1] = 4;
	*(struct in_addr *)(options + 2) = routers[0];
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_SERVERID;
	options[1] = 4;
	COPY_ARRAY((options + 2), &server_id.sin_addr, 4);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_LEASETIME;
	options[1] = 4;
	*(uint32_t*)(options + 2) = htonl(lease_entry.leasetime);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_DNS;
	options[1] = 4;
	*(struct in_addr *)(options + 2) = nameservers[0];
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	*options = DHCP_OPT_END;
	options = DHCP_OPT_NEXT(options);
	send_len += 1;

	err = sendto(w->fd,
		send_buffer, send_len,
		MSG_DONTWAIT, (struct sockaddr *)&broadcast, sizeof broadcast);

	if (err < 0)
		error(0, 1, "Could not send DHCPOFFER");

	return;
sql_error:

	error(0, 0, "sqlite3: %s", sqlite3_errmsg(leasedb));
	sqlite3_finalize(ldb_query);
}

static void request_cb(EV_P_ ev_io *w, struct dhcp_msg *msg)
{
	struct in_addr *requested_addr, *requested_server;
	uint8_t *options;
	struct dhcp_opt current_opt;

	requested_addr = NULL;
	requested_server = (struct in_addr *)DHCP_MSG_F_SIADDR(msg->data);
	options = DHCP_MSG_F_OPTIONS(msg->data);

	while (dhcp_opt_next(&options, &current_opt, msg->end))
		switch (current_opt.code)
		{
			case 50:
				requested_addr = (struct in_addr *)current_opt.data;
				break;
			case 54:
				requested_server = (struct in_addr *)current_opt.data;
				break;
		}

	if (requested_server->s_addr != server_id.sin_addr.s_addr)
		return;

	int sqlerr, err;
	sqlite3_stmt *ldb_query;

	sqlerr = sqlite3_prepare_v2(leasedb,
		"SELECT address, routers, nameservers, prefixlen, leasetime "
		"FROM leases "
		"WHERE hwaddr = ?;", -1, &ldb_query, NULL);
	if (sqlerr != SQLITE_OK)
	{
		error(0, 0, "sqlite3: %s", sqlite3_errmsg(leasedb));
		return;
	}

	sqlerr = sqlite3_bind_text(ldb_query, 1, msg->chaddr, -1, NULL);
	if (sqlerr != SQLITE_OK)
		goto sql_error;

	sqlerr = sqlite3_step(ldb_query);
	if (sqlerr != SQLITE_ROW)
	{
		if (sqlerr != SQLITE_DONE)
			goto sql_error;
		sqlite3_finalize(ldb_query);
		goto nack;
	}

	struct {
		const char *address;
		const char *routers;
		const char *nameservers;
		uint8_t prefixlen;
		uint32_t leasetime;
	} lease_entry;

	lease_entry.address = (char*)sqlite3_column_text(ldb_query, 0);
	lease_entry.routers = (char*)sqlite3_column_text(ldb_query, 1);
	lease_entry.nameservers = (char*)sqlite3_column_text(ldb_query, 2);
	lease_entry.prefixlen = sqlite3_column_int(ldb_query, 3);
	lease_entry.leasetime = sqlite3_column_int(ldb_query, 4);

	struct in_addr address, routers[4], nameservers[4];

	if (!inet_pton(AF_INET, lease_entry.address, &address))
		goto invalid_lease_entry;

	if (!inet_pton(AF_INET, lease_entry.routers, routers))
		goto invalid_lease_entry;

	if (!inet_pton(AF_INET, lease_entry.nameservers, nameservers))
		goto invalid_lease_entry;

	if (0)
	{
invalid_lease_entry:
		fprintf(stderr, "Invalid lease entry for %s:\n"
				"\tAddress: %s\n"
				"\tRouters: %s\n"
				"\tNameservers: %s\n"
				"\tPrefix Length: %hhu\n"
				"\tLease Time: %u\n",
				msg->chaddr,
				lease_entry.address,
				lease_entry.routers,
				lease_entry.nameservers,
				lease_entry.prefixlen,
				lease_entry.leasetime);
		sqlite3_finalize(ldb_query);
		goto nack;
	}

	uint32_t netmask = netmask_from_prefixlen(lease_entry.prefixlen);

	sqlite3_finalize(ldb_query);

	if (memcmp(&address, requested_addr, 4) != 0)
	{
		size_t send_len;
nack:
		send_len = DHCP_MSG_HDRLEN;
		memset(send_buffer, 0, DHCP_MSG_LEN);
		dhcp_msg_prepare(send_buffer, msg->data);

		COPY_ARRAY(DHCP_MSG_F_SIADDR(send_buffer), &server_id.sin_addr, 4);

		uint8_t *options = DHCP_MSG_F_OPTIONS(send_buffer);

		options[0] = DHCP_OPT_MSGTYPE;
		options[1] = 1;
		options[2] = DHCPNAK;
		options = DHCP_OPT_NEXT(options);
		send_len += 3;

		options[0] = DHCP_OPT_END;
		options = DHCP_OPT_NEXT(options);
		send_len += 1;

		err = sendto(w->fd,
			send_buffer, send_len,
			MSG_DONTWAIT, (struct sockaddr *)&broadcast, sizeof broadcast);

		if (err < 0)
			error(0, 1, "Could not send DHCPNAK");
		
		return;
	}

	/* Prepare DHCP message to send */
	size_t send_len = DHCP_MSG_HDRLEN;
	memset(send_buffer, 0, DHCP_MSG_LEN);
	dhcp_msg_prepare(send_buffer, msg->data);

	COPY_ARRAY(DHCP_MSG_F_SIADDR(send_buffer), &server_id.sin_addr, 4);

	*((struct in_addr *)DHCP_MSG_F_YIADDR(send_buffer)) = address;

	options = DHCP_MSG_F_OPTIONS(send_buffer);

	options[0] = DHCP_OPT_MSGTYPE;
	options[1] = 1;
	options[2] = DHCPACK;
	options = DHCP_OPT_NEXT(options);
	send_len += 3;

	options[0] = DHCP_OPT_NETMASK;
	options[1] = 4;
	COPY_ARRAY((options + 2), (uint8_t*)&netmask, 4);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_ROUTER;
	options[1] = 4;
	*(struct in_addr *)(options + 2) = routers[0];
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_SERVERID;
	options[1] = 4;
	COPY_ARRAY((options + 2), &server_id.sin_addr, 4);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_LEASETIME;
	options[1] = 4;
	*(uint32_t*)(options + 2) = htonl(lease_entry.leasetime);
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	options[0] = DHCP_OPT_DNS;
	options[1] = 4;
	*(struct in_addr *)(options + 2) = nameservers[0];
	options = DHCP_OPT_NEXT(options);
	send_len += 6;

	*options = DHCP_OPT_END;
	options = DHCP_OPT_NEXT(options);
	send_len += 1;

	err = sendto(w->fd,
		send_buffer, send_len,
		MSG_DONTWAIT, (struct sockaddr *)&broadcast, sizeof broadcast);

	if (err < 0)
		error(0, 1, "Could not send DHCPACK");

	return;
sql_error:

	error(0, 0, "sqlite3: %s", sqlite3_errmsg(leasedb));
	sqlite3_finalize(ldb_query);
}

static void release_cb(EV_P_ ev_io *w, struct dhcp_msg *msg)
{
}

static void decline_cb(EV_P_ ev_io *w, struct dhcp_msg *msg)
{
}

static void inform_cb(EV_P_ ev_io *w, struct dhcp_msg *msg)
{
}

static void req_cb(EV_P_ ev_io *w, int revents)
{
	struct sockaddr_in src_addr;
	socklen_t src_addrlen;

	/* Receive data from socket */
	ssize_t recvd = recvfrom(
		w->fd,
		recv_buffer,
		RECV_BUF_LEN,
		MSG_DONTWAIT,
		(struct sockaddr * restrict)&src_addr, &src_addrlen);

	/* Detect errors */
	if (recvd < 0)
		return;
	/* Detect too small messages */
	if (recvd < 241)
		return;
	/* Check magic value */
	uint8_t *magic = DHCP_MSG_F_MAGIC(recv_buffer);
	if (!DHCP_MSG_MAGIC_CHECK(magic))
		return;

	/* Convert addresses to strings */
	char ciaddr[INET_ADDRSTRLEN],
			 yiaddr[INET_ADDRSTRLEN],
			 siaddr[INET_ADDRSTRLEN],
			 giaddr[INET_ADDRSTRLEN],
			 chaddr[MAC_ADDRSTRLEN],
			 srcaddr[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, DHCP_MSG_F_CIADDR(recv_buffer), ciaddr, sizeof ciaddr);
	inet_ntop(AF_INET, DHCP_MSG_F_YIADDR(recv_buffer), yiaddr, sizeof yiaddr);
	inet_ntop(AF_INET, DHCP_MSG_F_SIADDR(recv_buffer), siaddr, sizeof siaddr);
	inet_ntop(AF_INET, DHCP_MSG_F_GIADDR(recv_buffer), giaddr, sizeof giaddr);
	inet_ntop(AF_INET, &src_addr.sin_addr, srcaddr, sizeof srcaddr);
	mac_ntop(DHCP_MSG_F_CHADDR(recv_buffer), chaddr, sizeof chaddr);

	/* Extract message type from options */
	uint8_t *options = DHCP_MSG_F_OPTIONS(recv_buffer);
	struct dhcp_opt current_option;

	enum dhcp_msg_type msg_type = 0;

	while (dhcp_opt_next(&options, &current_option, (uint8_t*)(recv_buffer + recvd)))
		if (current_option.code == 53)
			msg_type = (enum dhcp_msg_type)current_option.data[0];

	if (debug)
		fprintf(stderr,
			"DHCP message from %s:\n"
			"\tOP %hhu [%s]\n"
			"\tHTYPE %hhu HLEN %hhu\n"
			"\tHOPS %hhu\n"
			"\tXID %X\n"
			"\tSECS %hu FLAGS %hu\n"
			"\tCIADDR %s YIADDR %s SIADDR %s GIADDR %s\n"
			"\tCHADDR %s\n"
			"\tMAGIC %X\n"
			"\tMSG TYPE %u\n",
			srcaddr,
			*DHCP_MSG_F_OP(recv_buffer),
			(*DHCP_MSG_F_OP(recv_buffer) == 1 ? "REQUEST" : "REPLY"),
			*DHCP_MSG_F_HTYPE(recv_buffer),
			*DHCP_MSG_F_HLEN(recv_buffer),
			*DHCP_MSG_F_HOPS(recv_buffer),
			*DHCP_MSG_F_XID(recv_buffer),
			*DHCP_MSG_F_SECS(recv_buffer),
			*DHCP_MSG_F_FLAGS(recv_buffer),
			ciaddr, yiaddr, siaddr, giaddr,
			chaddr,
			*DHCP_MSG_F_MAGIC(recv_buffer),
			msg_type);

	struct dhcp_msg msg = {
		.data = recv_buffer,
		.end = recv_buffer + recvd,
		.length = recvd,
		.type = msg_type,
		.ciaddr = ciaddr,
		.yiaddr = yiaddr,
		.siaddr = siaddr,
		.giaddr = giaddr,
		.chaddr = chaddr,
		.srcaddr = srcaddr,
		.source = (struct sockaddr *)&src_addr
	};

	switch (msg_type)
	{
		case DHCPDISCOVER:
			discover_cb(EV_A_ w, &msg);
			break;

		case DHCPREQUEST:
			request_cb(EV_A_ w, &msg);
			break;

		case DHCPRELEASE:
			release_cb(EV_A_ w, &msg);
			break;

		case DHCPDECLINE:
			decline_cb(EV_A_ w, &msg);
			break;

		case DHCPINFORM:
			inform_cb(EV_A_ w, &msg);
			break;

		default:
			fprintf(stderr, BROKEN_SOFTWARE_NOTIFICATION);
			break;
	}
}

int main(int argc, char **argv)
{
	broadcast.sin_port = htons(68);
	if (argc != 2)
		error(1, 0, "Usage: dhcpd INTERFACE");

	char *if_name = argv[1];
	unsigned int interface = if_nametoindex(if_name);
	if (interface == 0)
		error(1, errno, if_name);

	char db_file[strlen(if_name) + sizeof(".db") + 1];
	stpcpy(stpcpy(db_file, if_name), ".db");
	db_file[-1] = 0;

	if (sqlite3_open(db_file, &leasedb) != SQLITE_OK)
		error(1, 0, "Error while opening lease database: %s", sqlite3_errmsg(leasedb));

	int sock;
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		error(1, errno, "Could not create socket");

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(67),
		.sin_addr = {INADDR_ANY}
	};

	struct ifaddrs *ifaddrs, *ifa;

	if (getifaddrs(&ifaddrs) == -1)
		error(1, errno, "Could not get interface information");

	for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == NULL)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET)
		{
			struct sockaddr_in *ifa_addr_in = (struct sockaddr_in *)ifa->ifa_addr;
			server_id = *ifa_addr_in;
		}
	}

	freeifaddrs(ifaddrs);

	if (bind(sock, (const struct sockaddr *)&bind_addr, sizeof(struct sockaddr_in)) < 0)
		error(1, errno, "Could not bind to 0.0.0.0:67");

	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (int[]){1}, sizeof(int)) != 0)
		error(1, errno, "Could not set broadcast socket option");
	if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name)) != 0)
		error(1, errno, "Could not bind to device %s", if_name);

	struct ev_loop *loop = EV_DEFAULT;

	ev_io read_watch;

	ev_io_init(&read_watch, req_cb, sock, EV_READ);
	ev_io_start(loop, &read_watch);

	ev_run(loop, 0);
}

