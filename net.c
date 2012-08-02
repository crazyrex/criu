#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <net/if_arp.h>
#include <sys/wait.h>
#include "syscall-types.h"
#include "namespaces.h"
#include "net.h"
#include "libnetlink.h"

#include "protobuf.h"
#include "protobuf/netdev.pb-c.h"

void show_netdevices(int fd, struct cr_options *opt)
{
	pb_show_plain(fd, net_device_entry);
}

static int dump_one_netdev(int type, struct nlmsghdr *h, struct ifinfomsg *ifi, struct cr_fdset *fds)
{
	int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
	struct rtattr * tb[IFLA_MAX+1];
	NetDeviceEntry netdev = NET_DEVICE_ENTRY__INIT;

	if (len < 0) {
		pr_err("No iflas for link %d\n", ifi->ifi_index);
		return -1;
	}

	parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

	if (!tb[IFLA_IFNAME]) {
		pr_err("No name for link %d\n", ifi->ifi_index);
		return -1;
	}

	netdev.type = type;
	netdev.ifindex = ifi->ifi_index;
	netdev.mtu = *(int *)RTA_DATA(tb[IFLA_MTU]);
	netdev.flags = ifi->ifi_flags;
	netdev.name = RTA_DATA(tb[IFLA_IFNAME]);

	return pb_write(fdset_fd(fds, CR_FD_NETDEV), &netdev, net_device_entry);
}

static int dump_one_link(struct nlmsghdr *hdr, void *arg)
{
	struct cr_fdset *fds = arg;
	struct ifinfomsg *ifi;
	int ret = 0;

	ifi = NLMSG_DATA(hdr);
	pr_info("\tLD: Got link %d, type %d\n", ifi->ifi_index, ifi->ifi_type);

	switch (ifi->ifi_type) {
	case ARPHRD_LOOPBACK:
		ret = dump_one_netdev(ND_TYPE__LOOPBACK, hdr, ifi, fds);
		break;
	default:
		pr_err("Unsupported link type %d\n", ifi->ifi_type);
		ret = 0; /* just skip for now */
		break;
	}

	return ret;
}

static int dump_links(struct cr_fdset *fds)
{
	int sk, ret;
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg g;
	} req;

	pr_info("Dumping netns links\n");

	ret = sk = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (sk < 0) {
		pr_perror("Can't open rtnl sock for net dump");
		goto out;
	}

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETLINK;
	req.nlh.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.nlh.nlmsg_seq = CR_NLMSG_SEQ;
	req.g.rtgen_family = AF_PACKET;

	ret = do_rtnl_req(sk, &req, sizeof(req), dump_one_link, fds);
	close(sk);
out:
	return ret;
}

static int restore_link_cb(struct nlmsghdr *hdr, void *arg)
{
	pr_info("Got responce on SETLINK =)\n");
	return 0;
}

static int restore_one_link(NetDeviceEntry *nde, int nlsk)
{
	struct {
		struct nlmsghdr h;
		struct ifinfomsg i;
	} req;

	memset(&req, 0, sizeof(req));

	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.h.nlmsg_flags = NLM_F_REQUEST|NLM_F_ACK;
	req.h.nlmsg_type = RTM_NEWLINK;
	req.h.nlmsg_seq = CR_NLMSG_SEQ;
	req.i.ifi_family = AF_PACKET;
	req.i.ifi_index = nde->ifindex;
	req.i.ifi_flags = nde->flags;

	/* FIXME -- restore mtu as well */

	pr_info("Restoring netdev idx %d\n", nde->ifindex);
	return do_rtnl_req(nlsk, &req, sizeof(req), restore_link_cb, NULL);
}

static int restore_link(NetDeviceEntry *nde, int nlsk)
{
	pr_info("Restoring link type %d\n", nde->type);

	switch (nde->type) {
	case ND_TYPE__LOOPBACK:
		return restore_one_link(nde, nlsk);
	}

	BUG_ON(1);
	return -1;
}

static int restore_links(int pid)
{
	int fd, nlsk, ret;
	NetDeviceEntry *nde;

	fd = open_image_ro(CR_FD_NETDEV, pid);
	if (fd < 0)
		return -1;

	nlsk = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nlsk < 0) {
		pr_perror("Can't create nlk socket");
		return -1;
	}

	while (1) {
		ret = pb_read_eof(fd, &nde, net_device_entry);
		if (ret <= 0)
			break;

		ret = restore_link(nde, nlsk);
		net_device_entry__free_unpacked(nde, NULL);
		if (ret)
			break;
	}

	close(nlsk);
	close(fd);
	return ret;
}

int dump_net_ns(int pid, struct cr_fdset *fds)
{
	int ret;

	ret = switch_ns(pid, CLONE_NEWNET, "net", NULL);
	if (!ret)
		ret = dump_links(fds);

	return ret;
}

int prepare_net_ns(int pid)
{
	int ret;

	ret = restore_links(pid);

	return ret;
}
