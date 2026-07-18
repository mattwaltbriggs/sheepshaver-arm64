/*
 *  ether_unix.cpp - Ethernet device driver, Unix specific stuff (Linux and FreeBSD)
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

/*
 *  NOTES concerning MacOS X issues:
 *  - poll() does not exist in 10.2.8, but is available in 10.4.4
 *  - select(), and very likely poll(), are not cancellation points. So
 *    the ethernet thread doesn't stop on exit. An explicit check is
 *    performed to workaround this problem.
 */
#if (defined __APPLE__ && defined __MACH__) || ! defined HAVE_POLL
#define USE_POLL 0
#else
#define USE_POLL 1
#endif

// Define to let the slirp library determine the right timeout for select()
#define USE_SLIRP_TIMEOUT 1

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <map>

#if defined(__FreeBSD__) || defined(sgi) || (defined(__APPLE__) && defined(__MACH__))
#include <net/if.h>
#endif

#if defined(HAVE_LINUX_IF_H) && defined(HAVE_LINUX_IF_TUN_H)
#include <linux/if.h>
#include <linux/if_tun.h>
#endif

#if defined(HAVE_NET_IF_H) && defined(HAVE_NET_IF_TUN_H)
#include <net/if.h>
#include <net/if_tun.h>
#endif

#ifdef HAVE_SLIRP
#include "libslirp.h"
#include "ctl.h"
#endif

#ifdef HAVE_LIBVDEPLUG
extern "C" {
#include <libvdeplug.h>
}
#endif

#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "ether.h"
#include "ether_defs.h"

#ifndef NO_STD_NAMESPACE
using std::map;
#endif

#define DEBUG 0
#include "debug.h"

#define STATISTICS 0
#define MONITOR 0

static void ether_trace(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("SheepShaver ether: ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static uint16 ether_read_be16(const uint8 *p)
{
	return ((uint16)p[0] << 8) | p[1];
}

static uint32 ether_read_be32(const uint8 *p)
{
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static void ether_trace_ip_addr(char *buf, size_t buf_size, const uint8 *addr)
{
	snprintf(buf, buf_size, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
}

static void ether_trace_frame(const char *prefix, const uint8 *frame, int len)
{
	if (len < 14) {
		ether_trace("%s len=%d", prefix, len);
		return;
	}

	const uint16 proto = ether_read_be16(frame + 12);
	if (proto == 0x0806 && len >= 42) {
		const uint8 *arp = frame + 14;
		char sender[16], target[16];
		ether_trace_ip_addr(sender, sizeof(sender), arp + 14);
		ether_trace_ip_addr(target, sizeof(target), arp + 24);
		ether_trace("%s len=%d ARP op=%u sender=%s target=%s dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x",
			prefix, len, ether_read_be16(arp + 6), sender, target,
			frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
			frame[6], frame[7], frame[8], frame[9], frame[10], frame[11]);
		return;
	}

	if (proto == 0x0800 && len >= 34) {
		const uint8 *ip = frame + 14;
		const int ihl = (ip[0] & 0x0f) * 4;
		const uint16 ip_len = ether_read_be16(ip + 2);
		char src[16], dst[16];
		ether_trace_ip_addr(src, sizeof(src), ip + 12);
		ether_trace_ip_addr(dst, sizeof(dst), ip + 16);

		if (ihl >= 20 && len >= 14 + ihl) {
			const uint8 ip_proto = ip[9];
			const uint8 *l4 = ip + ihl;
			if (ip_proto == 17 && len >= 14 + ihl + 8) {
				const uint16 sport = ether_read_be16(l4);
				const uint16 dport = ether_read_be16(l4 + 2);
				ether_trace("%s len=%d IPv4 UDP %s:%u -> %s:%u iplen=%u",
					prefix, len, src, sport, dst, dport, ip_len);
				return;
			}
			if (ip_proto == 6 && len >= 14 + ihl + 20) {
				const uint16 sport = ether_read_be16(l4);
				const uint16 dport = ether_read_be16(l4 + 2);
				const uint8 flags = l4[13];
				ether_trace("%s len=%d IPv4 TCP %s:%u -> %s:%u flags=0x%02x iplen=%u",
					prefix, len, src, sport, dst, dport, flags, ip_len);
				return;
			}
			if (ip_proto == 1 && len >= 14 + ihl + 4) {
				ether_trace("%s len=%d IPv4 ICMP %s -> %s type=%u code=%u iplen=%u",
					prefix, len, src, dst, l4[0], l4[1], ip_len);
				return;
			}
			ether_trace("%s len=%d IPv4 proto=%u %s -> %s iplen=%u",
				prefix, len, ip_proto, src, dst, ip_len);
			return;
		}
	}

	ether_trace("%s len=%d proto=0x%04x dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x",
		prefix, len, proto,
		frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
		frame[6], frame[7], frame[8], frame[9], frame[10], frame[11]);
}

#ifdef HAVE_SLIRP
static bool slirp_read_exact(int read_fd, void *buf, size_t len)
{
	uint8 *p = (uint8 *)buf;
	int wait_count = 0;
	while (len > 0) {
		ssize_t n = read(read_fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (++wait_count > 1000) {
					ether_trace("slirp frame read timed out fd=%d remaining=%zu", read_fd, len);
					return false;
				}
				Delay_usec(1000);
				continue;
			}
			ether_trace("slirp frame read failed fd=%d errno=%d", read_fd, errno);
			return false;
		}
		if (n == 0) {
			ether_trace("slirp frame read eof fd=%d remaining=%zu", read_fd, len);
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

static bool slirp_read_frame(int read_fd, uint8 *packet, size_t packet_size, ssize_t *length)
{
	int len;
	ssize_t n = read(read_fd, &len, sizeof(len));
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			ether_trace("slirp frame length read failed fd=%d errno=%d", read_fd, errno);
		return false;
	}
	if (n == 0)
		return false;
	if ((size_t)n < sizeof(len) && !slirp_read_exact(read_fd, (uint8 *)&len + n, sizeof(len) - n))
		return false;
	if (len < 14 || (size_t)len > packet_size) {
		ether_trace("slirp frame length invalid fd=%d len=%d max=%zu", read_fd, len, packet_size);
		return false;
	}
	if (!slirp_read_exact(read_fd, packet, len))
		return false;
	*length = len;
	return true;
}

static bool slirp_write_frame(int write_fd, const uint8 *packet, int len)
{
	if (len < 14 || len > 1516) {
		ether_trace("slirp frame write invalid fd=%d len=%d", write_fd, len);
		return false;
	}

	uint8 frame[sizeof(len) + 1516];
	memcpy(frame, &len, sizeof(len));
	memcpy(frame + sizeof(len), packet, len);

	const uint8 *p = frame;
	size_t remaining = sizeof(len) + len;
	while (remaining > 0) {
		ssize_t n = write(write_fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ether_trace("slirp frame write failed fd=%d errno=%d", write_fd, errno);
			return false;
		}
		if (n == 0) {
			ether_trace("slirp frame write eof fd=%d remaining=%zu", write_fd, remaining);
			return false;
		}
		p += n;
		remaining -= n;
	}
	return true;
}
#endif


// Ethernet device types
enum {
	NET_IF_SHEEPNET,
	NET_IF_ETHERTAP,
	NET_IF_TUNTAP,
	NET_IF_SLIRP,
	NET_IF_VDE
};

// Constants
#if ENABLE_TUNTAP
static const char ETHERCONFIG_FILE_NAME[] = DATADIR "/tunconfig";
#endif

// Global variables
static int fd = -1;							// fd of sheep_net device
static pthread_t ether_thread;				// Packet reception thread
static pthread_attr_t ether_thread_attr;	// Packet reception thread attributes
static bool thread_active = false;			// Flag: Packet reception thread installed
static sem_t int_ack;						// Interrupt acknowledge semaphore
static bool udp_tunnel;						// Flag: UDP tunnelling active, fd is the socket descriptor
static int net_if_type = -1;				// Ethernet device type
static char *net_if_name = NULL;			// TUN/TAP device name
static const char *net_if_script = NULL;	// Network config script
static pthread_t slirp_thread;				// Slirp reception thread
static bool slirp_thread_active = false;	// Flag: Slirp reception threadinstalled
static int slirp_output_fd = -1;			// fd of slirp output pipe
static int slirp_input_fds[2] = { -1, -1 };	// fds of slirp input pipe
#ifdef HAVE_LIBVDEPLUG
static VDECONN *vde_conn;
#endif
#ifdef SHEEPSHAVER
static bool net_open = false;				// Flag: initialization succeeded, network device open
static uint8 ether_addr[6];					// Our Ethernet address
#else
const bool ether_driver_opened = true;		// Flag: is the MacOS driver opened?
#endif

// Attached network protocols, maps protocol type to MacOS handler address
static map<uint16, uint32> net_protocols;

// Prototypes
static void *receive_func(void *arg);
static void *slirp_receive_func(void *arg);
static int16 ether_do_add_multicast(uint8 *addr);
static int16 ether_do_del_multicast(uint8 *addr);
static int16 ether_do_write(uint32 arg);
static void ether_do_interrupt(void);
static void slirp_add_redirs();
static int slirp_add_redir(const char *redir_str);


/*
 *  Start packet reception thread
 */

static bool start_thread(void)
{
	ether_trace("start_thread: begin");
	if (sem_init(&int_ack, 0, 0) < 0) {
		ether_trace("start_thread: sem_init failed errno=%d", errno);
		printf("WARNING: Cannot init semaphore");
		return false;
	}
	ether_trace("start_thread: semaphore ready");

	Set_pthread_attr(&ether_thread_attr, 1);
	thread_active = (pthread_create(&ether_thread, &ether_thread_attr, receive_func, NULL) == 0);
	if (!thread_active) {
		ether_trace("start_thread: ethernet pthread_create failed errno=%d", errno);
		printf("WARNING: Cannot start Ethernet thread");
		return false;
	}
	ether_trace("start_thread: ethernet thread started");

#ifdef HAVE_SLIRP
	if (net_if_type == NET_IF_SLIRP) {
		slirp_thread_active = (pthread_create(&slirp_thread, NULL, slirp_receive_func, NULL) == 0);
		if (!slirp_thread_active) {
			ether_trace("start_thread: slirp pthread_create failed errno=%d", errno);
			printf("WARNING: Cannot start slirp reception thread\n");
			return false;
		}
		ether_trace("start_thread: slirp thread started");
	}
#endif

	ether_trace("start_thread: done");
	return true;
}


/*
 *  Stop packet reception thread
 */

static void stop_thread(void)
{
#ifdef HAVE_SLIRP
	if (slirp_thread_active) {
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(slirp_thread);
#endif
		pthread_join(slirp_thread, NULL);
		slirp_thread_active = false;
	}
#endif

	if (thread_active) {
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(ether_thread);
#endif
		pthread_join(ether_thread, NULL);
		sem_destroy(&int_ack);
		thread_active = false;
	}
}


/*
 *  Execute network script up|down
 */

static bool execute_network_script(const char *action)
{
	if (net_if_script == NULL || net_if_name == NULL)
		return false;

	int pid = fork();
	if (pid >= 0) {
		if (pid == 0) {
			char *args[4];
			args[0] = (char *)net_if_script;
			args[1] = net_if_name;
			args[2] = (char *)action;
			args[3] = NULL;
			execv(net_if_script, args);
			exit(1);
		}
		int status;
		while (waitpid(pid, &status, 0) != pid);
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	return false;
}


/*
 *  Initialization
 */

bool ether_init(void)
{
	int val;
	char str[256];

	ether_trace("ether_init: begin");

	// Do nothing if no Ethernet device specified
	const char *name = PrefsFindString("ether");
	if (name == NULL || name[0] == '\0') {
		ether_trace("ether_init: no ether preference");
		return false;
	}
	ether_trace("ether_init: preference='%s'", name);

	// Determine Ethernet device type
	net_if_type = -1;
	if (strncmp(name, "tap", 3) == 0) {
		net_if_type = NET_IF_ETHERTAP;
		printf("selected Ethernet device type tap\n");
	}
#if ENABLE_TUNTAP
	else if (strcmp(name, "tun") == 0) {
		net_if_type = NET_IF_TUNTAP;
		printf("selected Ethernet device type tun\n");
	}
#endif
#ifdef HAVE_SLIRP
	else if (strcmp(name, "slirp") == 0) {
		net_if_type = NET_IF_SLIRP;
		printf("selected Ethernet device type slirp\n");
	}
#endif
#ifdef HAVE_LIBVDEPLUG
	else if (strcmp(name, "vde") == 0) {
		net_if_type = NET_IF_VDE;
		printf("selected Ethernet device type VDE\n");
	}
#endif
	else {
#if defined(__APPLE__) && defined(__MACH__) && (defined(__arm64__) || defined(__aarch64__)) && defined(HAVE_SLIRP)
		net_if_type = NET_IF_SLIRP;
		printf("selected Ethernet device type slirp (legacy device '%s' requested; /dev/sheep_net is unavailable on arm64 macOS)\n", name);
#else
		net_if_type = NET_IF_SHEEPNET;
		printf("selected Ethernet device type sheep_net\n");
#endif
	}

	// Don't raise SIGPIPE, let errno be set to EPIPE
	struct sigaction sigpipe_sa;
	if (sigaction(SIGPIPE, NULL, &sigpipe_sa) == 0) {
		assert(sigpipe_sa.sa_handler == SIG_DFL || sigpipe_sa.sa_handler == SIG_IGN);
		sigfillset(&sigpipe_sa.sa_mask);
		sigpipe_sa.sa_flags = 0;
		sigpipe_sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sigpipe_sa, NULL);
	}
	ether_trace("ether_init: SIGPIPE ignored");

#ifdef HAVE_SLIRP
	// Initialize slirp library
	if (net_if_type == NET_IF_SLIRP) {
		ether_trace("ether_init: slirp_init start");
		if (slirp_init() < 0) {
			ether_trace("ether_init: slirp_init failed");
			snprintf(str, sizeof(str), "%s", GetString(STR_SLIRP_NO_DNS_FOUND_WARN));
			WarningAlert(str);
			return false;
		}
		ether_trace("ether_init: slirp_init ok");

			// Open slirp output pipe
			int fds[2];
			if (pipe(fds) < 0) {
				ether_trace("ether_init: slirp output pipe failed errno=%d", errno);
				return false;
			}
			fd = fds[0];
			slirp_output_fd = fds[1];
			ether_trace("ether_init: slirp output pipe read=%d write=%d", fd, slirp_output_fd);

		// Open slirp input pipe
		if (pipe(slirp_input_fds) < 0) {
			ether_trace("ether_init: slirp input pipe failed errno=%d", errno);
			return false;
		}
		ether_trace("ether_init: slirp input pipe read=%d write=%d", slirp_input_fds[0], slirp_input_fds[1]);

		// Set up port redirects
		slirp_add_redirs();
		ether_trace("ether_init: slirp redirs done");
	}
#endif

	// Open sheep_net or ethertap or TUN/TAP device
	char dev_name[16];
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
		snprintf(dev_name, sizeof(dev_name), "/dev/%s", name);
		break;
	case NET_IF_TUNTAP:
		strcpy(dev_name, "/dev/net/tun");
		break;
	case NET_IF_SHEEPNET:
		strcpy(dev_name, "/dev/sheep_net");
		break;
	}

#ifdef HAVE_LIBVDEPLUG
	//vde switch information
	int port = 0;
	char *init_group = NULL;
	mode_t mode = 0700;

	struct vde_open_args args = {
		.port = port,
		.group = init_group,
		.mode = mode,
	};

	if (net_if_type == NET_IF_VDE) {
		/* calling vde open to open the vde connection to the vde switch */
		vde_conn = vde_open(vde_sock, (char *)"macemu", &args);

		if (!vde_conn) {
			D(bug("VDE open failed\n"));
			return -1;
		} else {
			/* for select/poll when this fd receive data, there are
			 * packets to recv(call vde_recv) */
			fd = vde_datafd(vde_conn);
		}
	}
#endif

	if (net_if_type != NET_IF_SLIRP && net_if_type != NET_IF_VDE) {
		ether_trace("ether_init: opening device '%s'", dev_name);
		fd = open(dev_name, O_RDWR);
		if (fd < 0) {
			ether_trace("ether_init: open failed errno=%d", errno);
			snprintf(str, sizeof(str), GetString(STR_NO_SHEEP_NET_DRIVER_WARN), dev_name, strerror(errno));
			WarningAlert(str);
			goto open_error;
		}
		ether_trace("ether_init: device opened fd=%d", fd);
	}

#if ENABLE_TUNTAP
	// Open TUN/TAP interface
	if (net_if_type == NET_IF_TUNTAP) {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
		strcpy(ifr.ifr_name, "tun%d");
		if (ioctl(fd, TUNSETIFF, (void *) &ifr) != 0) {
			snprintf(str, sizeof(str), GetString(STR_SHEEP_NET_ATTACH_WARN), strerror(errno));
			WarningAlert(str);
			goto open_error;
		}

		// Get network config script file path
		net_if_script = PrefsFindString("etherconfig");
		if (net_if_script == NULL)
			net_if_script = ETHERCONFIG_FILE_NAME;

		// Start network script up
		if (net_if_script == NULL) {
			snprintf(str, sizeof(str), GetString(STR_TUN_TAP_CONFIG_WARN), "script not found");
			WarningAlert(str);
			goto open_error;
		}
		net_if_name = strdup(ifr.ifr_name);
		if (!execute_network_script("up")) {
			snprintf(str, sizeof(str), GetString(STR_TUN_TAP_CONFIG_WARN), "script execute error");
			WarningAlert(str);
			goto open_error;
		}
		D(bug("Connected to host network interface: %s\n", net_if_name));
	}
#endif

#if defined(__linux__)
	// Attach sheep_net to selected Ethernet card
	if (net_if_type == NET_IF_SHEEPNET && ioctl(fd, SIOCSIFLINK, name) < 0) {
		snprintf(str, sizeof(str), GetString(STR_SHEEP_NET_ATTACH_WARN), strerror(errno));
		WarningAlert(str);
		goto open_error;
	}
#endif

	// Set nonblocking I/O
#ifdef USE_FIONBIO
	int nonblock = 1;
	if (ioctl(fd, FIONBIO, &nonblock) < 0) {
		ether_trace("ether_init: FIONBIO failed errno=%d fd=%d", errno, fd);
		snprintf(str, sizeof(str), GetString(STR_BLOCKING_NET_SOCKET_WARN), strerror(errno));
		WarningAlert(str);
		goto open_error;
	}
	ether_trace("ether_init: nonblocking set fd=%d", fd);
#else
	val = fcntl(fd, F_GETFL, 0);
	if (val < 0 || fcntl(fd, F_SETFL, val | O_NONBLOCK) < 0) {
		snprintf(str, sizeof(str), GetString(STR_BLOCKING_NET_SOCKET_WARN), strerror(errno));
		WarningAlert(str);
		goto open_error;
	}
#endif

	// Get Ethernet address
	if (net_if_type == NET_IF_ETHERTAP || net_if_type == NET_IF_TUNTAP) {
		pid_t p = getpid();	// If configured for multicast, ethertap requires that the lower 32 bit of the Ethernet address are our PID
		ether_addr[0] = 0xfe;
		ether_addr[1] = 0xfd;
		ether_addr[2] = p >> 24;
		ether_addr[3] = p >> 16;
		ether_addr[4] = p >> 8;
		ether_addr[5] = p;
#ifdef HAVE_SLIRP
	} else if (net_if_type == NET_IF_SLIRP) {
		ether_addr[0] = 0x52;
		ether_addr[1] = 0x54;
		ether_addr[2] = 0x00;
		ether_addr[3] = 0x12;
		ether_addr[4] = 0x34;
		ether_addr[5] = 0x56;
#endif
#ifdef HAVE_LIBVDEPLUG
	} else if (net_if_type == NET_IF_VDE) {
		ether_addr[0] = 0x52;
		ether_addr[1] = 0x54;
		ether_addr[2] = 0x00;
		ether_addr[3] = 0x12;
		ether_addr[4] = 0x34;
		ether_addr[5] = 0x56;
#endif
	} else
		ioctl(fd, SIOCGIFADDR, ether_addr);
	D(bug("Ethernet address %02x %02x %02x %02x %02x %02x\n", ether_addr[0], ether_addr[1], ether_addr[2], ether_addr[3], ether_addr[4], ether_addr[5]));
	ether_trace("ether_init: mac=%02x:%02x:%02x:%02x:%02x:%02x",
		ether_addr[0], ether_addr[1], ether_addr[2], ether_addr[3], ether_addr[4], ether_addr[5]);

	// Start packet reception thread
	if (!start_thread()) {
		ether_trace("ether_init: start_thread failed");
		goto open_error;
	}

	// Everything OK
	ether_trace("ether_init: success");
	return true;

open_error:
	ether_trace("ether_init: open_error cleanup");
	stop_thread();

	if (fd > 0) {
		close(fd);
		fd = -1;
	}
	if (slirp_input_fds[0] >= 0) {
		close(slirp_input_fds[0]);
		slirp_input_fds[0] = -1;
	}
	if (slirp_input_fds[1] >= 0) {
		close(slirp_input_fds[1]);
		slirp_input_fds[1] = -1;
	}
	if (slirp_output_fd >= 0) {
		close(slirp_output_fd);
		slirp_output_fd = -1;
	}
	return false;
}


/*
 *  Deinitialization
 */

void ether_exit(void)
{
	// Stop reception threads
	stop_thread();

	// Shut down TUN/TAP interface
	if (net_if_type == NET_IF_TUNTAP)
		execute_network_script("down");

	// Free TUN/TAP device name
	if (net_if_name)
		free(net_if_name);

	// Close sheep_net device
	if (fd > 0)
		close(fd);

	// Close slirp input buffer
	if (slirp_input_fds[0] >= 0)
		close(slirp_input_fds[0]);
	if (slirp_input_fds[1] >= 0)
		close(slirp_input_fds[1]);

	// Close slirp output buffer
	if (slirp_output_fd > 0)
		close(slirp_output_fd);

#ifdef HAVE_LIBVDEPLUG
	// Close vde_connection
	if (net_if_type == NET_IF_VDE)
		vde_close(vde_conn);
#endif
#if STATISTICS
	// Show statistics
	printf("%ld messages put on write queue\n", num_wput);
	printf("%ld error acks\n", num_error_acks);
	printf("%ld packets transmitted (%ld raw, %ld normal)\n", num_tx_packets, num_tx_raw_packets, num_tx_normal_packets);
	printf("%ld tx packets dropped because buffer full\n", num_tx_buffer_full);
	printf("%ld packets received\n", num_rx_packets);
	printf("%ld packets passed upstream (%ld Fast Path, %ld normal)\n", num_rx_fastpath + num_unitdata_ind, num_rx_fastpath, num_unitdata_ind);
	printf("EtherIRQ called %ld times\n", num_ether_irq);
	printf("%ld rx packets dropped due to low memory\n", num_rx_no_mem);
	printf("%ld rx packets dropped because no stream found\n", num_rx_dropped);
	printf("%ld rx packets dropped because stream not ready\n", num_rx_stream_not_ready);
	printf("%ld rx packets dropped because no memory for unitdata_ind\n", num_rx_no_unitdata_mem);
#endif
}


/*
 *  Glue around low-level implementation
 */

#ifdef SHEEPSHAVER
// Error codes
enum {
	eMultiErr		= -91,
	eLenErr			= -92,
	lapProtErr		= -94,
	excessCollsns	= -95
};

// Initialize ethernet
void EtherInit(void)
{
	ether_trace("EtherInit: entered");
	net_open = false;

	// Do nothing if the user disabled the network
	if (PrefsFindBool("nonet")) {
		ether_trace("EtherInit: disabled by nonet pref");
		return;
	}

	net_open = ether_init();
	ether_trace("EtherInit: ether_init returned net_open=%d", net_open ? 1 : 0);
}

// Exit ethernet
void EtherExit(void)
{
	ether_trace("EtherExit: entered");
	ether_exit();
	net_open = false;
	ether_trace("EtherExit: done");
}

// Get ethernet hardware address
void AO_get_ethernet_address(uint32 arg)
{
	static int log_count = 0;
	uint8 *addr = Mac2HostAddr(arg);
	if (net_open)
		OTCopy48BitAddress(ether_addr, addr);
	else {
		addr[0] = 0x12;
		addr[1] = 0x34;
		addr[2] = 0x56;
		addr[3] = 0x78;
		addr[4] = 0x9a;
		addr[5] = 0xbc;
	}
	if (log_count < 4) {
		ether_trace("AO_get_ethernet_address: net_open=%d addr=%02x:%02x:%02x:%02x:%02x:%02x",
			net_open ? 1 : 0, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
		log_count++;
	}
	D(bug("AO_get_ethernet_address: got address %02x%02x%02x%02x%02x%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]));
}

// Add multicast address
void AO_enable_multicast(uint32 addr)
{
	if (net_open)
		ether_do_add_multicast(Mac2HostAddr(addr));
}

// Disable multicast address
void AO_disable_multicast(uint32 addr)
{
	if (net_open)
		ether_do_del_multicast(Mac2HostAddr(addr));
}

// Transmit one packet
void AO_transmit_packet(uint32 mp)
{
	if (net_open) {
		static int log_count = 0;
		switch (ether_do_write(mp)) {
		case noErr:
			num_tx_packets++;
			if (log_count < 8) {
				ether_trace("AO_transmit_packet: ok count=%ld mp=%08x", num_tx_packets, mp);
				log_count++;
			}
			break;
		case excessCollsns:
			num_tx_buffer_full++;
			ether_trace("AO_transmit_packet: excessCollsns mp=%08x", mp);
			break;
		}
	}
}

// Copy packet data from message block to linear buffer
static inline int ether_arg_to_buffer(uint32 mp, uint8 *p)
{
	return ether_msgb_to_buffer(mp, p);
}

// Ethernet interrupt
void EtherIRQ(void)
{
	D(bug("EtherIRQ\n"));
	num_ether_irq++;

	OTEnterInterrupt();
	ether_do_interrupt();
	OTLeaveInterrupt();

	// Acknowledge interrupt to reception thread
	D(bug(" EtherIRQ done\n"));
	ether_trace("EtherIRQ: posting interrupt acknowledge");
	sem_post(&int_ack);
}
#else
// Add multicast address
int16 ether_add_multicast(uint32 pb)
{
	return ether_do_add_multicast(Mac2HostAddr(pb + eMultiAddr));
}

// Disable multicast address
int16 ether_del_multicast(uint32 pb)
{
	return ether_do_del_multicast(Mac2HostAddr(pb + eMultiAddr));
}

// Transmit one packet
int16 ether_write(uint32 wds)
{
	return ether_do_write(wds);
}

// Copy packet data from WDS to linear buffer
static inline int ether_arg_to_buffer(uint32 wds, uint8 *p)
{
	return ether_wds_to_buffer(wds, p);
}

// Dispatch packet to protocol handler
static void ether_dispatch_packet(uint32 p, uint32 length)
{
	// Get packet type
	uint16 type = ReadMacInt16(p + 12);

	// Look for protocol
	uint16 search_type = (type <= 1500 ? 0 : type);
	if (net_protocols.find(search_type) == net_protocols.end())
		return;
	uint32 handler = net_protocols[search_type];

	// No default handler
	if (handler == 0)
		return;

	// Copy header to RHA
	Mac2Mac_memcpy(ether_data + ed_RHA, p, 14);
	D(bug(" header %08x%04x %08x%04x %04x\n", ReadMacInt32(ether_data + ed_RHA), ReadMacInt16(ether_data + ed_RHA + 4), ReadMacInt32(ether_data + ed_RHA + 6), ReadMacInt16(ether_data + ed_RHA + 10), ReadMacInt16(ether_data + ed_RHA + 12)));

	// Call protocol handler
	M68kRegisters r;
	r.d[0] = type;									// Packet type
	r.d[1] = length - 14;							// Remaining packet length (without header, for ReadPacket)
	r.a[0] = p + 14;								// Pointer to packet (Mac address, for ReadPacket)
	r.a[3] = ether_data + ed_RHA + 14;				// Pointer behind header in RHA
	r.a[4] = ether_data + ed_ReadPacket;			// Pointer to ReadPacket/ReadRest routines
	D(bug(" calling protocol handler %08x, type %08x, length %08x, data %08x, rha %08x, read_packet %08x\n", handler, r.d[0], r.d[1], r.a[0], r.a[3], r.a[4]));
	Execute68k(handler, &r);
}

// Ethernet interrupt
void EtherInterrupt(void)
{
	D(bug("EtherIRQ\n"));
	ether_do_interrupt();

	// Acknowledge interrupt to reception thread
	D(bug(" EtherIRQ done\n"));
	ether_trace("EtherInterrupt: posting interrupt acknowledge");
	sem_post(&int_ack);
}
#endif


/*
 *  Reset
 */

void ether_reset(void)
{
	net_protocols.clear();
}


/*
 *  Add multicast address
 */

static int16 ether_do_add_multicast(uint8 *addr)
{
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
	case NET_IF_SHEEPNET:
		if (ioctl(fd, SIOCADDMULTI, addr) < 0) {
			D(bug("WARNING: Couldn't enable multicast address\n"));
			if (net_if_type == NET_IF_ETHERTAP) {
				return noErr;
			} else {
				return eMultiErr;
			}
		}
		return noErr;
	default:
		return noErr;
	}
}


/*
 *  Delete multicast address
 */

static int16 ether_do_del_multicast(uint8 *addr)
{
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
	case NET_IF_SHEEPNET:
		if (ioctl(fd, SIOCDELMULTI, addr) < 0) {
			D(bug("WARNING: Couldn't disable multicast address\n"));
			return eMultiErr;
		}
		return noErr;
	default:
		return noErr;
	}
}


/*
 *  Attach protocol handler
 */

int16 ether_attach_ph(uint16 type, uint32 handler)
{
	if (net_protocols.find(type) != net_protocols.end())
		return lapProtErr;
	net_protocols[type] = handler;
	return noErr;
}


/*
 *  Detach protocol handler
 */

int16 ether_detach_ph(uint16 type)
{
	if (net_protocols.erase(type) == 0)
		return lapProtErr;
	return noErr;
}


/*
 *  Transmit raw ethernet packet
 */

static int16 ether_do_write(uint32 arg)
{
	// Copy packet to buffer
	uint8 packet[1516], *p = packet;
	int len = 0;
#if defined(__linux__)
	if (net_if_type == NET_IF_ETHERTAP) {
		*p++ = 0;	// Linux ethertap discards the first 2 bytes
		*p++ = 0;
		len += 2;
	}
#endif
	len += ether_arg_to_buffer(arg, p);
	{
		static int log_count = 0;
		const uint8 *frame = p;
		const int frame_len = len - (int)(frame - packet);
		if (log_count < 8) {
			ether_trace_frame("ether_do_write: tx", frame, frame_len);
			log_count++;
		}
	}

#if MONITOR
	bug("Sending Ethernet packet:\n");
	for (int i=0; i<len; i++) {
		bug("%02x ", packet[i]);
	}
	bug("\n");
#endif

	// Transmit packet
#ifdef HAVE_SLIRP
	if (net_if_type == NET_IF_SLIRP) {
		const int slirp_input_fd = slirp_input_fds[1];
		if (!slirp_write_frame(slirp_input_fd, packet, len))
			return excessCollsns;
		return noErr;
	} else
#endif
#ifdef HAVE_LIBVDEPLUG
	if (net_if_type == NET_IF_VDE) {
		if (fd == -1) {	// which means vde service is not running
			D(bug("WARNING: Couldn't transmit VDE packet\n"));
			return excessCollsns;
		}

		if (vde_conn == NULL) {
			D(bug("WARNING: vde_conn is NULL\n"));
			return -1;
		}

		do {
			len = vde_send(vde_conn, packet, sizeof(packet), 0);
		} while (len < 0);

		return noErr;
	} else
#endif
	if (write(fd, packet, len) < 0) {
		D(bug("WARNING: Couldn't transmit packet\n"));
		return excessCollsns;
	} else
		return noErr;
}


/*
 *  Start UDP packet reception thread
 */

bool ether_start_udp_thread(int socket_fd)
{
	fd = socket_fd;
	udp_tunnel = true;
	return start_thread();
}


/*
 *  Stop UDP packet reception thread
 */

void ether_stop_udp_thread(void)
{
	stop_thread();
	fd = -1;
}


/*
 *  SLIRP output buffer glue
 */

#ifdef HAVE_SLIRP
int slirp_can_output(void)
{
	return 1;
}

void slirp_output(const uint8 *packet, int len)
{
	static int log_count = 0;
	if (log_count < 8) {
		ether_trace_frame("slirp_output", packet, len);
		log_count++;
	}
	slirp_write_frame(slirp_output_fd, packet, len);
}

static bool slirp_input_pending(int slirp_input_fd)
{
	if (slirp_input_fd < 0)
		return false;

	fd_set rfds;
	struct timeval tv = { 0, 0 };
	FD_ZERO(&rfds);
	FD_SET(slirp_input_fd, &rfds);
	return select(slirp_input_fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(slirp_input_fd, &rfds);
}

static void slirp_process_input_frame(int slirp_input_fd)
{
	static int log_count = 0;
	static int slirp_input_call_log_count = 0;
	ssize_t len;
	uint8 packet[1516];

	if (!slirp_read_frame(slirp_input_fd, packet, sizeof(packet), &len))
		return;
	if (log_count < 8) {
		ether_trace_frame("slirp_receive_func: guest->slirp", packet, (int)len);
		log_count++;
	}
	const bool trace_call = slirp_input_call_log_count < 8;
	if (trace_call) {
		ether_trace("slirp_receive_func: calling slirp_input len=%d", (int)len);
		slirp_input_call_log_count++;
	}
	slirp_input(packet, (int)len);
	if (trace_call)
		ether_trace("slirp_receive_func: slirp_input returned len=%d", (int)len);
}

static bool slirp_drain_input(int slirp_input_fd)
{
	bool drained = false;
	for (int i = 0; i < 16 && slirp_input_pending(slirp_input_fd); i++) {
		slirp_process_input_frame(slirp_input_fd);
		drained = true;
	}
	return drained;
}

void *slirp_receive_func(void *arg)
{
	const int slirp_input_fd = slirp_input_fds[0];
	ether_trace("slirp_receive_func: started input_fd=%d", slirp_input_fd);

	for (;;) {
		if (slirp_drain_input(slirp_input_fd))
			continue;

		fd_set rfds, wfds, xfds;
		int nfds = -1;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&xfds);
		int timeout = slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
#if ! USE_SLIRP_TIMEOUT
		timeout = 10000;
#endif
		FD_SET(slirp_input_fd, &rfds);
		if (nfds < slirp_input_fd)
			nfds = slirp_input_fd;

		struct timeval tv;
		tv.tv_sec = timeout / 1000000;
		tv.tv_usec = timeout % 1000000;
		if (select(nfds + 1, &rfds, &wfds, &xfds, &tv) >= 0) {
			if (FD_ISSET(slirp_input_fd, &rfds)) {
				FD_CLR(slirp_input_fd, &rfds);
				slirp_process_input_frame(slirp_input_fd);
				slirp_drain_input(slirp_input_fd);
				continue;
			}
			slirp_select_poll(&rfds, &wfds, &xfds);
		}

#ifdef HAVE_PTHREAD_TESTCANCEL
		// Explicit cancellation point if select() was not covered
		// This seems to be the case on MacOS X 10.2
		pthread_testcancel();
#endif
	}
	return NULL;
}
#else
int slirp_can_output(void)
{
	return 0;
}

void slirp_output(const uint8 *packet, int len)
{
}
#endif


/*
 *  Packet reception thread
 */

static void *receive_func(void *arg)
{
	ether_trace("receive_func: started fd=%d", fd);
	for (;;) {

		// Wait for packets to arrive
#if USE_POLL
		struct pollfd pf = {fd, POLLIN, 0};
		int res = poll(&pf, 1, -1);
#else
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		// A NULL timeout could cause select() to block indefinitely,
		// even if it is supposed to be a cancellation point [MacOS X]
		struct timeval tv = { 0, 20000 };
		int res = select(fd + 1, &rfds, NULL, NULL, &tv);
#ifdef HAVE_PTHREAD_TESTCANCEL
		pthread_testcancel();
#endif
		if (res == 0 || (res == -1 && errno == EINTR))
			continue;
#endif
		if (res <= 0)
			break;

		if (ether_driver_opened) {
			static int log_count = 0;
			if (log_count < 8) {
				ether_trace("receive_func: fd ready res=%d driver_opened=1", res);
				log_count++;
			}
			// Trigger Ethernet interrupt
			D(bug(" packet received, triggering Ethernet interrupt\n"));
			SetInterruptFlag(INTFLAG_ETHER);
			TriggerInterrupt();

			// Wait for interrupt acknowledge by EtherInterrupt()
			sem_wait(&int_ack);
			if (log_count <= 8)
				ether_trace("receive_func: interrupt acknowledged");
		} else
			Delay_usec(20000);
	}
	ether_trace("receive_func: exiting");
	return NULL;
}


/*
 *  Ethernet interrupt - activate deferred tasks to call IODone or protocol handlers
 */

void ether_do_interrupt(void)
{
	// Call protocol handler for received packets
	EthernetPacket ether_packet;
	uint32 packet = ether_packet.addr();
	ssize_t length;
	static int log_count = 0;
	for (;;) {

#ifndef SHEEPSHAVER
		if (udp_tunnel) {

			// Read packet from socket
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			length = recvfrom(fd, Mac2HostAddr(packet), 1514, 0, (struct sockaddr *)&from, &from_len);
			if (length < 14)
				break;
			ether_udp_read(packet, length, &from);

		} else
#endif
		{
#ifdef HAVE_LIBVDEPLUG
			if (net_if_type == NET_IF_VDE) {
				length = vde_recv(vde_conn, Mac2HostAddr(packet), 1514, 0);
			} else
#endif
			{
				// Read packet from sheep_net device
#ifdef HAVE_SLIRP
				if (net_if_type == NET_IF_SLIRP) {
					if (!slirp_read_frame(fd, Mac2HostAddr(packet), 1516, &length))
						break;
				} else
#endif
				{
#if defined(__linux__)
					length = read(fd, Mac2HostAddr(packet), net_if_type == NET_IF_ETHERTAP ? 1516 : 1514);
#else
					length = read(fd, Mac2HostAddr(packet), 1514);
#endif
				}
			}
			
			if (length < 14)
				break;
				if (log_count < 8) {
					uint8 *frame = Mac2HostAddr(packet);
					ether_trace_frame("ether_do_interrupt: read", frame, (int)length);
					log_count++;
				}

#if MONITOR
			bug("Receiving Ethernet packet:\n");
			for (int i=0; i<length; i++) {
				bug("%02x ", ReadMacInt8(packet + i));
			}
			bug("\n");
#endif

			// Pointer to packet data (Ethernet header)
			uint32 p = packet;
#if defined(__linux__)
			if (net_if_type == NET_IF_ETHERTAP) {
				p += 2;			// Linux ethertap has two random bytes before the packet
				length -= 2;
			}
#endif

				// Dispatch packet
				ether_dispatch_packet(p, length);
				if (log_count <= 8)
					ether_trace("ether_do_interrupt: dispatch returned len=%d", (int)length);
			}
		}
}

// Helper function for port forwarding
static int get_str_sep(char *buf, int buf_size, const char **pp, int sep)
{
	const char *p, *p1;
	int len;
	p = *pp;
	p1 = strchr(p, sep);
	if (!p1)
		return -1;
	len = p1 - p;
	p1++;
	if (buf_size > 0) {
		if (len > buf_size - 1)
			len = buf_size - 1;
		memcpy(buf, p, len);
		buf[len] = '\0';
	}
	*pp = p1;
	return 0;
}

// Set up port forwarding for slirp
static void slirp_add_redirs()
{
	int index = 0;
	const char *str;
	while ((str = PrefsFindString("redir", index++)) != NULL) {
		slirp_add_redir(str);
	}
}

// Add a port forward/redirection for slirp
static int slirp_add_redir(const char *redir_str)
{
	// code adapted from qemu source
	struct in_addr guest_addr = {0};
	int host_port, guest_port;
	const char *p;
	char buf[256];
	int is_udp;
	char *end;
	char str[256];

	p = redir_str;
	if (!p || get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	if (!strcmp(buf, "tcp") || buf[0] == '\0') {
		is_udp = 0;
	} else if (!strcmp(buf, "udp")) {
		is_udp = 1;
	} else {
		goto fail_syntax;
	}

	if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	host_port = strtol(buf, &end, 0);
	if (*end != '\0' || host_port < 1 || host_port > 65535) {
		goto fail_syntax;
	}

	if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	// 0.0.0.0 doesn't seem to work, so default to a client address
	// if none is specified
	if (buf[0] == '\0' ?
			!inet_aton(CTL_LOCAL, &guest_addr) :
			!inet_aton(buf, &guest_addr)) {
		goto fail_syntax;
	}

	guest_port = strtol(p, &end, 0);
	if (*end != '\0' || guest_port < 1 || guest_port > 65535) {
		goto fail_syntax;
	}

	if (slirp_redir(is_udp, host_port, guest_addr, guest_port) < 0) {
		snprintf(str, sizeof(str), "could not set up host forwarding rule '%s'", redir_str);
		WarningAlert(str);
		return -1;
	}
	return 0;

 fail_syntax:
	snprintf(str, sizeof(str), "invalid host forwarding rule '%s'", redir_str);
	WarningAlert(str);
	return -1;
}
