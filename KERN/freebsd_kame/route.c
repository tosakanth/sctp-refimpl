/*
 * Copyright (c) 1980, 1986, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)route.c	8.3 (Berkeley) 1/9/95
 * $FreeBSD: src/sys/net/route.c,v 1.59.2.7 2001/12/20 10:30:17 ru Exp $
 */

#include "opt_inet.h"
#include "opt_mrouting.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/domain.h>
#include <sys/kernel.h>

#include <net/if.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/ip_mroute.h>
#include "opt_sctp.h"

#define	SA(p) ((struct sockaddr *)(p))

struct route_cb route_cb;
static struct rtstat rtstat;
struct radix_node_head *rt_tables[AF_MAX+1];

static int	rttrash;		/* routes not in table but not freed */

static struct callout rt_timer_ch;

/* XXX do these values make any sense? */
static long rt_cache_max;
static long rt_cache_hiwat;
static long rt_cache_lowat;

static int rt_cachetimeout = 3600;	/* should be configurable */
static struct rttimer_queue *rt_cache_timeout_q = NULL;

static void rt_maskedcopy __P((struct sockaddr *,
	    struct sockaddr *, struct sockaddr *));
static void rtable_init __P((void **));

static void rt_timer_init __P((void));
static void rt_draincache __P((void));

static void
rtable_init(table)
	void **table;
{
	struct domain *dom;
	for (dom = domains; dom; dom = dom->dom_next)
		if (dom->dom_rtattach)
			dom->dom_rtattach(&table[dom->dom_family],
			    dom->dom_rtoffset);
}

void
route_init()
{
	rn_init();	/* initialize all zeroes, all ones, mask table */
	rtable_init((void **)rt_tables);

	rt_cache_max = ((M_RTABLE->ks_limit) >> 2) / sizeof(struct rtentry);
	rt_cache_hiwat = rt_cache_max - (rt_cache_max >> 3);
	rt_cache_lowat = rt_cache_max - (rt_cache_max >> 2);

	rt_cache_timeout_q = rt_timer_queue_create(rt_cachetimeout);
}

/*
 * Packet routing routines.
 */
void
rtalloc(ro)
	register struct route *ro;
{
	rtalloc_ign(ro, 0UL);
}

void
rtalloc_ign(ro, ignore)
	register struct route *ro;
	u_long ignore;
{
	struct rtentry *rt;
	int s;

	if ((rt = ro->ro_rt) != NULL) {
		if (rt->rt_ifp != NULL && rt->rt_flags & RTF_UP)
			return;
		/* XXX - We are probably always at splnet here already. */
		s = splnet();
		RTFREE(rt);
		ro->ro_rt = NULL;
		splx(s);
	}
	ro->ro_rt = rtalloc1(&ro->ro_dst, 1, ignore);
}

/*
 * Look up the route that matches the address given
 * Or, at least try.. Create a cloned route if needed.
 */
struct rtentry *
rtalloc1(dst, report, ignflags)
	register struct sockaddr *dst;
	int report;
	u_long ignflags;
{
	register struct radix_node_head *rnh = rt_tables[dst->sa_family];
	register struct rtentry *rt;
	register struct radix_node *rn;
	struct rtentry *newrt = 0;
	struct rt_addrinfo info;
	u_long nflags;
	int  s = splnet(), err = 0, msgtype = RTM_MISS;

	/*
	 * Look up the address in the table for that Address Family
	 */
	if (rnh && (rn = rnh->rnh_matchaddr((caddr_t)dst, rnh)) &&
	    ((rn->rn_flags & RNF_ROOT) == 0)) {
		/*
		 * If we find it and it's not the root node, then
		 * get a refernce on the rtentry associated.
		 */
		newrt = rt = (struct rtentry *)rn;
		nflags = rt->rt_flags & ~ignflags;
		if (report && (nflags & (RTF_CLONING | RTF_PRCLONING))) {
			/*
			 * We are apparently adding (report = 0 in delete).
			 * If it requires that it be cloned, do so.
			 * (This implies it wasn't a HOST route.)
			 */
			err = rtrequest(RTM_RESOLVE, dst, SA(0),
					      SA(0), RTF_CACHE, &newrt);
			if (err) {
				/*
				 * If the cloning didn't succeed, maybe
				 * what we have will do. Return that.
				 */
				newrt = rt;
				rt->rt_refcnt++;
				goto miss;
			}
			if ((rt = newrt) && (rt->rt_flags & RTF_XRESOLVE)) {
				/*
				 * If the new route specifies it be
				 * externally resolved, then go do that.
				 */
				msgtype = RTM_RESOLVE;
				goto miss;
			}
			/* Inform listeners of the new route. */
			bzero(&info, sizeof(info));
			info.rti_info[RTAX_DST] = rt_key(rt);
			info.rti_info[RTAX_NETMASK] = rt_mask(rt);
			info.rti_info[RTAX_GATEWAY] = rt->rt_gateway;
			if (rt->rt_ifp != NULL) {
				info.rti_info[RTAX_IFP] =
				    TAILQ_FIRST(&rt->rt_ifp->if_addrhead)->ifa_addr;
				info.rti_info[RTAX_IFA] = rt->rt_ifa->ifa_addr;
			}
			rt_missmsg(RTM_ADD, &info, rt->rt_flags, 0);
		} else {
#ifdef RTREUSE			/* for statistics */
			if (rt->rt_refcnt == 0)
				RTREUSE(rt);
#endif
			rt->rt_refcnt++;
		}
	} else {
		/*
		 * Either we hit the root or couldn't find any match,
		 * Which basically means
		 * "caint get there frm here"
		 */
		rtstat.rts_unreach++;
	miss:	if (report) {
			/*
			 * If required, report the failure to the supervising
			 * Authorities.
			 * For a delete, this is not an error. (report == 0)
			 */
			bzero((caddr_t)&info, sizeof(info));
			info.rti_info[RTAX_DST] = dst;
			rt_missmsg(msgtype, &info, 0, err);
		}
	}
	splx(s);
	return (newrt);
}

#ifdef SCTP_ALTERNATE_ROUTE 
struct rtentry * rtalloc_alternate __P((struct sockaddr *,struct rtentry *));
#endif


#ifdef SCTP_ALTERNATE_ROUTE
static struct rtentry *
rtfinalize_route(struct sockaddr *dst, struct rtentry *rt, int s)
{
	/*
	 * We handle cloning in this module (if needed). 
	 * This could probably be put inline but I don't want
	 * to clone it multiple times :>
	 */
	struct rt_addrinfo info;
	struct rtentry *newrt;
	int err;

	if (rt->rt_flags & (RTF_CLONING | RTF_PRCLONING)) {
		newrt = rt;
		bzero((caddr_t)&info, sizeof(info));
		err = rtrequest(RTM_RESOLVE, dst, SA(0), SA(0), 0, &newrt);
		if (err) {
			info.rti_info[RTAX_DST] = dst;
			rt_missmsg(RTM_MISS, &info, 0, err);
			rt->rt_refcnt++;
			splx(s);
			return(rt);
		} else {
			rt = newrt;
			if (rt->rt_flags & RTF_XRESOLVE) {
				info.rti_info[RTAX_DST] = dst;
				rt_missmsg(RTM_RESOLVE, &info, 0, err);
				splx(s);
				return(rt);
			}
			/* Inform listeners of the new route */
			info.rti_info[RTAX_DST] = rt_key(rt);
			info.rti_info[RTAX_NETMASK] = rt_mask(rt);
			info.rti_info[RTAX_GATEWAY] = rt->rt_gateway;
		}
	} else {
		/* No cloning needed */
		rt->rt_refcnt++;
	}
	splx(s);
	return(rt);
}

/*
 * Look up the route that matches the address given
 * Or, at least try.. Create a cloned route if needed.
 */
struct rtentry *
rtalloc_alternate(struct sockaddr *dst,
		  struct rtentry *existing)
{
	int i, cursalen, s;
	struct rtentry *tmp, *tmp2;
	struct radix_node *exist, *cmp, *curparent;
	struct sockaddr_storage s_store;
	struct sockaddr *sa;
	s = splnet();

	if (existing == NULL) {
		/* No existing route, we just to rtalloc1() */
		goto noexisting;
	}
	exist = &existing->rt_nodes[0];

	if (exist->rn_dupedkey == NULL) {
		/* No duplicated routes that we can access sorry :-< */
		goto nodups;
	}

	/*
	 * ok if we reach here there is a chance we can allocate
	 * an alternate route. Qualifications are:
	 *
	 * - We look for a route with a matching key but differnt IFP.
	 * - If we hit the end of the chain, we go ahead and rtalloc1()
	 *   and start at the top again in case there are some ahead
	 *   of existing.
	 * - The end of the chain is either a NULL or where the netmask
	 *   changes.
	 * - When we reach the end of the chain we have a choice we
	 *   can give up, or we can do a search for a higher level
	 *   route with a less specific key. So if some one put in
	 *   say a network route to 128.10.1.0 and we found no duplicate
	 *   to it we could look upwards with a less specific route
	 *   say to 128.10.0.0 or default by backing up the tree after
	 *   modifying the dst we are searching for.
	 *
	 */
	/* first lets look in the rn_dupedkey chain */
	cmp = exist->rn_dupedkey;
	while (cmp != NULL) {
		if (!rn_are_keys_same(exist, cmp)) {
			/* Keys are no longer the same */
			break;
		}
		if (existing->rt_ifp != ((struct rtentry *)cmp)->rt_ifp) {
			/* found a different one */
			return (rtfinalize_route(dst, 
						 (struct rtentry *)cmp, s));
		}
		cmp = cmp->rn_dupedkey;
	}
	/*
	 * ok if we reach here then from existing on out there were
	 * no dups that matched are qualifications. Lets rewind to
	 * the start of the list. End qualification is now we find
	 * existing.
	 */
	tmp = rtalloc1(dst, 0, (RTF_CLONING | RTF_PRCLONING));
	if (tmp == NULL) {
		goto noexisting;
	}
	tmp->rt_refcnt--;
	while (tmp != existing) {
		cmp = &tmp->rt_nodes[0];
		if (!rn_are_keys_same(exist, cmp)) {
			/* Keys are no longer the same */
			break;
		}
		if (existing->rt_ifp != tmp->rt_ifp) {
			/* found a different one */
			return(rtfinalize_route(dst, tmp, s));
		}
		tmp = (struct rtentry *)cmp->rn_dupedkey;
	}
 nodups:
	/*
	 * Now at this point we have two choices. We give up or
	 * move up the tree to see if a less specific route has
	 * a different outbound interface.
	 */

	memcpy(&s_store, dst, dst->sa_len);
	sa = (struct sockaddr *)&s_store;
	cursalen = sa->sa_len;
	curparent = existing->rt_nodes[1].rn_parent;
	while (curparent && ((curparent->rn_flags & RNF_ROOT) == 0)) {
		caddr_t tc;

		if (curparent->rn_bit < 0) {
			/* Not a internal node, up man up. */
			curparent = curparent->rn_parent;
			continue;
		}
		for (i = (curparent->rn_offset + 1), tc = (caddr_t)sa; 
		     i < cursalen; i++) {
			*tc++ = 0;
		}
		/* reset the length */
		cursalen = curparent->rn_offset + 1;
		/*
		 * now we must handle rn_offset by turning OFF the bit 
		 * of the last branch.
		 */
		/* Turn off all the bits below the current mask */
		tc = (caddr_t)sa + curparent->rn_offset;
		*tc &= ~(curparent->rn_bmask - 1);
		/* Turn off the bit in question */
		*tc &= ~curparent->rn_bmask - 1;
		tmp = rtalloc1(sa,0,(RTF_CLONING | RTF_PRCLONING));
		if (tmp == NULL) {
			goto noexisting;
		}
		tmp->rt_refcnt--;
		if (tmp == existing) {
			/* Got the same result, move up */
			curparent = curparent->rn_parent;
			continue;
		}
		if (existing->rt_ifp != tmp->rt_ifp) {
			/* found a different one */
			return(rtfinalize_route(dst, tmp, s));
		}
		/* now what about any dup's of tmp? */
		tmp2 = tmp;
		exist = &tmp2->rt_nodes[0];
		cmp = exist->rn_dupedkey;
		while (cmp) {
			if (!rn_are_keys_same(exist, cmp)) {
				/* Keys are no longer the same */
				break;
			}
			if (tmp2->rt_ifp != ((struct rtentry *)cmp)->rt_ifp) {
				/* found a different one */
				return(rtfinalize_route(dst,
							(struct rtentry *)cmp,
							s));
			}
			cmp = cmp->rn_dupedkey;
		}
		/* Ok we need to move up a level */
		curparent = curparent->rn_parent;
	}
	if (curparent && (curparent->rn_flags & RNF_ROOT)) {
		/*
		 * We climbed all the way up to the root, see
		 * if a default route exists.. if so go get it.
		 */
		caddr_t tc;
		if (curparent->rn_bit < 0) {
			goto noexisting;
		}
		for (i = (curparent->rn_offset + 1), tc = (caddr_t)sa;
		     i < cursalen; i++) {
			*tc++ = 0;
		}
		/* reset the length */
		cursalen = curparent->rn_offset + 1;
		/*
		 * now we must handle rn_offset by turning OFF the bit 
		 * of the last branch.
		 */
		/* Turn off all the bits below the current mask */
		tc = (caddr_t)sa + curparent->rn_offset;
		*tc &= ~(curparent->rn_bmask - 1);
		/* Turn off the bit in question */
		*tc &= ~curparent->rn_bmask - 1;
		tmp = rtalloc1(sa, 0, (RTF_CLONING | RTF_PRCLONING));
		if (tmp) {
			/* If tmp is null there is no default route */
			tmp->rt_refcnt--;
			if (tmp == existing) {
				goto noexisting;
			}
			if (existing->rt_ifp != tmp->rt_ifp) {
				/* found a different one */
				return(rtfinalize_route(dst, tmp, s));
			}
			/* now what about any dup's of tmp? */
			tmp2 = tmp;
			exist = &tmp2->rt_nodes[0];
			cmp = exist->rn_dupedkey;
			while (cmp != NULL) {
				if (!rn_are_keys_same(exist, cmp)) {
					/* Keys are no longer the same */
					break;
				}
				if (existing->rt_ifp !=
				    ((struct rtentry *)cmp)->rt_ifp) {
					/* found a different one */
					return(rtfinalize_route(dst, (struct rtentry *)cmp, s));
				}
				cmp = cmp->rn_dupedkey;
			}
		}
	}
 noexisting:
	tmp = rtalloc1(dst, 1, 0);
	splx(s);
	return(tmp);
}
#endif /* SCTP_ALTERNATE_ROUTE */

/*
 * Remove a reference count from an rtentry.
 * If the count gets low enough, take it out of the routing table
 */
void
rtfree(rt)
	register struct rtentry *rt;
{
	/*
	 * find the tree for that address family
	 */
	register struct radix_node_head *rnh =
		rt_tables[rt_key(rt)->sa_family];
	register struct ifaddr *ifa;

	if (rt == 0 || rnh == 0)
		panic("rtfree");

	/*
	 * decrement the reference count by one and if it reaches 0,
	 * and there is a close function defined, call the close function
	 */
	rt->rt_refcnt--;
#ifdef RTRELEASE
	if (rt->rt_refcnt == 0)
		RTRELEASE(rt);
#endif
	if(rnh->rnh_close && rt->rt_refcnt == 0) {
		rnh->rnh_close((struct radix_node *)rt, rnh);
	}

	/*
	 * If we are no longer "up" (and ref == 0)
	 * then we can free the resources associated
	 * with the route.
	 */
	if (rt->rt_refcnt <= 0 && (rt->rt_flags & RTF_UP) == 0) {
		if (rt->rt_nodes->rn_flags & (RNF_ACTIVE | RNF_ROOT))
			panic ("rtfree 2");
		/*
		 * the rtentry must have been removed from the routing table
		 * so it is represented in rttrash.. remove that now.
		 */
		rttrash--;

#ifdef	DIAGNOSTIC
		if (rt->rt_refcnt < 0) {
			printf("rtfree: %p not freed (neg refs)\n", rt);
			return;
		}
#endif

		rt_timer_remove_all(rt);

		/*
		 * release references on items we hold them on..
		 * e.g other routes and ifaddrs.
		 */
		if((ifa = rt->rt_ifa))
			IFAFREE(ifa);
		if (rt->rt_parent) {
			RTFREE(rt->rt_parent);
		}

		/*
		 * The key is separatly alloc'd so free it (see rt_setgate()).
		 * This also frees the gateway, as they are always malloc'd
		 * together.
		 */
		Free(rt_key(rt));

		/*
		 * and the rtentry itself of course
		 */
		Free(rt);
	}
}

void
ifafree(ifa)
	register struct ifaddr *ifa;
{
	if (ifa == NULL)
		panic("ifafree");
	if (ifa->ifa_refcnt == 0)
		free(ifa, M_IFADDR);
	else
		ifa->ifa_refcnt--;
}

/*
 * Force a routing table entry to the specified
 * destination to go through the given gateway.
 * Normally called as a result of a routing redirect
 * message from the network layer.
 *
 * N.B.: must be called at splnet
 *
 */
void
rtredirect(dst, gateway, netmask, flags, src, rtp)
	struct sockaddr *dst, *gateway, *netmask, *src;
	int flags;
	struct rtentry **rtp;
{
	struct rtentry *rt;
	int error = 0;
	short *stat = 0;
	struct rt_addrinfo info;
	struct ifaddr *ifa;

	/* verify the gateway is directly reachable */
	if ((ifa = ifa_ifwithnet(gateway)) == 0) {
		error = ENETUNREACH;
		goto out;
	}
	rt = rtalloc1(dst, 0, 0UL);
	/*
	 * If the redirect isn't from our current router for this dst,
	 * it's either old or wrong.  If it redirects us to ourselves,
	 * we have a routing loop, perhaps as a result of an interface
	 * going down recently.
	 */
#define	equal(a1, a2) \
	((a1)->sa_len == (a2)->sa_len && \
	 bcmp((caddr_t)(a1), (caddr_t)(a2), (a1)->sa_len) == 0)
	if (!(flags & RTF_DONE) && rt &&
	     (!equal(src, rt->rt_gateway) || rt->rt_ifa != ifa))
		error = EINVAL;
	else if (ifa_ifwithaddr(gateway))
		error = EHOSTUNREACH;
	if (error)
		goto done;
	/*
	 * Create a new entry if we just got back a wildcard entry
	 * or the the lookup failed.  This is necessary for hosts
	 * which use routing redirects generated by smart gateways
	 * to dynamically build the routing tables.
	 */
	if ((rt == 0) || (rt_mask(rt) && rt_mask(rt)->sa_len < 2))
		goto create;
	/*
	 * Don't listen to the redirect if it's
	 * for a route to an interface.
	 */
	if (rt->rt_flags & RTF_GATEWAY) {
		if (((rt->rt_flags & RTF_HOST) == 0) && (flags & RTF_HOST)) {
			/*
			 * Changing from route to net => route to host.
			 * Create new route, rather than smashing route to net.
			 */
		create:
			if (rt)
				rtfree(rt);
			flags |=  RTF_GATEWAY | RTF_DYNAMIC;
			bzero((caddr_t)&info, sizeof(info));
			info.rti_info[RTAX_DST] = dst;
			info.rti_info[RTAX_GATEWAY] = gateway;
			info.rti_info[RTAX_NETMASK] = netmask;
			info.rti_ifa = ifa;
			info.rti_flags = flags | RTF_CACHE;
			rt = NULL;
			error = rtrequest1(RTM_ADD, &info, &rt);
			if (rt != NULL)
				flags = rt->rt_flags;
			stat = &rtstat.rts_dynamic;
		} else {
			/*
			 * Smash the current notion of the gateway to
			 * this destination.  Should check about netmask!!!
			 */
			rt->rt_flags |= RTF_MODIFIED;
			flags |= RTF_MODIFIED;
			stat = &rtstat.rts_newgateway;
			/*
			 * add the key and gateway (in one malloc'd chunk).
			 */
			rt_setgate(rt, rt_key(rt), gateway);
		}
	} else
		error = EHOSTUNREACH;
done:
	if (rt) {
		if (rtp && !error)
			*rtp = rt;
		else
			rtfree(rt);
	}
out:
	if (error)
		rtstat.rts_badredirect++;
	else if (stat != NULL)
		(*stat)++;
	bzero((caddr_t)&info, sizeof(info));
	info.rti_info[RTAX_DST] = dst;
	info.rti_info[RTAX_GATEWAY] = gateway;
	info.rti_info[RTAX_NETMASK] = netmask;
	info.rti_info[RTAX_AUTHOR] = src;
	rt_missmsg(RTM_REDIRECT, &info, flags, error);
}

/*
* Routing table ioctl interface.
*/
int
rtioctl(req, data, p)
	u_long req;
	caddr_t data;
	struct proc *p;
{
#ifdef INET
	/* Multicast goop, grrr... */
#ifdef MROUTING
	return mrt_ioctl(req, data);
#else
	return mrt_ioctl(req, data, p);
#endif
#else /* INET */
	return ENXIO;
#endif /* INET */
}

struct ifaddr *
ifa_ifwithroute(flags, dst, gateway)
	int flags;
	struct sockaddr	*dst, *gateway;
{
	register struct ifaddr *ifa;
	if ((flags & RTF_GATEWAY) == 0) {
		/*
		 * If we are adding a route to an interface,
		 * and the interface is a pt to pt link
		 * we should search for the destination
		 * as our clue to the interface.  Otherwise
		 * we can use the local address.
		 */
		ifa = 0;
		if (flags & RTF_HOST) {
			ifa = ifa_ifwithdstaddr(dst);
		}
		if (ifa == 0)
			ifa = ifa_ifwithaddr(gateway);
	} else {
		/*
		 * If we are adding a route to a remote net
		 * or host, the gateway may still be on the
		 * other end of a pt to pt link.
		 */
		ifa = ifa_ifwithdstaddr(gateway);
	}
	if (ifa == 0)
		ifa = ifa_ifwithnet(gateway);
	if (ifa == 0) {
		struct rtentry *rt = rtalloc1(gateway, 0, 0UL);
		if (rt == 0)
			return (0);
		rt->rt_refcnt--;
		if ((ifa = rt->rt_ifa) == 0)
			return (0);
	}
	if (ifa->ifa_addr->sa_family != dst->sa_family) {
		struct ifaddr *oifa = ifa;
		ifa = ifaof_ifpforaddr(dst, ifa->ifa_ifp);
		if (ifa == 0)
			ifa = oifa;
	}
	return (ifa);
}

static int rt_fixdelete __P((struct radix_node *, void *));
static int rt_fixchange __P((struct radix_node *, void *));

struct rtfc_arg {
	struct rtentry *rt0;
	struct radix_node_head *rnh;
};

/*
 * Do appropriate manipulations of a routing tree given
 * all the bits of info needed
 */
int
rtrequest(req, dst, gateway, netmask, flags, ret_nrt)
	int req, flags;
	struct sockaddr *dst, *gateway, *netmask;
	struct rtentry **ret_nrt;
{
	struct rt_addrinfo info;

	bzero((caddr_t)&info, sizeof(info));
	info.rti_flags = flags;
	info.rti_info[RTAX_DST] = dst;
	info.rti_info[RTAX_GATEWAY] = gateway;
	info.rti_info[RTAX_NETMASK] = netmask;
	return rtrequest1(req, &info, ret_nrt);
}

/*
 * These (questionable) definitions of apparent local variables apply
 * to the next two functions.  XXXXXX!!!
 */
#define	dst	info->rti_info[RTAX_DST]
#define	gateway	info->rti_info[RTAX_GATEWAY]
#define	netmask	info->rti_info[RTAX_NETMASK]
#define	ifaaddr	info->rti_info[RTAX_IFA]
#define	ifpaddr	info->rti_info[RTAX_IFP]
#define	flags	info->rti_flags

int
rt_getifa(info)
	struct rt_addrinfo *info;
{
	struct ifaddr *ifa;
	int error = 0;

	/*
	 * ifp may be specified by sockaddr_dl
	 * when protocol address is ambiguous.
	 */
	if (info->rti_ifp == NULL && ifpaddr != NULL &&
	    ifpaddr->sa_family == AF_LINK &&
	    (ifa = ifa_ifwithnet(ifpaddr)) != NULL)
		info->rti_ifp = ifa->ifa_ifp;
	if (info->rti_ifa == NULL && ifaaddr != NULL)
		info->rti_ifa = ifa_ifwithaddr(ifaaddr);
	if (info->rti_ifa == NULL) {
		struct sockaddr *sa;

		sa = ifaaddr != NULL ? ifaaddr :
		    (gateway != NULL ? gateway : dst);
		if (sa != NULL && info->rti_ifp != NULL)
			info->rti_ifa = ifaof_ifpforaddr(sa, info->rti_ifp);
		else if (dst != NULL && gateway != NULL)
			info->rti_ifa = ifa_ifwithroute(flags, dst, gateway);
		else if (sa != NULL)
			info->rti_ifa = ifa_ifwithroute(flags, sa, sa);
	}
	if ((ifa = info->rti_ifa) != NULL) {
		if (info->rti_ifp == NULL)
			info->rti_ifp = ifa->ifa_ifp;
	} else
		error = ENETUNREACH;
	return (error);
}

int
rtrequest1(req, info, ret_nrt)
	int req;
	struct rt_addrinfo *info;
	struct rtentry **ret_nrt;
{
	int s = splnet(); int error = 0;
	int cache = 0;
	register struct rtentry *rt;
	register struct radix_node *rn;
	register struct radix_node_head *rnh;
	struct ifaddr *ifa;
	struct sockaddr *ndst;
#define senderr(x) { error = x ; goto bad; }

	/*
	 * Find the correct routing tree to use for this Address Family
	 */
	if ((rnh = rt_tables[dst->sa_family]) == 0)
		senderr(EAFNOSUPPORT);
	/*
	 * If we are adding a host route then we don't want to put
	 * a netmask in the tree, nor do we want to clone it.
	 */
	if (flags & RTF_HOST) {
		netmask = 0;
		flags &= ~(RTF_CLONING | RTF_PRCLONING);
	}
	if ((flags & (RTF_CACHE|RTF_STATIC)) == RTF_CACHE)
		cache = 1;
	switch (req) {
	case RTM_DELETE:
		/*
		 * Remove the item from the tree and return it.
		 * Complain if it is not there and do no more processing.
		 */
#ifdef SCTP_ALTERNATE_ROUTE
		if ((rn = rnh->rnh_deladdrdup(dst, netmask,
					      rnh, (void *)gateway)) == 0)
#else
		if ((rn = rnh->rnh_deladdr(dst, netmask, rnh)) == 0)
			senderr(ESRCH);
#endif

		if (rn->rn_flags & (RNF_ACTIVE | RNF_ROOT))
			panic ("rtrequest delete");
		rt = (struct rtentry *)rn;

		/*
		 * Now search what's left of the subtree for any cloned
		 * routes which might have been formed from this node.
		 */
		if ((rt->rt_flags & (RTF_CLONING | RTF_PRCLONING)) &&
		    rt_mask(rt)) {
			rnh->rnh_walktree_from(rnh, dst, rt_mask(rt),
					       rt_fixdelete, rt);
		}

		/*
		 * Remove any external references we may have.
		 * This might result in another rtentry being freed if
		 * we held its last reference.
		 */
		if (rt->rt_gwroute) {
			rt = rt->rt_gwroute;
			RTFREE(rt);
			(rt = (struct rtentry *)rn)->rt_gwroute = 0;
		}

		/*
		 * NB: RTF_UP must be set during the search above,
		 * because we might delete the last ref, causing
		 * rt to get freed prematurely.
		 *  eh? then why not just add a reference?
		 * I'm not sure how RTF_UP helps matters. (JRE)
		 */
		rt->rt_flags &= ~RTF_UP;

		/*
		 * give the protocol a chance to keep things in sync.
		 */
		if ((ifa = rt->rt_ifa) && ifa->ifa_rtrequest)
			ifa->ifa_rtrequest(RTM_DELETE, rt, info);

		/*
		 * one more rtentry floating around that is not
		 * linked to the routing table.
		 */
		rttrash++;

		/*
		 * If the caller wants it, then it can have it,
		 * but it's up to it to free the rtentry as we won't be
		 * doing it.
		 */
		if (ret_nrt)
			*ret_nrt = rt;
		else if (rt->rt_refcnt <= 0) {
			rt->rt_refcnt++; /* make a 1->0 transition */
			rtfree(rt);
		}
		break;

	case RTM_RESOLVE:
		if (ret_nrt == 0 || (rt = *ret_nrt) == 0)
			senderr(EINVAL);
		ifa = rt->rt_ifa;
		flags = rt->rt_flags &
		    ~(RTF_CLONING | RTF_PRCLONING | RTF_STATIC);
		flags |= RTF_WASCLONED;
		gateway = rt->rt_gateway;
		if ((netmask = rt->rt_genmask) == 0)
			flags |= RTF_HOST;
		goto makeroute;

	case RTM_ADD:
		if ((flags & RTF_GATEWAY) && !gateway)
			panic("rtrequest: GATEWAY but no gateway");

		if (info->rti_ifa == NULL && (error = rt_getifa(info)))
			senderr(error);
		ifa = info->rti_ifa;

	makeroute:
		if (cache && rt_cache_max) {
			unsigned long rtcount;

			rtcount = rt_timer_count(rt_cache_timeout_q);
			if (rtcount > rt_cache_max)
				senderr(ENOBUFS);
			if (rtcount > rt_cache_hiwat)
				rt_draincache();
		}
		R_Malloc(rt, struct rtentry *, sizeof(*rt));
		if (rt == 0)
			senderr(ENOBUFS);
		Bzero(rt, sizeof(*rt));
		rt->rt_createtime = time_second; /* for statistics */
		rt->rt_flags = RTF_UP | flags;
		LIST_INIT(&rt->rt_timer);
		/*
		 * Add the gateway. Possibly re-malloc-ing the storage for it
		 * also add the rt_gwroute if possible.
		 */
		if ((error = rt_setgate(rt, dst, gateway)) != 0) {
			Free(rt);
			senderr(error);
		}

		/*
		 * point to the (possibly newly malloc'd) dest address.
		 */
		ndst = rt_key(rt);

		/*
		 * make sure it contains the value we want (masked if needed).
		 */
		if (netmask) {
			rt_maskedcopy(dst, ndst, netmask);
		} else
			Bcopy(dst, ndst, dst->sa_len);

		/*
		 * Note that we now have a reference to the ifa.
		 * This moved from below so that rnh->rnh_addaddr() can
		 * examine the ifa and  ifa->ifa_ifp if it so desires.
		 */
		ifa->ifa_refcnt++;
		rt->rt_ifa = ifa;
		rt->rt_ifp = ifa->ifa_ifp;
		/* XXX mtu manipulation will be done in rnh_addaddr -- itojun */

#ifdef SCTP_ALTERNATE_ROUTE
		rn = rnh->rnh_addaddrdupok((caddr_t)ndst, (caddr_t)netmask,
					rnh, rt->rt_nodes);
#else
		rn = rnh->rnh_addaddr((caddr_t)ndst, (caddr_t)netmask,
					rnh, rt->rt_nodes);
#endif
		if (rn == 0) {
			struct rtentry *rt2;
			/*
			 * Uh-oh, we already have one of these in the tree.
			 * We do a special hack: if the route that's already
			 * there was generated by the protocol-cloning
			 * mechanism, then we just blow it away and retry
			 * the insertion of the new one.
			 */
			rt2 = rtalloc1(dst, 0, RTF_PRCLONING);
			if (rt2 && rt2->rt_parent) {
				rtrequest(RTM_DELETE,
					  (struct sockaddr *)rt_key(rt2),
					  rt2->rt_gateway,
					  rt_mask(rt2), rt2->rt_flags, 0);
				RTFREE(rt2);
#ifdef SCTP_ALTERNATE_ROUTE
				rn = rnh->rnh_addaddrdupok((caddr_t)ndst,
						      (caddr_t)netmask,
						      rnh, rt->rt_nodes);
#else
				rn = rnh->rnh_addaddr((caddr_t)ndst,
						      (caddr_t)netmask,
						      rnh, rt->rt_nodes);
#endif
			} else if (rt2) {
				/* undo the extra ref we got */
				RTFREE(rt2);
			}
		}

		/*
		 * If it still failed to go into the tree,
		 * then un-make it (this should be a function)
		 */
		if (rn == 0) {
			if (rt->rt_gwroute)
				rtfree(rt->rt_gwroute);
			if (rt->rt_ifa) {
				IFAFREE(rt->rt_ifa);
			}
			Free(rt_key(rt));
			Free(rt);
			senderr(EEXIST);
		}

		rt->rt_parent = 0;

		/*
		 * If we got here from RESOLVE, then we are cloning
		 * so clone the rest, and note that we
		 * are a clone (and increment the parent's references)
		 */
		if (req == RTM_RESOLVE) {
			rt->rt_rmx = (*ret_nrt)->rt_rmx; /* copy metrics */
			if ((*ret_nrt)->rt_flags & (RTF_CLONING | RTF_PRCLONING)) {
				rt->rt_parent = (*ret_nrt);
				(*ret_nrt)->rt_refcnt++;
			}
		}

		/*
		 * if this protocol has something to add to this then
		 * allow it to do that as well.
		 */
		if (ifa->ifa_rtrequest)
			ifa->ifa_rtrequest(req, rt, info);

		/*
		 * We repeat the same procedure from rt_setgate() here because
		 * it doesn't fire when we call it there because the node
		 * hasn't been added to the tree yet.
		 */
		if (!(rt->rt_flags & RTF_HOST) && rt_mask(rt) != 0) {
			struct rtfc_arg arg;
			arg.rnh = rnh;
			arg.rt0 = rt;
			rnh->rnh_walktree_from(rnh, rt_key(rt), rt_mask(rt),
					       rt_fixchange, &arg);
		}

		/*
		 * actually return a resultant rtentry and
		 * give the caller a single reference.
		 */
		if (ret_nrt) {
			*ret_nrt = rt;
			rt->rt_refcnt++;
		}

		if (cache)
			rt_add_cache(rt, NULL);
		break;
	default:
		error = EOPNOTSUPP;
	}
bad:
	splx(s);
	return (error);
#undef dst
#undef gateway
#undef netmask
#undef ifaaddr
#undef ifpaddr
#undef flags
}

/*
 * Called from rtrequest(RTM_DELETE, ...) to fix up the route's ``family''
 * (i.e., the routes related to it by the operation of cloning).  This
 * routine is iterated over all potential former-child-routes by way of
 * rnh->rnh_walktree_from() above, and those that actually are children of
 * the late parent (passed in as VP here) are themselves deleted.
 */
static int
rt_fixdelete(rn, vp)
	struct radix_node *rn;
	void *vp;
{
	struct rtentry *rt = (struct rtentry *)rn;
	struct rtentry *rt0 = vp;

	if (rt->rt_parent == rt0 && !(rt->rt_flags & RTF_PINNED)) {
		return rtrequest(RTM_DELETE, rt_key(rt),
				 (struct sockaddr *)0, rt_mask(rt),
				 rt->rt_flags, (struct rtentry **)0);
	}
	return 0;
}

/*
 * This routine is called from rt_setgate() to do the analogous thing for
 * adds and changes.  There is the added complication in this case of a
 * middle insert; i.e., insertion of a new network route between an older
 * network route and (cloned) host routes.  For this reason, a simple check
 * of rt->rt_parent is insufficient; each candidate route must be tested
 * against the (mask, value) of the new route (passed as before in vp)
 * to see if the new route matches it.
 *
 * XXX - it may be possible to do fixdelete() for changes and reserve this
 * routine just for adds.  I'm not sure why I thought it was necessary to do
 * changes this way.
 */
#ifdef DEBUG
static int rtfcdebug = 0;
#endif

static int
rt_fixchange(rn, vp)
	struct radix_node *rn;
	void *vp;
{
	struct rtentry *rt = (struct rtentry *)rn;
	struct rtfc_arg *ap = vp;
	struct rtentry *rt0 = ap->rt0;
	struct radix_node_head *rnh = ap->rnh;
	u_char *xk1, *xm1, *xk2, *xmp;
	int i, len, mlen;

#ifdef DEBUG
	if (rtfcdebug)
		printf("rt_fixchange: rt %p, rt0 %p\n", rt, rt0);
#endif

	if (!rt->rt_parent || (rt->rt_flags & RTF_PINNED)) {
#ifdef DEBUG
		if(rtfcdebug) printf("no parent or pinned\n");
#endif
		return 0;
	}

	if (rt->rt_parent == rt0) {
#ifdef DEBUG
		if(rtfcdebug) printf("parent match\n");
#endif
		return rtrequest(RTM_DELETE, rt_key(rt),
				 (struct sockaddr *)0, rt_mask(rt),
				 rt->rt_flags, (struct rtentry **)0);
	}

	/*
	 * There probably is a function somewhere which does this...
	 * if not, there should be.
	 */
	len = imin(((struct sockaddr *)rt_key(rt0))->sa_len,
		   ((struct sockaddr *)rt_key(rt))->sa_len);

	xk1 = (u_char *)rt_key(rt0);
	xm1 = (u_char *)rt_mask(rt0);
	xk2 = (u_char *)rt_key(rt);

	/* avoid applying a less specific route */
	xmp = (u_char *)rt_mask(rt->rt_parent);
	mlen = ((struct sockaddr *)rt_key(rt->rt_parent))->sa_len;
	if (mlen > ((struct sockaddr *)rt_key(rt0))->sa_len) {
#ifdef DEBUG
		if (rtfcdebug)
			printf("rt_fixchange: inserting a less "
			       "specific route\n");
#endif
		return 0;
	}
	for (i = rnh->rnh_treetop->rn_offset; i < mlen; i++) {
		if ((xmp[i] & ~(xmp[i] ^ xm1[i])) != xmp[i]) {
#ifdef DEBUG
			if (rtfcdebug)
				printf("rt_fixchange: inserting a less "
				       "specific route\n");
#endif
			return 0;
		}
	}

	for (i = rnh->rnh_treetop->rn_offset; i < len; i++) {
		if ((xk2[i] & xm1[i]) != xk1[i]) {
#ifdef DEBUG
			if(rtfcdebug) printf("no match\n");
#endif
			return 0;
		}
	}

	/*
	 * OK, this node is a clone, and matches the node currently being
	 * changed/added under the node's mask.  So, get rid of it.
	 */
#ifdef DEBUG
	if(rtfcdebug) printf("deleting\n");
#endif
	return rtrequest(RTM_DELETE, rt_key(rt), (struct sockaddr *)0,
			 rt_mask(rt), rt->rt_flags, (struct rtentry **)0);
}

#define ROUNDUP(a) (a>0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

int
rt_setgate(rt0, dst, gate)
	struct rtentry *rt0;
	struct sockaddr *dst, *gate;
{
	caddr_t new, old;
	int dlen = ROUNDUP(dst->sa_len), glen = ROUNDUP(gate->sa_len);
	register struct rtentry *rt = rt0;
	struct radix_node_head *rnh = rt_tables[dst->sa_family];

	/*
	 * A host route with the destination equal to the gateway
	 * will interfere with keeping LLINFO in the routing
	 * table, so disallow it.
	 */
	if (((rt0->rt_flags & (RTF_HOST|RTF_GATEWAY|RTF_LLINFO)) ==
					(RTF_HOST|RTF_GATEWAY)) &&
	    (dst->sa_len == gate->sa_len) &&
	    (bcmp(dst, gate, dst->sa_len) == 0)) {
		/*
		 * The route might already exist if this is an RTM_CHANGE
		 * or a routing redirect, so try to delete it.
		 */
		if (rt_key(rt0))
			rtrequest(RTM_DELETE, (struct sockaddr *)rt_key(rt0),
			    rt0->rt_gateway, rt_mask(rt0), rt0->rt_flags, 0);
		return EADDRNOTAVAIL;
	}

	/*
	 * Both dst and gateway are stored in the same malloc'd chunk
	 * (If I ever get my hands on....)
	 * if we need to malloc a new chunk, then keep the old one around
	 * till we don't need it any more.
	 */
	if (rt->rt_gateway == 0 || glen > ROUNDUP(rt->rt_gateway->sa_len)) {
		old = (caddr_t)rt_key(rt);
		R_Malloc(new, caddr_t, dlen + glen);
		if (new == 0)
			return ENOBUFS;
		rt->rt_nodes->rn_key = new;
	} else {
		/*
		 * otherwise just overwrite the old one
		 */
		new = rt->rt_nodes->rn_key;
		old = 0;
	}

	/*
	 * copy the new gateway value into the memory chunk
	 */
	Bcopy(gate, (rt->rt_gateway = (struct sockaddr *)(new + dlen)), glen);

	/*
	 * if we are replacing the chunk (or it's new) we need to
	 * replace the dst as well
	 */
	if (old) {
		Bcopy(dst, new, dlen);
		Free(old);
	}

	/*
	 * If there is already a gwroute, it's now almost definitly wrong
	 * so drop it.
	 */
	if (rt->rt_gwroute != NULL) {
		RTFREE(rt->rt_gwroute);
		rt->rt_gwroute = NULL;
	}
	/*
	 * Cloning loop avoidance:
	 * In the presence of protocol-cloning and bad configuration,
	 * it is possible to get stuck in bottomless mutual recursion
	 * (rtrequest rt_setgate rtalloc1).  We avoid this by not allowing
	 * protocol-cloning to operate for gateways (which is probably the
	 * correct choice anyway), and avoid the resulting reference loops
	 * by disallowing any route to run through itself as a gateway.
	 * This is obviously mandatory when we get rt->rt_output().
	 */
	if (rt->rt_flags & RTF_GATEWAY) {
		rt->rt_gwroute = rtalloc1(gate, 1, RTF_PRCLONING);
		if (rt->rt_gwroute == rt) {
			RTFREE(rt->rt_gwroute);
			rt->rt_gwroute = 0;
			return EDQUOT; /* failure */
		}
	}

	/*
	 * This isn't going to do anything useful for host routes, so
	 * don't bother.  Also make sure we have a reasonable mask
	 * (we don't yet have one during adds).
	 */
	if (!(rt->rt_flags & RTF_HOST) && rt_mask(rt) != 0) {
		struct rtfc_arg arg;
		arg.rnh = rnh;
		arg.rt0 = rt;
		rnh->rnh_walktree_from(rnh, rt_key(rt), rt_mask(rt),
				       rt_fixchange, &arg);
	}

	return 0;
}

static void
rt_maskedcopy(src, dst, netmask)
	struct sockaddr *src, *dst, *netmask;
{
	register u_char *cp1 = (u_char *)src;
	register u_char *cp2 = (u_char *)dst;
	register u_char *cp3 = (u_char *)netmask;
	u_char *cplim = cp2 + *cp3;
	u_char *cplim2 = cp2 + *cp1;

	*cp2++ = *cp1++; *cp2++ = *cp1++; /* copies sa_len & sa_family */
	cp3 += 2;
	if (cplim > cplim2)
		cplim = cplim2;
	while (cp2 < cplim)
		*cp2++ = *cp1++ & *cp3++;
	if (cp2 < cplim2)
		bzero((caddr_t)cp2, (unsigned)(cplim2 - cp2));
}

/*
 * Set up a routing table entry, normally
 * for an interface.
 */
int
rtinit(ifa, cmd, flags)
	register struct ifaddr *ifa;
	int cmd, flags;
{
	register struct rtentry *rt;
	register struct sockaddr *dst;
	register struct sockaddr *deldst;
	struct sockaddr *netmask;
	struct mbuf *m = 0;
	struct rtentry *nrt = 0;
	struct radix_node_head *rnh;
	struct radix_node *rn;
	int error;
	struct rt_addrinfo info;

	if (flags & RTF_HOST) {
		dst = ifa->ifa_dstaddr;
		netmask = NULL;
	} else {
		dst = ifa->ifa_addr;
		netmask = ifa->ifa_netmask;
	}
	/*
	 * If it's a delete, check that if it exists, it's on the correct
	 * interface or we might scrub a route to another ifa which would
	 * be confusing at best and possibly worse.
	 */
	if (cmd == RTM_DELETE) {
		/*
		 * It's a delete, so it should already exist..
		 * If it's a net, mask off the host bits
		 * (Assuming we have a mask)
		 */
		if (netmask != NULL) {
			m = m_get(M_DONTWAIT, MT_SONAME);
			if (m == NULL)
				return(ENOBUFS);
			deldst = mtod(m, struct sockaddr *);
			rt_maskedcopy(dst, deldst, netmask);
			dst = deldst;
		}
		/*
		 * Look up an rtentry that is in the routing tree and
		 * contains the correct info.
		 */
		if ((rnh = rt_tables[dst->sa_family]) == NULL ||
		    (rn = rnh->rnh_lookup(dst, netmask, rnh)) == NULL ||
		    (rn->rn_flags & RNF_ROOT) ||
		    ((struct rtentry *)rn)->rt_ifa != ifa ||
		    !equal(SA(rn->rn_key), dst)) {
			if (m)
				(void) m_free(m);
			return (flags & RTF_HOST ? EHOSTUNREACH : ENETUNREACH);
		}
		/* XXX */
#if 0
		else {
			/*
			 * One would think that as we are deleting, and we know
			 * it doesn't exist, we could just return at this point
			 * with an "ELSE" clause, but apparently not..
			 */
			return (flags & RTF_HOST ? EHOSTUNREACH : ENETUNREACH);
		}
#endif
	}
	/*
	 * Do the actual request
	 */
	bzero((caddr_t)&info, sizeof(info));
	info.rti_ifa = ifa;
	info.rti_flags = flags | ifa->ifa_flags;
	info.rti_info[RTAX_DST] = dst;
	info.rti_info[RTAX_GATEWAY] = ifa->ifa_addr;
	info.rti_info[RTAX_NETMASK] = netmask;
	error = rtrequest1(cmd, &info, &nrt);
	if (error == 0 && (rt = nrt) != NULL) {
		/*
		 * notify any listening routing agents of the change
		 */
		rt_newaddrmsg(cmd, ifa, error, rt);
		if (cmd == RTM_DELETE) {
			/*
			 * If we are deleting, and we found an entry, then
			 * it's been removed from the tree.. now throw it away.
			 */
			if (rt->rt_refcnt <= 0) {
				rt->rt_refcnt++; /* make a 1->0 transition */
				rtfree(rt);
			}
		} else if (cmd == RTM_ADD) {
			/*
			 * We just wanted to add it.. we don't actually
			 * need a reference.
			 */
			rt->rt_refcnt--;
		}
	}
	if (m)
		(void) m_free(m);
	return (error);
}

/*
 * Route timer routines.  These routes allow functions to be called
 * for various routes at any time.  This is useful in supporting
 * path MTU discovery and redirect route deletion.
 *
 * This is similar to some BSDI internal functions, but it provides
 * for multiple queues for efficiency's sake...
 */

LIST_HEAD(, rttimer_queue) rttimer_queue_head;
static int rt_init_done = 0;

#define RTTIMER_CALLOUT(r)	{				\
	if (r->rtt_func != NULL) {				\
		(*r->rtt_func)(r->rtt_rt, r);			\
	} else {						\
		rtrequest((int) RTM_DELETE,			\
			  (struct sockaddr *)rt_key(r->rtt_rt),	\
			  0, 0, 0, 0);				\
	}							\
}

/* 
 * Some subtle order problems with domain initialization mean that
 * we cannot count on this being run from rt_init before various
 * protocol initializations are done.  Therefore, we make sure
 * that this is run when the first queue is added...
 */

static void
rt_timer_init()
{
	if (rt_init_done)
		panic("rt_timer_init");

	LIST_INIT(&rttimer_queue_head);
	callout_init(&rt_timer_ch);
	callout_reset(&rt_timer_ch, hz, rt_timer_timer, NULL);
	rt_init_done = 1;
}

struct rttimer_queue *
rt_timer_queue_create(timeout)
	u_int	timeout;
{
	struct rttimer_queue *rtq;

	if (rt_init_done == 0)
		rt_timer_init();

	R_Malloc(rtq, struct rttimer_queue *, sizeof *rtq);
	if (rtq == NULL)
		return (NULL);		
	Bzero(rtq, sizeof *rtq);

	rtq->rtq_timeout = timeout;
	rtq->rtq_count = 0;
	TAILQ_INIT(&rtq->rtq_head);
	LIST_INSERT_HEAD(&rttimer_queue_head, rtq, rtq_link);

	return (rtq);
}

void
rt_timer_queue_change(rtq, timeout)
	struct rttimer_queue *rtq;
	long timeout;
{

	rtq->rtq_timeout = timeout;
}

void
rt_timer_queue_destroy(rtq, destroy)
	struct rttimer_queue *rtq;
	int destroy;
{
	struct rttimer *r;

	while ((r = TAILQ_FIRST(&rtq->rtq_head)) != NULL) {
		LIST_REMOVE(r, rtt_link);
		TAILQ_REMOVE(&rtq->rtq_head, r, rtt_next);
		if (destroy)
			RTTIMER_CALLOUT(r);
		Free(r);
		if (rtq->rtq_count > 0)
			rtq->rtq_count--;
		else
			printf("rt_timer_queue_destroy: rtq_count reached 0\n");
	}

	LIST_REMOVE(rtq, rtq_link);

	/*
	 * Caller is responsible for freeing the rttimer_queue structure.
	 */
}

unsigned long
rt_timer_count(rtq)
	struct rttimer_queue *rtq;
{

	return rtq->rtq_count;
}

void     
rt_timer_remove_all(rt)
	struct rtentry *rt;
{
	struct rttimer *r;

	while ((r = LIST_FIRST(&rt->rt_timer)) != NULL) {
		LIST_REMOVE(r, rtt_link);
		TAILQ_REMOVE(&r->rtt_queue->rtq_head, r, rtt_next);
		if (r->rtt_queue->rtq_count > 0)
			r->rtt_queue->rtq_count--;
		else
			printf("rt_timer_remove_all: rtq_count reached 0\n");
		Free(r);
	}
}

int      
rt_timer_add(rt, func, queue)
	struct rtentry *rt;
	void(*func) __P((struct rtentry *, struct rttimer *));
	struct rttimer_queue *queue;
{
	struct rttimer *r;
	long current_time = time_second;

	/*
	 * If there's already a timer with this action, destroy it before
	 * we add a new one.
	 */
	for (r = LIST_FIRST(&rt->rt_timer); r != NULL;
	     r = LIST_NEXT(r, rtt_link)) {
		if (r->rtt_func == func) {
			LIST_REMOVE(r, rtt_link);
			TAILQ_REMOVE(&r->rtt_queue->rtq_head, r, rtt_next);
			if (r->rtt_queue->rtq_count > 0)
				r->rtt_queue->rtq_count--;
			else
				printf("rt_timer_add: rtq_count reached 0\n");
			Free(r);
			break;  /* only one per list, so we can quit... */
		}
	}

	R_Malloc(r, struct rttimer *, sizeof(struct rttimer));
	if (r == NULL)
		return (ENOBUFS);
	Bzero(r, sizeof(*r));

	r->rtt_rt = rt;
	r->rtt_time = current_time;
	r->rtt_func = func;
	r->rtt_queue = queue;
	LIST_INSERT_HEAD(&rt->rt_timer, r, rtt_link);
	TAILQ_INSERT_TAIL(&queue->rtq_head, r, rtt_next);
	r->rtt_queue->rtq_count++;
	
	return (0);
}

/* ARGSUSED */
void
rt_timer_timer(arg)
	void *arg;
{
	struct rttimer_queue *rtq;
	struct rttimer *r;
	long current_time;
	int s;

	current_time = time_second;

	s = splnet();
	for (rtq = LIST_FIRST(&rttimer_queue_head); rtq != NULL; 
	     rtq = LIST_NEXT(rtq, rtq_link)) {
		while ((r = TAILQ_FIRST(&rtq->rtq_head)) != NULL &&
		    (r->rtt_time + rtq->rtq_timeout) < current_time) {
			LIST_REMOVE(r, rtt_link);
			TAILQ_REMOVE(&rtq->rtq_head, r, rtt_next);
			RTTIMER_CALLOUT(r);
			Free(r);
			if (rtq->rtq_count > 0)
				rtq->rtq_count--;
			else
				printf("rt_timer_timer: rtq_count reached 0\n");
		}
	}
	splx(s);

	callout_reset(&rt_timer_ch, hz, rt_timer_timer, NULL);
}

void
rt_add_cache(rt, func)
	struct rtentry *rt;
	void(*func) __P((struct rtentry *, struct rttimer *));
{
	struct rttimer *r;

	/*
	 * check if we already have a cache timer entry associated to the
	 * route.
	 */
	for (r = LIST_FIRST(&rt->rt_timer); r != NULL;
	     r = LIST_NEXT(r, rtt_link)) {
		if (r->rtt_queue == rt_cache_timeout_q)
			return;	/* we already have one.  do nothing. */
	}

	rt_timer_add(rt, func, rt_cache_timeout_q);

	/* TODO: adjust timeout based on the number of entries */
}

static void
rt_draincache()
{
	int s, again = 0;
	struct rttimer *r, *r_next;
	long rtcount;

	s = splnet();

#if 0				/* for debug */
	printf("rt_draincache: purge cache entries from %ld", rt_cache_timeout_q->rtq_count);
#endif

  again:
	rtcount = rt_cache_timeout_q->rtq_count;

	/* First, make entries that do not have references expire. */
	for (r = TAILQ_FIRST(&rt_cache_timeout_q->rtq_head); r; r = r_next) {
		r_next = TAILQ_NEXT(r, rtt_next);

		if (r->rtt_rt->rt_refcnt <= 0) {
			TAILQ_REMOVE(&rt_cache_timeout_q->rtq_head,
				     r, rtt_next);
			r->rtt_time = 0;
			TAILQ_INSERT_HEAD(&rt_cache_timeout_q->rtq_head, r,
					  rtt_next);
		}

		/*
		 * At the first attempt, we try to limit the number of entries
		 * being dropped so that entries won't shrink too much beyond
		 * the lower limit.
		 */
		if (!again && --rtcount < rt_cache_lowat)
			break;
	}

	/* then perform expiration.  XXX code borrowed from rt_timer_timer() */
	while ((r = TAILQ_FIRST(&rt_cache_timeout_q->rtq_head)) != NULL &&
	       (r->rtt_time == 0)) {
		LIST_REMOVE(r, rtt_link);
		TAILQ_REMOVE(&rt_cache_timeout_q->rtq_head, r, rtt_next);
		RTTIMER_CALLOUT(r);
		Free(r);
		if (rt_cache_timeout_q->rtq_count > 0)
			rt_cache_timeout_q->rtq_count--;
		else
			printf("rt_draincache: rtq_count reached 0\n");
	}

	/*
	 * if we cannot purge enough entries, do it again with a more
	 * aggressive policy.
	 */
	rtcount = rt_cache_timeout_q->rtq_count;
	if (!again && rtcount > rt_cache_hiwat) {
#if 0
		printf("...to %ld (not enough)...", rtcount);
#endif
		again = 1;
		goto again;
	}
#if 0
	printf(" to %ld\n", rtcount);
#endif

	splx(s);
}

/* This must be before ip6_init2(), which is now SI_ORDER_MIDDLE */
SYSINIT(route, SI_SUB_PROTO_DOMAIN, SI_ORDER_THIRD, route_init, 0);
