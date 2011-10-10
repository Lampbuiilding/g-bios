#include <sysconf.h>
#include <net/net.h>
#include <net/mii.h>
#include <uart/uart.h>

struct host_addr
{
	struct eth_addr in_addr;
	struct list_node node;
};

struct pseudo_header
{
	u8  src_ip[IPV4_ADR_LEN];
	u8  des_ip[IPV4_ADR_LEN];
	u8  zero;
	u8  prot;
	u16 size;
};

static struct list_node g_host_list;
static struct list_node g_ndev_list;
static struct net_device *g_curr_ndev; // fixme
#ifdef CONFIG_DEBUG
static const char *g_arp_desc[] = {"N/A", "Request", "Reply"};
#endif
static int ndev_count = 0;

static int pseudo_calculate_checksum(struct sock_buff *skb, u16 *checksum)
{
	struct pseudo_header *pse_hdr;
	struct socket *sock = skb->sock;

	pse_hdr = (struct pseudo_header *)skb->data - 1;

	memcpy(pse_hdr->src_ip, &sock->saddr[SA_SRC].sin_addr, IPV4_ADR_LEN);
	memcpy(pse_hdr->des_ip, &sock->saddr[SA_DST].sin_addr, IPV4_ADR_LEN);
	pse_hdr->zero = 0;

	switch (sock->type)
	{
	case SOCK_STREAM:
		pse_hdr->prot = PROT_TCP;
		break;

	case SOCK_DGRAM:
		pse_hdr->prot = PROT_UDP;
		break;

	default:
		return -EINVAL;
	}

	pse_hdr->size = CPU_TO_BE16(skb->size);

	if (skb->size & 1)
	{
		skb->data[skb->size] = 0;
	}

	*checksum = ~net_calc_checksum(pse_hdr, sizeof(*pse_hdr) + skb->size);

	return 0;
}

static void ether_send_packet(struct sock_buff *skb, const u8 mac[], u16 eth_type);

struct socket *udp_search_socket(const struct udp_header *, const struct ip_header *);

struct socket *tcp_search_socket(const struct tcp_header *, const struct ip_header *);

static inline BOOL ip_is_bcast(u32 ip)
{
	u32 mask;
	int ret;

	ret = ndev_ioctl(NULL, NIOC_GET_MASK, &mask);
	if (ret < 0)
	{
		return FALSE;
	}

	if (~(mask | ip) == 0)
		return TRUE;

	return FALSE;
}

static inline void mac_fill_bcast(u8 mac[])
{
	memset(mac,(u8)0xff, MAC_ADR_LEN);
}

#ifdef CONFIG_NET_DEBUG
static void ether_info(struct ether_header *eth_head)
{

	printf("\tEther frame type: 0x%04x\n", BE16_TO_CPU(eth_head->frame_type));

	printf("\tdest mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
		eth_head->des_mac[0],
		eth_head->des_mac[1],
		eth_head->des_mac[2],
		eth_head->des_mac[3],
		eth_head->des_mac[4],
		eth_head->des_mac[5]);

	printf("\tsource mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
		eth_head->src_mac[0],
		eth_head->src_mac[1],
		eth_head->src_mac[2],
		eth_head->src_mac[3],
		eth_head->src_mac[4],
		eth_head->src_mac[5]);
}

static void dump_sock_buff(const struct sock_buff *skb)
{
	int idx;
	u8 *data;

	for (idx = 0, data = skb->head; idx < skb->size; idx++, data++)
	{
		printf("%02x", *data);
	}

	printf("\n");
}
#endif

#if 0
void tcp_make_pkg(struct sock_buff *skb, u16 flag)
{
	skb->data -= TCP_HDR_LEN + opt_len;
	skb->size += TCP_HDR_LEN + opt_len;

	tcp_hdr = (struct tcp_header *)skb->data;
	//
	tcp_hdr->src_port = sock->saddr[SA_SRC].sin_port;
	tcp_hdr->dst_port = sock->saddr[SA_DST].sin_port;
	tcp_hdr->seq_num  = CPU_TO_BE32(1);
	tcp_hdr->ack_num  = 0;
	tcp_hdr->hdr_len  = (TCP_HDR_LEN + opt_len) / 4;
	tcp_hdr->reserve1 = 0;
	tcp_hdr->reserve2 = 0;
	tcp_hdr->flg_urg  = 0;
	tcp_hdr->flg_ack  = 0;
	tcp_hdr->flg_psh  = 0;
	tcp_hdr->flg_rst  = 0;
	tcp_hdr->flg_syn  = 1;
	tcp_hdr->flg_fin  = 0;
	tcp_hdr->win_size = 0; // CPU_TO_BE16(32792);
	tcp_hdr->checksum = 0;
	tcp_hdr->urg_ptr  = 0;

	if (opt_len > 0)
		memset(tcp_hdr->options, 0x0, opt_len);

	pseudo_calculate_checksum(skb, &tcp_hdr->checksum);

}
#endif

//----------------- TCP Layer -----------------

void tcp_send_packet(struct sock_buff *skb, __u16 flags, struct tcp_option *opt)
{
	__u16 size = skb->size, opt_len;
	struct tcp_header *tcp_hdr;
	struct socket *sock = skb->sock;

	opt_len = opt ? opt->len : 0;

	skb->data -= TCP_HDR_LEN + opt_len;
	skb->size += TCP_HDR_LEN + opt_len;

	tcp_hdr = (struct tcp_header *)skb->data;
	//
	tcp_hdr->src_port = sock->saddr[SA_SRC].sin_port;
	tcp_hdr->dst_port = sock->saddr[SA_DST].sin_port;
	tcp_hdr->seq_num  = htonl(sock->seq_num);
	tcp_hdr->ack_num  = htonl(sock->ack_num);
	tcp_hdr->hdr_len  = (TCP_HDR_LEN + opt_len) >> 2;
	tcp_hdr->reserve1 = 0;
	tcp_hdr->reserve2 = 0;
	tcp_hdr->flg_urg  = !!(flags & FLG_URG);
	tcp_hdr->flg_ack  = !!(flags & FLG_ACK);
	tcp_hdr->flg_psh  = !!(flags & FLG_PSH);
	tcp_hdr->flg_rst  = !!(flags & FLG_RST);
	tcp_hdr->flg_syn  = !!(flags & FLG_SYN);
	tcp_hdr->flg_fin  = !!(flags & FLG_FIN);
	// fixme!!!
	if (flags & FLG_ACK)
		tcp_hdr->win_size = CPU_TO_BE16(457);
	else
		tcp_hdr->win_size = CPU_TO_BE16(14600);
	tcp_hdr->checksum = 0;
	tcp_hdr->urg_ptr  = 0;

	if (opt_len > 0)
		memcpy(tcp_hdr->options, opt, opt_len);

	pseudo_calculate_checksum(skb, &tcp_hdr->checksum);

	ip_send_packet(skb, PROT_TCP);

	if (flags & (FLG_SYN | FLG_FIN))
		sock->seq_num++;

	if (flags & FLG_PSH)
		sock->seq_num += size;
}

//----------------- UDP Layer -----------------
void udp_send_packet(struct sock_buff *skb)
{
	struct udp_header *udp_hdr;
	struct socket *sock = skb->sock;

	skb->data -= UDP_HDR_LEN;
	skb->size += UDP_HDR_LEN;

	udp_hdr = (struct udp_header *)skb->data;
	//
	udp_hdr->src_port = sock->saddr[SA_SRC].sin_port;
	udp_hdr->dst_port = sock->saddr[SA_DST].sin_port;
	udp_hdr->udp_len  = CPU_TO_BE16(skb->size);
	udp_hdr->checksum = 0;

	ip_send_packet(skb, PROT_UDP);
}

struct sock_buff *udp_recv_packet(struct socket *sock)
{
	u32 psr;
	struct sock_buff *skb;
	struct list_node *first;

	while (1)
	{
		int ret;
		char key;

		ret = uart_read(CONFIG_DBGU_ID, (u8 *)&key, 1, WAIT_ASYNC);
		if (ret > 0 && key == CHAR_CTRL_C)
			return NULL;

		netif_rx_poll();

		lock_irq_psr(psr);
		if (!list_is_empty(&sock->rx_qu))
		{
			unlock_irq_psr(psr);
			break;
		}
		unlock_irq_psr(psr);

		udelay(10);
	}

	lock_irq_psr(psr);
	first = sock->rx_qu.next;
	list_del_node(first);
	unlock_irq_psr(psr);

	skb = container_of(first, struct sock_buff, node);

	return skb;
}

static int udp_layer_deliver(struct sock_buff *skb, const struct ip_header *ip_hdr)
{
	struct udp_header *udp_hdr;
	struct socket *sock;

	udp_hdr = (struct udp_header *)skb->data;

	skb->data += UDP_HDR_LEN;
	skb->size -= UDP_HDR_LEN;

	sock = udp_search_socket(udp_hdr, ip_hdr);
	if (NULL == sock)
	{
		skb_free(skb);
		return -ENOENT;
	}

	memcpy(&sock->saddr[SA_DST].sin_addr, ip_hdr->src_ip, IPV4_ADR_LEN);
	sock->saddr[SA_DST].sin_port = udp_hdr->src_port;

	list_add_tail(&skb->node, &sock->rx_qu);

	return 0;
}

struct sock_buff *tcp_recv_packet(struct socket *sock)
{
	u32 psr;
	struct sock_buff *skb;
	struct list_node *first;

	while (1)
	{
		int ret;
		char key;

		ret = uart_read(CONFIG_DBGU_ID, (u8 *)&key, 1, WAIT_ASYNC);
		if (ret > 0 && key == CHAR_CTRL_C)
			return NULL;

		netif_rx_poll();

		lock_irq_psr(psr);
		if (!list_is_empty(&sock->rx_qu))
		{
			unlock_irq_psr(psr);
			break;
		}
		unlock_irq_psr(psr);

		udelay(10);
	}

	lock_irq_psr(psr);
	first = sock->rx_qu.next;
	list_del_node(first);
	unlock_irq_psr(psr);

	skb = container_of(first, struct sock_buff, node);

	return skb;
}

static int tcp_layer_deliver(struct sock_buff *skb, const struct ip_header *ip_hdr)
{
	__u16 hdr_len;
	struct tcp_header *tcp_hdr;
	struct socket *sock;

	tcp_hdr = (struct tcp_header *)skb->data;

	hdr_len = tcp_hdr->hdr_len << 2;

	skb->data += hdr_len;
	skb->size -= hdr_len;

	DPRINT("%s(): src_port = 0x%x, dst_port = 0x%x, flags = 0x%08x\n",
		__func__, tcp_hdr->src_port, tcp_hdr->dst_port, *(&tcp_hdr->ack_num + 1));

	sock = tcp_search_socket(tcp_hdr, ip_hdr);
	if (NULL == sock)
	{
		skb_free(skb);
		return -ENOENT;
	}

	skb->sock = sock;

	if (tcp_hdr->flg_syn)
	{
		sock->ack_num = BE32_TO_CPU(tcp_hdr->seq_num) + 1;

		if (tcp_hdr->flg_ack)
		{
			skb_free(skb);

			skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN, 0);
			// if null
			skb->sock = sock;
			
			tcp_send_packet(skb, FLG_ACK, NULL);

			if (TCPS_SYN_SENT == sock->state)
				sock->state = TCPS_ESTABLISHED;
			else
				BUG();
		}
		else
		{
			// TODO: add code here
			printf("%s() line %d: SYN not supported!\n", __func__, __LINE__);
		}
	}
	else
	{
		if (tcp_hdr->flg_fin)
		{
			sock->ack_num = BE32_TO_CPU(tcp_hdr->seq_num) + 1;
#if 0
			if (tcp_hdr->flg_psh)
			{
				sock->ack_num += skb->size;
			}
#endif
			skb_free(skb);

			skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN, 0);
			// if null
			skb->sock = sock;
			tcp_send_packet(skb, FLG_ACK, NULL);

			if (TCPS_FIN_WAIT1 == sock->state)
			{
				if (tcp_hdr->flg_ack)
					sock->state = TCPS_TIME_WAIT;
				else
					sock->state = TCPS_CLOSING;
			}
			else if (TCPS_FIN_WAIT2 == sock->state)
				sock->state = TCPS_TIME_WAIT;
			else if (TCPS_ESTABLISHED == sock->state)
				sock->state = TCPS_CLOSE_WAIT;
			else
				BUG();
		}
		else if (tcp_hdr->flg_ack && !tcp_hdr->flg_psh)
		{
			skb_free(skb);

			if (TCPS_FIN_WAIT1 == sock->state)
				sock->state = TCPS_FIN_WAIT2;
			else if (TCPS_CLOSING == sock->state)
				sock->state = TCPS_TIME_WAIT;
			else if (TCPS_LAST_ACK == sock->state)
				sock->state = TCPS_CLOSED;
		}

		if (tcp_hdr->flg_psh)
		{
			list_add_tail(&skb->node, &sock->rx_qu);
			sock->ack_num = BE32_TO_CPU(tcp_hdr->seq_num) + skb->size;
		}
	}

	return 0;
}

static int init_ping_packet(struct ping_packet *ping_pkt,
				const u8 *buff, u8 type, u32 size, u16 id, u16 seq)
{
	char *data = NULL;
	u32 hdr_len = sizeof(struct ping_packet);

	if (NULL == ping_pkt || NULL == buff)
	    return -EINVAL;

	ping_pkt->type   = type;
	ping_pkt->code   = 0;
	ping_pkt->chksum = 0;
	ping_pkt->id     = id;
	ping_pkt->seqno  = seq;

	data = (char *)ping_pkt + hdr_len;

	memcpy(data, buff, size - hdr_len);

	ping_pkt->chksum = ~net_calc_checksum(ping_pkt, size);

	return 0;
}

int ping_send(struct sock_buff *rx_skb, const struct ip_header *ip_hdr, u8 type)
{
	u8 ping_buff[PING_PACKET_LENGTH];
	size_t hdr_len = sizeof(struct ping_packet);
	struct socket sock;
	struct sock_buff *tx_skb;
	struct ether_header *eth_head;
	struct ping_packet *ping_pkt, *rx_ping_head;

	eth_head = (struct ether_header *)(rx_skb->data - IP_HDR_LEN - ETH_HDR_LEN);

	rx_ping_head = (struct ping_packet *)rx_skb->data;

	ping_pkt = (struct ping_packet *)ping_buff;

	tx_skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN, PING_PACKET_LENGTH);
	if (NULL == tx_skb)
	{
	    printf("%s(): skb_alloc return null\n", __func__);
	    return -EBUSY;
	}

	// ?
	memcpy(&sock.saddr[SA_DST].sin_addr, ip_hdr->src_ip, IPV4_ADR_LEN);
	memcpy(&sock.saddr[SA_SRC].sin_addr, ip_hdr->des_ip, IPV4_ADR_LEN);

	tx_skb->sock = &sock;

	init_ping_packet(ping_pkt, (u8 *)rx_ping_head + hdr_len, type, PING_PACKET_LENGTH, rx_ping_head->id, rx_ping_head->seqno);

	memcpy(tx_skb->data, ping_pkt, PING_PACKET_LENGTH);

	ip_send_packet(tx_skb, PROT_ICMP);

	return 0;
}

static int ping_recv(struct sock_buff *rx_skb, const struct ip_header *ip_hdr)
{
	struct ping_packet *ping_hdr;

	ping_hdr = (struct ping_packet *)rx_skb->data;

	printf("%d bytes from %d.%d.%d.%d: icmp_seq=%d ttl=%d time=12.7 ms\n", // fixme :-)
	        rx_skb->size,
	        ip_hdr->src_ip[0],
	        ip_hdr->src_ip[1],
	        ip_hdr->src_ip[2],
	        ip_hdr->src_ip[3],
	        BE16_TO_CPU(ping_hdr->seqno),
	        ip_hdr->ttl);

	return 0;
}

#if 0
int ping_request(struct socket *socket, struct eth_addr *remote_addr)
{
	int i, j;
	u8  ping_buff[PING_PACKET_LENGTH];
	u32 hdr_len = sizeof(struct ping_packet);
	struct sock_buff *skb;
	struct ping_packet *ping_pkt;

	for (i = 0; i < PING_MAX_TIMES; i++)
	{
	    skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN, PING_PACKET_LENGTH);
	    if (NULL == skb)
	    {
			printf("%s(): skb_alloc failed\n", __func__);
			return -ENOMEM;
	    }

	    skb->sock = socket;
		memcpy(&skb->remote_addr.sin_addr, remote_addr->ip, IPV4_ADR_LEN);

	    ping_pkt = (struct ping_packet *)ping_buff;

	    init_ping_packet(ping_pkt, ping_buff + hdr_len,
			ICMP_TYPE_ECHO_REQUEST, PING_PACKET_LENGTH, CPU_TO_BE16(PING_DEFUALT_PID), CPU_TO_BE16(i + 1));

	    memcpy(skb->data, ping_pkt, PING_PACKET_LENGTH);

		ip_send_packet(skb, PROT_ICMP);

		for (j = 0; j < 10; j++)
		{
			mdelay(10);
			netif_rx_poll();
		}
	}

	return 0;
}
#endif

static int icmp_deliver(struct sock_buff *skb, const struct ip_header *ip_hdr)
{
	struct ping_packet *ping_hdr;
	struct ether_header *eth_head;

	ping_hdr = (struct ping_packet *)skb->data;
	eth_head = (struct ether_header *)(skb->data - IP_HDR_LEN - ETH_HDR_LEN);

	switch(ping_hdr->type)
	{
	case ICMP_TYPE_ECHO_REQUEST:
	    ping_send(skb, ip_hdr, ICMP_TYPE_ECHO_REPLY);

	    break;

	case ICMP_TYPE_ECHO_REPLY:
	    ping_recv(skb, ip_hdr);

	    break;

	case ICMP_TYPE_DEST_UNREACHABLE:
	    printf("From %d.%d.%d.%d: icmp_seq=%d Destination Host Unreachable\n",
	            ip_hdr->des_ip[0],
	            ip_hdr->des_ip[1],
	            ip_hdr->des_ip[2],
	            ip_hdr->des_ip[3],
	            BE16_TO_CPU(ping_hdr->seqno));

	    break;

	default:
	    break;
	}

	skb_free(skb);

	return 0;
}

int ip_layer_deliver(struct sock_buff *skb)
{
	struct ip_header *ip_hdr;
	u8 ip_hdr_len;

	ip_hdr = (struct ip_header *)skb->data;
	ip_hdr_len = (ip_hdr->ver_len & 0xf) << 2;

	skb->data += ip_hdr_len;
	skb->size = BE16_TO_CPU(ip_hdr->total_len) - ip_hdr_len;

	if (ip_hdr_len < IP_HDR_LEN)
	{
		printf("Warning: ip_hdr head len not match\n");
		return -1;
	}

	switch(ip_hdr->up_prot)
	{
	case PROT_UDP:
		// printf("\tUDP received!\n");
		udp_layer_deliver(skb, ip_hdr);
		break;

	case PROT_ICMP:
		// printf("\n\tICMP received!\n");
		icmp_deliver(skb, ip_hdr);
		break;

	case PROT_TCP:
		// printf("\tTCP received!\n");
		tcp_layer_deliver(skb, ip_hdr);
		break;

	case PROT_IGMP:
		// printf("\tIGMP received!\n");
		skb_free(skb);
		break;

	case PROT_OSPF:
		printf("\tOSPF received!\n");
		skb_free(skb);
		break;

	default:
		printf("\n\tUnknown IP Protocol!\n");
		skb_free(skb);
		return 0;
	}

	return ip_hdr->up_prot;
}

void ip_send_packet(struct sock_buff *skb, u8 prot)
{
	u8 mac[MAC_ADR_LEN];
	u8 *pmac = NULL;
	u32 nip;
	struct ip_header *ip_hdr;
	struct eth_addr *remote_addr = NULL;
	struct socket *sock = skb->sock;
	static __u16 ip_id = 1;

	skb->data -= IP_HDR_LEN;
	skb->size += IP_HDR_LEN;

	ip_hdr = (struct ip_header *)skb->data;

	ip_hdr->ver_len   = 0x45;
	ip_hdr->tos       = 0;
	ip_hdr->total_len = (u16)CPU_TO_BE16((u16)skb->size);
	ip_hdr->id        = (u16)CPU_TO_BE16(ip_id); //
	ip_hdr->flag_frag = (u16)CPU_TO_BE16(0x4000);
	ip_hdr->ttl       = 64;
	ip_hdr->up_prot   = prot;
	ip_hdr->chksum    = 0;

	ip_id++;

	memcpy(ip_hdr->src_ip, &sock->saddr[SA_SRC].sin_addr, IPV4_ADR_LEN);
	memcpy(ip_hdr->des_ip, &sock->saddr[SA_DST].sin_addr, IPV4_ADR_LEN);

	ip_hdr->chksum = ~net_calc_checksum(ip_hdr, IP_HDR_LEN);

	nip = sock->saddr[SA_DST].sin_addr.s_addr;

	if (ip_is_bcast(BE32_TO_CPU(nip)))
	{
		mac_fill_bcast(mac);
		pmac = mac;
	}
	else
	{
		remote_addr = getaddr(nip);

		if (NULL == remote_addr)
	    {
			remote_addr = gethostaddr(sock->saddr[SA_DST].sin_addr.s_addr);
			if (NULL == remote_addr)
			{
				DPRINT("%s(): addr error!\n", __func__);
				return;
			}
		}

		pmac = remote_addr->mac;
	}

	ether_send_packet(skb, pmac, ETH_TYPE_IP);
}

//------------------------ ARP Layer -------------------------
void arp_send_packet(const u8 nip[], const u8 *mac, u16 op_code)
{
	struct sock_buff *skb;
	struct arp_packet *arp_pkt;
	u8 mac_addr[MAC_ADR_LEN];
	u32 src_ip;

	skb = skb_alloc(ETH_HDR_LEN, ARP_PKT_LEN);
	if (NULL == skb)
	{
		printf("%s: fail to alloc skb!\n", __func__);
		return;
	}

	arp_pkt = (struct arp_packet *)skb->data;

	arp_pkt->hard_type = CPU_TO_BE16(1);
	arp_pkt->prot_type = ETH_TYPE_IP;
	arp_pkt->hard_size = MAC_ADR_LEN;
	arp_pkt->prot_size = IPV4_ADR_LEN;
	arp_pkt->op_code   = op_code;

	ndev_ioctl(NULL, NIOC_GET_MAC, mac_addr); //fixme for failure
	memcpy(arp_pkt->src_mac, mac_addr, MAC_ADR_LEN);

	ndev_ioctl(NULL, NIOC_GET_IP, &src_ip);
	memcpy(arp_pkt->src_ip, &src_ip, IPV4_ADR_LEN);

	if (NULL == mac)
		mac_fill_bcast(arp_pkt->des_mac);
	else
		memcpy(arp_pkt->des_mac, mac, MAC_ADR_LEN);
	memcpy(arp_pkt->des_ip, nip, IPV4_ADR_LEN);

	ether_send_packet(skb, arp_pkt->des_mac, ETH_TYPE_ARP);
}

static int arp_recv_packet(struct sock_buff *skb)
{
	struct arp_packet *arp_pkt;
	u32 ip;
	u32 local_ip;

	arp_pkt = (struct arp_packet *)skb->data;

	if (arp_pkt->prot_type != ETH_TYPE_IP)
	{
		printf("\tProt Error!\n");
		return -1;
	}

	memcpy(&ip, arp_pkt->src_ip, 4);

	DPRINT("\t%s ARP received from: %d.%d.%d.%d\n",
		g_arp_desc[BE16_TO_CPU(arp_pkt->op_code)],
		arp_pkt->src_ip[0], arp_pkt->src_ip[1], arp_pkt->src_ip[2], arp_pkt->src_ip[3]);

	switch (arp_pkt->op_code)
	{
	case ARP_OP_REP:
		if (getaddr(ip) == NULL)
		{
			struct host_addr *host;

			host = malloc(sizeof(struct host_addr));
			if (NULL == host)
				return -ENOMEM;

			memcpy(host->in_addr.ip, arp_pkt->src_ip, IPV4_ADR_LEN);
			memcpy(host->in_addr.mac, arp_pkt->src_mac, MAC_ADR_LEN);
#if 0
			printf("ARP %d.%d.%d.%d<-->%02x.%02x.%02x.%02x.%02x.%02x\n",
				   host->in_addr.ip[0],
				   host->in_addr.ip[1],
				   host->in_addr.ip[2],
				   host->in_addr.ip[3],
				   host->in_addr.mac[0],
				   host->in_addr.mac[1],
				   host->in_addr.mac[2],
				   host->in_addr.mac[3],
				   host->in_addr.mac[4],
				   host->in_addr.mac[5]
				   );
#endif

			list_add_tail(&host->node, &g_host_list);
		}

		break;

	case ARP_OP_REQ:
		ndev_ioctl(NULL, NIOC_GET_IP, &local_ip);
		if (0 == memcmp(arp_pkt->des_ip, &local_ip, IPV4_ADR_LEN))
			arp_send_packet(arp_pkt->src_ip, arp_pkt->src_mac, ARP_OP_REP);
		break;

	default:
		printf("\t%s(): op_code error!\n", __func__);
		break;
	}

	skb_free(skb);

	return 0;
}

//-----------------------------------------------
int netif_rx(struct sock_buff *skb)
{
	struct ether_header *eth_head;

	eth_head = (struct ether_header *)skb->data;

	skb->data += ETH_HDR_LEN;
	skb->size -= ETH_HDR_LEN;

	switch(eth_head->frame_type)
	{
	case ETH_TYPE_ARP:
		// printf("\tARP received.\n");
		arp_recv_packet(skb);
		break;

	case ETH_TYPE_RARP:
		// printf("\tRARP packet received.\n");
		break;

	case ETH_TYPE_IP:
		// printf("\tIP packet received.\n");
		ip_layer_deliver(skb);
		break;

	default:
		// DPRINT("\tframe type error (= 0x%04x)!\n", eth_head->frame_type);
		skb_free(skb);
		break;
	}

	return 0;
}

//------------------ Send Package to Hardware -----------------
void ether_send_packet(struct sock_buff *skb, const u8 mac[], u16 eth_type)
{
	struct ether_header *eth_head;
	u8 mac_addr[MAC_ADR_LEN];

	skb->data -= ETH_HDR_LEN;
	skb->size += ETH_HDR_LEN;

	if (skb->data != skb->head)
	{
		printf("%s(): skb len error! (data = 0x%p, head = 0x%p)\n",
			__func__, skb->data, skb->head);
		return;
	}

	eth_head = (struct ether_header *)skb->data;

	memcpy(eth_head->des_mac, mac, MAC_ADR_LEN);

	ndev_ioctl(NULL, NIOC_GET_MAC, mac_addr); //fixme for failure
	memcpy(eth_head->src_mac, mac_addr, MAC_ADR_LEN);

	eth_head->frame_type = eth_type;

	g_curr_ndev->send_packet(g_curr_ndev, skb);

	skb_free(skb);
}

u16 net_calc_checksum(const void *buff, u32 size)
{
	int n;
	u32	chksum;
	const u16 *p;

	chksum = 0;

	for (n = size, p = buff; n > 0; n -= 2, p++)
		chksum += *p;

	chksum = (chksum & 0xffff) + (chksum >> 16);
	chksum = (chksum & 0xffff) + (chksum >> 16);

	return chksum & 0xffff;
}

// fixme!
struct sock_buff *skb_alloc(u32 prot_len, u32 data_len)
{
	struct sock_buff *skb;

	skb = malloc(sizeof(struct sock_buff));
	if (NULL == skb)
		return NULL;

	skb->head = malloc((prot_len + data_len + 1) & ~1);
	if (NULL == skb->head)
	{
		DPRINT("%s(): malloc failed (size = %d bytes)!\n",
			__func__, prot_len + data_len);
		return NULL;
	}

	skb->data = skb->head + prot_len;
	skb->size = data_len;

	list_head_init(&skb->node);

	return skb;
}

void skb_free(struct sock_buff *skb)
{
	BUG_ON(!skb);

	free(skb->head);
	free(skb);
}

struct eth_addr *getaddr(u32 nip)
{
	struct list_node *iter;
	struct host_addr *host;
	u32 psr;
	u32 *dip;
	struct eth_addr *addr = NULL;

	lock_irq_psr(psr);

	list_for_each(iter, &g_host_list)
	{
		host = container_of(iter, struct host_addr, node);

		dip = (u32 *)host->in_addr.ip;
		if (*dip == nip)
		{
			addr = &host->in_addr;
			break;
		}
	}

	unlock_irq_psr(psr);

	return addr;
}

struct list_node *net_get_device_list(void)
{
	return &g_ndev_list;
}

struct net_device *net_get_dev(const char *ifx)
{
	struct net_device *ndev;
	struct list_node *iter;

	list_for_each(iter, net_get_device_list())
	{
		ndev = container_of(iter, struct net_device, ndev_node);

		if (!strcmp(ifx, ndev->ifx_name))
		{
			return ndev;
		}
	}

	return NULL;
}

int net_dev_register(struct net_device *ndev)
{
	int index;
	struct mii_phy *phy;
	static u8 mac_addr[] = CONFIG_MAC_ADDR;

	if (!ndev || !ndev->send_packet || !ndev->set_mac_addr)
	{
		return -EINVAL;
	}

	if (!ndev->chip_name)
	{
		printf("Warning: chip_name is NOT set!\n");
	}

	mac_addr[0] = (mac_addr[0] & ~1) + 2; // fixme

	if (ndev_ioctl(ndev, NIOC_SET_MAC, mac_addr) < 0)
	{
		DPRINT("%s(): fail to set MAC address!\n");
		return -EIO;
	}

	list_add_tail(&ndev->ndev_node, &g_ndev_list);

	if (!g_curr_ndev)
		g_curr_ndev = ndev;

	// fixme
	if (!ndev->phy_mask || !ndev->mdio_read || !ndev->mdio_write)
		return 0;

	// detecting PHY
	for (index = 0; index < 32; index++)
	{
		if (!((1 << index) & ndev->phy_mask))
			continue;

		phy = mii_phy_probe(ndev, index);

		if (phy)
		{
			phy->ndev = ndev;
			list_add_tail(&phy->phy_node, &ndev->phy_list);

			mii_reset_phy(ndev, phy);

			// fixme
			printf("PHY found @ MII[%d]: ID1 = 0x%04x, ID2 = 0x%04x\n",
				index, phy->ven_id, phy->dev_id);
		}
	}

	return 0;
}

struct net_device *net_dev_new(u32 chip_size)
{
	struct net_device *ndev;
	u32 core_size = (sizeof(struct net_device) + WORD_SIZE - 1) & ~(WORD_SIZE - 1);

	ndev = zalloc(core_size + chip_size);
	if (NULL == ndev)
		return NULL;

	ndev->chip = (struct net_device *)((u8 *)ndev + core_size);
	ndev->phy_mask = 0xFFFFFFFF;

	// set default name
	snprintf(ndev->ifx_name, NET_NAME_LEN, "eth%d", ndev_count);
	ndev_count++;

	list_head_init(&ndev->ndev_node);
	list_head_init(&ndev->phy_list);

	return ndev;
}

#ifndef CONFIG_IRQ_SUPPORT
int netif_rx_poll()
{
	if (!g_curr_ndev)
		return -ENODEV;

	if (!g_curr_ndev->ndev_poll)
		return 0;

	return g_curr_ndev->ndev_poll(g_curr_ndev);
}
#endif

int net_check_link_status()
{
	int speed, phy_count;
	struct net_device *ndev;
	struct mii_phy *phy;
	struct list_node *ndev_ln, *phy_ln;

	list_for_each(ndev_ln, &g_ndev_list)
	{
		ndev = container_of(ndev_ln, struct net_device, ndev_node);
		phy_count = 0;

		printf("Detecting ethernet speed for %s(%s):",
			ndev->ifx_name, ndev->chip_name);

		list_for_each(phy_ln, &ndev->phy_list)
		{
			phy = container_of(phy_ln, struct mii_phy, phy_node);

			printf("\n\tPHY%d -> ", phy->mii_id);

			speed = mii_get_link_speed(phy);

			if (speed < 0)
			{
				printf("NOT linked, please check the cable!\n");
			}
			else
			{
				switch(speed)
				{
				case ETHER_SPEED_10M_HD:
					printf("10M Half Duplex activated!\n");
					break;

				case ETHER_SPEED_10M_FD:
					printf("10M Full Duplex activated!\n");
					break;

				case ETHER_SPEED_100M_HD:
					printf("100M Half Duplex activated!\n");
					break;

				case ETHER_SPEED_100M_FD:
					printf("100M Full Duplex activated!\n");
					break;

				default:
					printf("Unknown speed (%d)!\n", speed);
					break;
				}
			}

			phy_count++;
		}

		if (0 == phy_count)
			printf(" no PHY found!\n");
	}

	return 0;
}

// fixme: to be moved to net_api.c
int ndev_ioctl(struct net_device *ndev, int cmd, void *arg)
{
	struct mii_phy *phy;
	struct link_status *status;
	u16 speed;

	// fixme!!!
	if (NULL == ndev)
		ndev = g_curr_ndev;

	switch (cmd)
	{
	case NIOC_GET_IP:
		*(u32 *)arg = ndev->ip;
		break;

	case NIOC_SET_IP:
		ndev->ip = (u32)arg;
		break;

	case NIOC_GET_MASK:
		*(u32 *)arg = ndev->mask;
		break;

	case NIOC_SET_MASK:
		ndev->mask = (u32)arg;
		break;

	case NIOC_GET_MAC:
		memcpy(arg, ndev->mac_addr, MAC_ADR_LEN);
		break;

	case NIOC_SET_MAC:
		if (!ndev->set_mac_addr)
			return -EINVAL;

		memcpy(ndev->mac_addr, arg, MAC_ADR_LEN);
		ndev->set_mac_addr(ndev, arg);
		break;

	case NIOC_GET_STATUS:
		if (list_is_empty(&ndev->phy_list))
			return -ENODEV;

		phy = container_of(ndev->phy_list.next, struct mii_phy, phy_node);
		status = (struct link_status *)arg;

		status->connected = !!(ndev->mdio_read(ndev, phy->mii_id, MII_REG_BMS) & 0x20);
		if (!status->connected)
		{
			status->link_speed = ETHER_SPEED_UNKNOW;
			break;
		}

		speed = ndev->mdio_read(ndev, phy->mii_id, MII_REG_STAT);
		switch (speed >> 12)
		{
		case 1:
			status->link_speed = ETHER_SPEED_10M_HD;
			break;

		case 2:
			status->link_speed = ETHER_SPEED_10M_FD;
			break;

		case 4:
			status->link_speed = ETHER_SPEED_100M_HD;
			break;

		case 8:
			status->link_speed = ETHER_SPEED_100M_FD;
			break;

		default:
			break;
		}

	default:
		return -EINVAL;
	}

	return 0;
}

void socket_init(void);

static int __INIT__ net_core_init(void)
{
	socket_init();

	list_head_init(&g_host_list);
	list_head_init(&g_ndev_list);

	g_curr_ndev = NULL;

	return 0;
}

SUBSYS_INIT(net_core_init);
