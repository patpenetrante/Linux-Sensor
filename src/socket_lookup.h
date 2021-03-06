/*
 * The following code was conveniently borrowed form the xt_socket
 * iptables module and used with minor modifications.  Thanks guys!
 *
 * Transparent proxy support for Linux/iptables
 *
 * Copyright (C) 2007-2008 BalaBit IT Ltd.
 * Author: Krisztian Kovacs
 *
 *
 * Modifications Copyright (C) 2011 Battelle Memorial Institute
 *   <http://www.battelle.org>
 *
 * Author: Brandon Carpenter
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/version.h>
#include <linux/skbuff.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/icmp.h>
#include <net/sock.h>
#include <net/inet_sock.h>
#include <net/netfilter/nf_tproxy_core.h>

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#include <net/ipv6.h>
#endif

#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
#define XT_SOCKET_HAVE_CONNTRACK 1
#include <net/netfilter/nf_conntrack.h>
#endif

// Not defined in older kernels
#ifndef NFT_LOOKUP_ANY
#define NFT_LOOKUP_ANY         0
#define NFT_LOOKUP_LISTENER    1
#define NFT_LOOKUP_ESTABLISHED 2
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
#define inet_rcv_saddr rcv_saddr
#endif

static void put_sock(struct sock *sk)
{
	if (sk->sk_state == TCP_TIME_WAIT)
		inet_twsk_put(inet_twsk(sk));
	else
		sock_put(sk);
}

static int extract_icmp4_fields(const struct sk_buff *skb, u8 *protocol,
		__be32 *raddr, __be32 *laddr, __be16 *rport, __be16 *lport)
{
	unsigned int outside_hdrlen = ip_hdrlen(skb);
	struct iphdr *inside_iph, _inside_iph;
	struct icmphdr *icmph, _icmph;
	__be16 *ports, _ports[2];

	icmph = skb_header_pointer(skb, outside_hdrlen,
				   sizeof(_icmph), &_icmph);
	if (icmph == NULL)
		return 1;

	switch (icmph->type) {
	case ICMP_DEST_UNREACH:
	case ICMP_SOURCE_QUENCH:
	case ICMP_REDIRECT:
	case ICMP_TIME_EXCEEDED:
	case ICMP_PARAMETERPROB:
		break;
	default:
		return 1;
	}

	inside_iph = skb_header_pointer(skb, outside_hdrlen +
					sizeof(struct icmphdr),
					sizeof(_inside_iph), &_inside_iph);
	if (inside_iph == NULL)
		return 1;

	if (inside_iph->protocol != IPPROTO_TCP &&
	    inside_iph->protocol != IPPROTO_UDP)
		return 1;

	ports = skb_header_pointer(skb, outside_hdrlen +
				   sizeof(struct icmphdr) +
				   (inside_iph->ihl << 2),
				   sizeof(_ports), &_ports);
	if (ports == NULL)
		return 1;

	/* the inside IP packet is the one quoted from our side, thus
	 * its saddr is the local address */
	*protocol = inside_iph->protocol;
	*laddr = inside_iph->saddr;
	*lport = ports[0];
	*raddr = inside_iph->daddr;
	*rport = ports[1];

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
static inline struct sock *
__nf_tproxy_get_sock_v4(struct net *net, const u8 protocol,
		      const __be32 saddr, const __be32 daddr,
		      const __be16 sport, const __be16 dport,
		      const struct net_device *in, int lookup_type)
{
	struct sock *sk;

	/* look up socket */
	switch (protocol) {
	case IPPROTO_TCP:
		switch (lookup_type) {
		case NFT_LOOKUP_ANY:
			sk = __inet_lookup(net, &tcp_hashinfo,
					   saddr, sport, daddr, dport,
					   in->ifindex);
			break;
		case NFT_LOOKUP_LISTENER:
			sk = inet_lookup_listener(net, &tcp_hashinfo,
						    daddr, dport,
						    in->ifindex);

			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */

			break;
		case NFT_LOOKUP_ESTABLISHED:
			sk = inet_lookup_established(net, &tcp_hashinfo,
						    saddr, sport, daddr, dport,
						    in->ifindex);
			break;
		default:
			WARN_ON(1);
			sk = NULL;
			break;
		}
		break;
	case IPPROTO_UDP:
		sk = udp4_lib_lookup(net, saddr, sport, daddr, dport,
				     in->ifindex);
		if (sk && lookup_type != NFT_LOOKUP_ANY) {
			int connected = (sk->sk_state == TCP_ESTABLISHED);
			int wildcard = (inet_sk(sk)->inet_rcv_saddr == 0);

			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */
			if ((lookup_type == NFT_LOOKUP_ESTABLISHED && (!connected || wildcard)) ||
			    (lookup_type == NFT_LOOKUP_LISTENER && connected)) {
				sock_put(sk);
				sk = NULL;
			}
		}
		break;
	default:
		WARN_ON(1);
		sk = NULL;
	}

	return sk;
}

#define nf_tproxy_get_sock_v4(...) __nf_tproxy_get_sock_v4(__VA_ARGS__)
#endif

static struct sock *lookup_v4_sock(const struct sk_buff *skb,
		const struct net_device *indev)
{
	struct iphdr *iph = ip_hdr(skb);
	struct udphdr _hdr, *hp = NULL;
	struct sock *sk;
	__be32 daddr, saddr;
	__be16 dport, sport;
	u8 protocol;
#ifdef XT_SOCKET_HAVE_CONNTRACK
	struct nf_conn const *ct;
	enum ip_conntrack_info ctinfo;
#endif

	if (iph->protocol == IPPROTO_UDP || iph->protocol == IPPROTO_TCP) {
		hp = skb_header_pointer(skb, ip_hdrlen(skb),
					sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return NULL;

		protocol = iph->protocol;
		saddr = iph->saddr;
		sport = hp->source;
		daddr = iph->daddr;
		dport = hp->dest;

	} else if (iph->protocol == IPPROTO_ICMP) {
		if (extract_icmp4_fields(skb, &protocol, &saddr, &daddr,
					&sport, &dport))
			return NULL;
	} else {
		return NULL;
	}

#ifdef XT_SOCKET_HAVE_CONNTRACK
	/* Do the lookup with the original socket address in case this is a
	 * reply packet of an established SNAT-ted connection. */

	ct = nf_ct_get(skb, &ctinfo);
	if (ct &&
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
			!nf_ct_is_untracked(ct) &&
#else
			(ct != &nf_conntrack_untracked) &&
#endif
	    ((iph->protocol != IPPROTO_ICMP &&
	      ctinfo == IP_CT_IS_REPLY + IP_CT_ESTABLISHED) ||
	     (iph->protocol == IPPROTO_ICMP &&
	      ctinfo == IP_CT_IS_REPLY + IP_CT_RELATED)) &&
	    (ct->status & IPS_SRC_NAT_DONE)) {

		daddr = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip;
		dport = (iph->protocol == IPPROTO_TCP) ?
			ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.tcp.port :
			ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.udp.port;
	}
#endif

	if (!(sk = nf_tproxy_get_sock_v4(dev_net(skb->dev), protocol,
			saddr, daddr, sport, dport, indev, NFT_LOOKUP_ANY)))
		return NULL;

	/* Ignore sockets listening on INADDR_ANY */
	if (sk->sk_state != TCP_TIME_WAIT && inet_sk(sk)->inet_rcv_saddr == 0) {
		put_sock(sk);
		return NULL;
	}
	return sk;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
// This prototype doesn't appear in include/net/ipv6.h in older kernels
extern int ipv6_skip_exthdr(const struct sk_buff *, int , u8 *);

static int extract_icmp6_fields(const struct sk_buff *skb,
		unsigned int outside_hdrlen, int *protocol, struct in6_addr **raddr,
		struct in6_addr **laddr, __be16 *rport, __be16 *lport)
{
	struct ipv6hdr *inside_iph, _inside_iph;
	struct icmp6hdr *icmph, _icmph;
	__be16 *ports, _ports[2];
	u8 inside_nexthdr;
	int inside_hdrlen;

	icmph = skb_header_pointer(skb, outside_hdrlen,
				   sizeof(_icmph), &_icmph);
	if (icmph == NULL)
		return 1;

	if (icmph->icmp6_type & ICMPV6_INFOMSG_MASK)
		return 1;

	inside_iph = skb_header_pointer(skb, outside_hdrlen + sizeof(_icmph), sizeof(_inside_iph), &_inside_iph);
	if (inside_iph == NULL)
		return 1;
	inside_nexthdr = inside_iph->nexthdr;

	inside_hdrlen = ipv6_skip_exthdr(skb, outside_hdrlen + sizeof(_icmph) + sizeof(_inside_iph), &inside_nexthdr);
	if (inside_hdrlen < 0)
		return 1; /* hjm: Packet has no/incomplete transport layer headers. */

	if (inside_nexthdr != IPPROTO_TCP &&
	    inside_nexthdr != IPPROTO_UDP)
		return 1;

	ports = skb_header_pointer(skb, inside_hdrlen,
				   sizeof(_ports), &_ports);
	if (ports == NULL)
		return 1;

	/* the inside IP packet is the one quoted from our side, thus
	 * its saddr is the local address */
	*protocol = inside_nexthdr;
	*laddr = &inside_iph->saddr;
	*lport = ports[0];
	*raddr = &inside_iph->daddr;
	*rport = ports[1];

	return 0;
}

int ipv6_find_hdr(const struct sk_buff *skb, unsigned int *offset,
		  int target, unsigned short *fragoff)
{
	unsigned int start = skb_network_offset(skb) + sizeof(struct ipv6hdr);
	u8 nexthdr = ipv6_hdr(skb)->nexthdr;
	unsigned int len = skb->len - start;

	if (fragoff)
		*fragoff = 0;

	while (nexthdr != target) {
		struct ipv6_opt_hdr _hdr, *hp;
		unsigned int hdrlen;

		if ((!ipv6_ext_hdr(nexthdr)) || nexthdr == NEXTHDR_NONE) {
			if (target < 0)
				break;
			return -ENOENT;
		}

		hp = skb_header_pointer(skb, start, sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return -EBADMSG;
		if (nexthdr == NEXTHDR_FRAGMENT) {
			unsigned short _frag_off;
			__be16 *fp;
			fp = skb_header_pointer(skb,
						start+offsetof(struct frag_hdr,
							       frag_off),
						sizeof(_frag_off),
						&_frag_off);
			if (fp == NULL)
				return -EBADMSG;

			_frag_off = ntohs(*fp) & ~0x7;
			if (_frag_off) {
				if (target < 0 &&
				    ((!ipv6_ext_hdr(hp->nexthdr)) ||
				     hp->nexthdr == NEXTHDR_NONE)) {
					if (fragoff)
						*fragoff = _frag_off;
					return hp->nexthdr;
				}
				return -ENOENT;
			}
			hdrlen = 8;
		} else if (nexthdr == NEXTHDR_AUTH)
			hdrlen = (hp->hdrlen + 2) << 2;
		else
			hdrlen = ipv6_optlen(hp);

		nexthdr = hp->nexthdr;
		len -= hdrlen;
		start += hdrlen;
	}

	*offset = start;
	return nexthdr;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
extern struct sock *inet6_lookup(struct net *net, struct inet_hashinfo *hashinfo,
				 const struct in6_addr *saddr, const __be16 sport,
				 const struct in6_addr *daddr, const __be16 dport,
				 const int dif);

extern struct sock *inet6_lookup_listener(struct net *net,
					  struct inet_hashinfo *hashinfo,
					  const struct in6_addr *daddr,
					  const unsigned short hnum,
					  const int dif);

extern struct sock *__inet6_lookup_established(struct net *net,
					   struct inet_hashinfo *hashinfo,
					   const struct in6_addr *saddr,
					   const __be16 sport,
					   const struct in6_addr *daddr,
					   const u16 hnum,
					   const int dif);

extern struct sock *__udp6_lib_lookup(struct net *net,
				      struct in6_addr *saddr, __be16 sport,
				      struct in6_addr *daddr, __be16 dport,
				      int dif, struct udp_table *udptable);

#define udp6_lib_lookup(net, saddr, sport, daddr, dport, dif) \
	__udp6_lib_lookup((net), (struct in6_addr *) (saddr), (sport), \
			(struct in6_addr *) (daddr), (dport), (dif), &udp_table)


static inline struct sock *
nf_tproxy_get_sock_v6(struct net *net, const u8 protocol,
		      const struct in6_addr *saddr, const struct in6_addr *daddr,
		      const __be16 sport, const __be16 dport,
		      const struct net_device *in, int lookup_type)
{
	struct sock *sk;

	/* look up socket */
	switch (protocol) {
	case IPPROTO_TCP:
		switch (lookup_type) {
		case NFT_LOOKUP_ANY:
			sk = inet6_lookup(net, &tcp_hashinfo,
					  saddr, sport, daddr, dport,
					  in->ifindex);
			break;
		case NFT_LOOKUP_LISTENER:
			sk = inet6_lookup_listener(net, &tcp_hashinfo,
						   daddr, ntohs(dport),
						   in->ifindex);

			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */

			break;
		case NFT_LOOKUP_ESTABLISHED:
			sk = __inet6_lookup_established(net, &tcp_hashinfo,
							saddr, sport, daddr, ntohs(dport),
							in->ifindex);
			break;
		default:
			WARN_ON(1);
			sk = NULL;
			break;
		}
		break;
	case IPPROTO_UDP:
		sk = udp6_lib_lookup(net, saddr, sport, daddr, dport, in->ifindex);
		if (sk && lookup_type != NFT_LOOKUP_ANY) {
			int connected = (sk->sk_state == TCP_ESTABLISHED);
			int wildcard = ipv6_addr_any(&inet6_sk(sk)->rcv_saddr);

			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */
			if ((lookup_type == NFT_LOOKUP_ESTABLISHED && (!connected || wildcard)) ||
			    (lookup_type == NFT_LOOKUP_LISTENER && connected)) {
				sock_put(sk);
				sk = NULL;
			}
		}
		break;
	default:
		WARN_ON(1);
		sk = NULL;
	}

	return sk;
}
#endif

static struct sock *lookup_v6_sock(const struct sk_buff *skb,
			const struct net_device *indev)
{
	struct ipv6hdr *iph = ipv6_hdr(skb);
	struct udphdr _hdr, *hp = NULL;
	struct sock *sk;
	struct in6_addr *daddr, *saddr;
	__be16 dport, sport;
	int thoff, tproto;

	tproto = ipv6_find_hdr(skb, &thoff, -1, NULL);
	if (tproto < 0)
		// unable to find transport header in IPv6 packet
		return NULL;

	if (tproto == IPPROTO_UDP || tproto == IPPROTO_TCP) {
		hp = skb_header_pointer(skb, thoff,
					sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return NULL;

		saddr = &iph->saddr;
		sport = hp->source;
		daddr = &iph->daddr;
		dport = hp->dest;

	} else if (tproto == IPPROTO_ICMPV6) {
		if (extract_icmp6_fields(skb, thoff, &tproto, &saddr, &daddr,
					 &sport, &dport))
			return NULL;
	} else {
		return NULL;
	}

	if (!(sk = nf_tproxy_get_sock_v6(dev_net(skb->dev), tproto,
			saddr, daddr, sport, dport, indev, NFT_LOOKUP_ANY)))
		return NULL;

	/* Ignore sockets listening on INADDR_ANY */
	if (sk->sk_state != TCP_TIME_WAIT && inet_sk(sk)->inet_rcv_saddr == 0) {
		put_sock(sk);
		return NULL;
	}
	return sk;
}
#endif

