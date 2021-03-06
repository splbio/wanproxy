/*
 * Copyright (c) 2013 Patrick Kelsey. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/ctype.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <net/if_tap.h>
#include <net/if_dl.h>

#include <machine/atomic.h>

#include "uinet_config_internal.h"
#include "uinet_host_interface.h"
#include "uinet_if_netmap.h"
#include "uinet_if_netmap_host.h"


/*
 *  IF_NETMAP_RXRING_ZCOPY_FRAC_NUM and IF_NETMAP_RXRING_ZCOPY_FRAC_DEN are
 *  the numerator and denominator of the fraction of rxring buffers that
 *  will be available for zero-copy receive at any given time.  During
 *  receive processing, if that many buffers have been handed to the stack
 *  in a zero-copy fashion, all further received buffers will be passed to
 *  the stack using copies until some of the zero-copy buffers are
 *  returned.
 *
 *  One way to look at this is that the complement of this fraction
 *  represents the fraction of buffers that will be available for packet
 *  reception at the end of each pass through the receive processing loop,
 *  and thus represents the capacity to absorb traffic between receive
 *  processing passes.
 *
 *  So, for example, if IF_NETMAP_RXRING_ZCOPY_FRAC_NUM = 1 and
 *  IF_NETMAP_RXRING_ZCOPY_FRAC_DEN = 4, up to 1/4 of the buffers in each
 *  rxring will be oustanding to the stack via zero-copy at any given time,
 *  and there will always be at least 3/4 of the buffers in the ring
 *  available for new packet reception at the end of each receive loop pass.
 *
 *  Setting IF_NETMAP_RXRING_ZCOPY_FRAC_NUM to zero will disable zero copy
 *  receive.
 */
#define IF_NETMAP_RXRING_ZCOPY_FRAC_NUM 1
#define IF_NETMAP_RXRING_ZCOPY_FRAC_DEN 2


struct if_netmap_bufinfo {
	unsigned int refcnt;
	uint32_t nm_index;  /* netmap buffer index */
	uint32_t bi_index;  /* bufinfo index */
};

struct if_netmap_bufinfo_pool {
	unsigned int initialized;
	struct mtx tail_lock;
	struct if_netmap_bufinfo *pool;
	uint32_t *free_list;
	uint32_t max;
	uint32_t avail;
	volatile u_int returnable;
	uint32_t head;
	uint32_t tail;
	uint32_t trail;
};

struct if_netmap_softc {
	struct ifnet *ifp;
	const struct uinet_config_if *cfg;
	uint8_t addr[ETHER_ADDR_LEN];
	int isvale;
	int fd;
	char host_ifname[IF_NAMESIZE];
	uint16_t queue;

	uint32_t hw_rx_rsvd_begin;
	struct if_netmap_host_context *nm_host_ctx;

	struct if_netmap_bufinfo_pool rx_bufinfo;

	struct thread *tx_thread;
	struct thread *rx_thread;
	struct mtx tx_lock;
};


static int if_netmap_setup_interface(struct if_netmap_softc *sc);



static unsigned int interface_count;



static int
if_netmap_bufinfo_pool_init(struct if_netmap_bufinfo_pool *p, uint32_t max)
{
	uint32_t i;

	p->max = max;

	if (p->max > 0) {
		p->pool = malloc(sizeof(struct if_netmap_bufinfo) * p->max, M_DEVBUF, M_WAITOK);
		if (NULL == p->pool) {
			p->free_list = NULL;
			return (-1);
		}
		p->free_list = malloc(sizeof(uint32_t) * p->max, M_DEVBUF, M_WAITOK);
		if (NULL == p->free_list) {
			return (-1);
		}
	} else {
		p->pool = NULL;
		p->free_list = NULL;
	}
	p->avail = p->max;
	p->returnable = 0;
	p->head = 0;
	p->tail = 0;
	p->trail = 0;

	for (i = 0; i < p->max; i++) {
		p->pool[i].bi_index = i;
		p->free_list[i] = i;
	}

	mtx_init(&p->tail_lock, "bitllk", NULL, MTX_DEF);

	p->initialized = 1;

	return (0);
}


static int
if_netmap_bufinfo_pool_destroy(struct if_netmap_bufinfo_pool *p)
{
	mtx_destroy(&p->tail_lock);

	if (p->free_list) {
		free(p->free_list, M_DEVBUF);
	}

	if (p->pool) {
		free(p->pool, M_DEVBUF);
	}

	return (0);
}


/* Only called from the receive thread */
static struct if_netmap_bufinfo *
if_netmap_bufinfo_alloc(struct if_netmap_bufinfo_pool *p)
{
	struct if_netmap_bufinfo *bi;

	if (p->avail) {
		p->avail--;
		bi = &p->pool[p->free_list[p->head]];

		p->head++;
		if (p->head == p->max) {
			p->head = 0;
		}

		return (bi);
	}

	return (NULL);
}


/*
 * Undo an allocation of a bufinfo that was just allocated.  Only called
 * from the receive thread.
 */
static void
if_netmap_bufinfo_unalloc(struct if_netmap_bufinfo_pool *p)
{
	p->avail++;
	if (p->head > 0) {
		p->head--;
	} else {
		p->head = p->max - 1;
	}
}

/* This may be called from arbitrary threads */
static void
if_netmap_bufinfo_free(struct if_netmap_bufinfo_pool *p, struct if_netmap_bufinfo *bi)
{
	mtx_lock(&p->tail_lock);
	p->free_list[p->tail] = bi->bi_index;

	/*
	 * p->returnable is the only state shared with
	 * if_netmap_sweep_trail, and using atomic add here saves us from
	 * locking there.
	 */
	atomic_add_int(&p->returnable, 1);

	p->tail++;
	if (p->tail == p->max) {
		p->tail = 0;
	}
	mtx_unlock(&p->tail_lock);
}


static int
if_netmap_process_configstr(struct if_netmap_softc *sc)
{
	char *configstr = sc->cfg->configstr;
	int error = 0;
	char *last_colon;
	char *p;
	int namelen;

	if (0 == strncmp(configstr, "vale", 4)) {
		sc->isvale = 1;
		sc->queue = 0;

		if (strlen(configstr) > (sizeof(sc->host_ifname) - 1)) {
			error = ENAMETOOLONG;
			goto out;
		}
		strcpy(sc->host_ifname, configstr);
	} else {
		sc->isvale = 0;

		last_colon = strchr(configstr, ':');
		if (last_colon) {
			if (last_colon == configstr) {
				/* no name */
				error = EINVAL;
				goto out;
			}

			p = last_colon + 1;
			if ('\0' == *p) {
				/* colon at the end */
				error = EINVAL;
				goto out;
			}

			while (isdigit(*p) && ('\0' != *p))
				p++;
			
			if ('\0' != *p) {
				/* non-numeric chars after colon */
				error = EINVAL;
				goto out;
			}

			sc->queue = strtoul(last_colon + 1, NULL, 10);
			
			namelen = last_colon - configstr;
			if (namelen > (sizeof(sc->host_ifname) - 1)) {
				error = ENAMETOOLONG;
				goto out;
			}
			
			memcpy(sc->host_ifname, configstr, namelen);
			sc->host_ifname[namelen] = '\0';
		} else {
			sc->queue = 0;
			strlcpy(sc->host_ifname, configstr, sizeof(sc->host_ifname));
		}
	}

out:
	return (error);
}


int
if_netmap_attach(struct uinet_config_if *cfg)
{
	struct if_netmap_softc *sc = NULL;
	int fd = -1;
	int error = 0;
	uint32_t pool_size;

	
	if (NULL == cfg->configstr) {
		error = EINVAL;
		goto fail;
	}

	printf("configstr is %s\n", cfg->configstr);

	snprintf(cfg->name, sizeof(cfg->name), "netmap%u", interface_count);
	interface_count++;

	sc = malloc(sizeof(struct if_netmap_softc), M_DEVBUF, M_WAITOK);
	if (NULL == sc) {
		printf("if_netmap_softc allocation failed\n");
		error = ENOMEM;
		goto fail;
	}
	memset(sc, 0, sizeof(struct if_netmap_softc));

	sc->cfg = cfg;

	error = if_netmap_process_configstr(sc);
	if (0 != error) {
		goto fail;
	}

	fd = uhi_open("/dev/netmap", UHI_O_RDWR);
	if (fd < 0) {
		printf("/dev/netmap open failed\n");
		error = ENXIO;
		goto fail;
	}

	sc->fd = fd;


	sc->nm_host_ctx = if_netmap_register_if(sc->fd, sc->host_ifname, sc->isvale, sc->queue);
	if (NULL == sc->nm_host_ctx) {
		printf("Failed to register netmap interface\n");
		error = ENXIO;
		goto fail;
	}

	/*
	 * Limiting the size of the rxring zero-copy context pool to the
	 * given fraction of the rxring size limits the amount of rxring
	 * buffers that can be outstanding to the stack via zero-copy at any
	 * given time as a failure to allocate a zero-copy context in the
	 * receive loop causes the buffer to be copied to the stack.
	 */
	pool_size = (if_netmap_rxslots(sc->nm_host_ctx) * IF_NETMAP_RXRING_ZCOPY_FRAC_NUM) / IF_NETMAP_RXRING_ZCOPY_FRAC_DEN;
	error = if_netmap_bufinfo_pool_init(&sc->rx_bufinfo, pool_size);
	if (error != 0) {
		printf("bufinfo pool init failed\n");
		goto fail;
	}

	if (!sc->isvale) {
		if (0 != if_netmap_get_ifaddr(sc->host_ifname, sc->addr)) {
			printf("failed to find interface address\n");
			error = ENXIO;
			goto fail;
		}
	}

	if (0 != if_netmap_setup_interface(sc)) {
		error = ENXIO;
		goto fail;
	}

	cfg->ifdata = sc;

	return (0);

fail:
	if (sc) {
		if (sc->nm_host_ctx)
			if_netmap_deregister_if(sc->nm_host_ctx);

		if (sc->rx_bufinfo.initialized)
			if_netmap_bufinfo_pool_destroy(&sc->rx_bufinfo);

		if (sc->fd >= 0)
			uhi_close(sc->fd);

		free(sc, M_DEVBUF);
	}

	return (error);
}


static void
if_netmap_init(void *arg)
{
	struct if_netmap_softc *sc = arg;
	struct ifnet *ifp = sc->ifp;

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
}


static void
if_netmap_start(struct ifnet *ifp)
{
	struct if_netmap_softc *sc = ifp->if_softc;

	mtx_lock(&sc->tx_lock);
	ifp->if_drv_flags |= IFF_DRV_OACTIVE;
	wakeup(&ifp->if_drv_flags);
	mtx_unlock(&sc->tx_lock);
}


static void
if_netmap_send(void *arg)
{
	struct mbuf *m;
	struct if_netmap_softc *sc = (struct if_netmap_softc *)arg;
	struct ifnet *ifp = sc->ifp;
	struct uhi_pollfd pfd;
	uint32_t avail;
	uint32_t cur;
	u_int pktlen;
	int rv;


	while (1) {
		mtx_lock(&sc->tx_lock);
		while (IFQ_DRV_IS_EMPTY(&ifp->if_snd)) {
			ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
			mtx_sleep(&ifp->if_drv_flags, &sc->tx_lock, 0, "wtxlk", 0);
		}
		mtx_unlock(&sc->tx_lock);
	
		rv = if_netmap_txsync(sc->nm_host_ctx, NULL, NULL);
		if (rv == -1) {
			printf("could not sync tx descriptors before transmit\n");
		}
	
		while (!IFQ_DRV_IS_EMPTY(&ifp->if_snd)) {
			avail = if_netmap_txavail(sc->nm_host_ctx);

			while (0 == avail) {
				memset(&pfd, 0, sizeof(pfd));

				pfd.fd = sc->fd;
				pfd.events = UHI_POLLOUT;
				
				rv = uhi_poll(&pfd, 1, -1);
				if (rv == -1)
					printf("error from poll for transmit\n");
					
				avail = if_netmap_txavail(sc->nm_host_ctx);
			}

			cur = if_netmap_txcur(sc->nm_host_ctx);

			while (avail) {
				IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
				avail--;

				pktlen = m_length(m, NULL);

				m_copydata(m, 0, pktlen,
					   if_netmap_txslot(sc->nm_host_ctx, &cur, pktlen)); 
				m_freem(m);

				if (IFQ_DRV_IS_EMPTY(&ifp->if_snd)) {
					break;
				}
			}

			rv = if_netmap_txsync(sc->nm_host_ctx, &avail, &cur);
			if (rv == -1) {
				printf("could not sync tx descriptors after transmit\n");
			}
		}
	}
	
}


static void
if_netmap_stop(struct if_netmap_softc *sc)
{
	struct ifnet *ifp = sc->ifp;

	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING|IFF_DRV_OACTIVE);
}


static int
if_netmap_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int error = 0;
	struct if_netmap_softc *sc = ifp->if_softc;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
			if_netmap_init(sc);
		else if (ifp->if_drv_flags & IFF_DRV_RUNNING)
			if_netmap_stop(sc);
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}


static void
if_netmap_free(void *arg1, void *arg2)
{
	struct if_netmap_softc *sc;
	struct if_netmap_bufinfo *bi;

	sc = (struct if_netmap_softc *)arg1;
	bi = (struct if_netmap_bufinfo *)arg2;

	if_netmap_bufinfo_free(&sc->rx_bufinfo, bi);
}

/* Only called from the receive thread */
static uint32_t
if_netmap_sweep_trail(struct if_netmap_softc *sc)
{
	struct if_netmap_bufinfo_pool *p;
	uint32_t i;
	uint32_t returned;
	unsigned int n;

	i = sc->hw_rx_rsvd_begin;
	
	p = &sc->rx_bufinfo;

	returned = p->returnable;
	for (n = 0; n < returned; n++) {
		if_netmap_rxsetslot(sc->nm_host_ctx, &i, p->pool[p->free_list[p->trail]].nm_index);

		p->trail++;
		if (p->trail == p->max) {
			p->trail = 0;
		}
	}
	sc->hw_rx_rsvd_begin = i;

	atomic_subtract_int(&p->returnable, returned);
	p->avail += returned;

	return (returned);
}


static void
if_netmap_receive(void *arg)
{
	struct if_netmap_softc *sc;
	struct uhi_pollfd pfd;
	struct mbuf *m;
	struct if_netmap_bufinfo *bi;
	void *slotbuf;
	uint32_t slotindex;
	uint32_t pktlen;
	uint32_t cur;
	uint32_t avail;
	uint32_t reserved;
	uint32_t returned;
	uint32_t new_reserved;
	unsigned int n;
	int rv;


	/* Zero-copy receive
	 *
	 * A packet header mbuf is allocated for each received netmap
	 * buffer, and the netmap buffer is attached to this mbuf as
	 * external storage, along with a free routine and piece of context
	 * that enables the free routine to move the netmap buffer on its
	 * way back to the receive ring.  The per-buffer context objects
	 * (struct if_netmap_bufinfo) are managed by this driver.
	 *
	 * When the mbuf layer calls the free routine for an mbuf-attached
	 * netmap buffer, its associated context object is added to a list
	 * that is part of the pool of those objects.  On each pass through
	 * the receive loop below, all of the context objects that have been
	 * returned to the list since the last pass are processed, and their
	 * associated netmap buffers are returned to the receive ring.
	 *
	 * With this approach, a given netmap buffer may be available for
	 * netmap's use on the ring, may be newly available for our
	 * consumption on the ring, may have been passed to the stack for
	 * processing and not yet returned, or may have been returned to us
	 * from the stack but not yet returned to the netmap ring.
	 */

	sc = (struct if_netmap_softc *)arg;

	rv = if_netmap_rxsync(sc->nm_host_ctx, NULL, NULL, NULL);
	if (rv == -1)
		printf("could not sync rx descriptors before receive loop\n");

	reserved = if_netmap_rxreserved(sc->nm_host_ctx);
	sc->hw_rx_rsvd_begin = if_netmap_rxcur(sc->nm_host_ctx);

	for (;;) {
		while (0 == (avail = if_netmap_rxavail(sc->nm_host_ctx))) {
			memset(&pfd, 0, sizeof pfd);

			pfd.fd = sc->fd;
			pfd.events = UHI_POLLIN;

			rv = uhi_poll(&pfd, 1, -1);
			if (rv == -1)
				printf("error from poll for receive\n");
		}

		cur = if_netmap_rxcur(sc->nm_host_ctx);
		new_reserved = 0;
		for (n = 0; n < avail; n++) {
			slotbuf = if_netmap_rxslot(sc->nm_host_ctx, &cur, &pktlen, &slotindex);

			bi = if_netmap_bufinfo_alloc(&sc->rx_bufinfo);
			if (NULL == bi) {
				/* copy receive */

				/* could streamline this a little since we
				 * know the data is going to fit in a
				 * cluster
				 */
				m = m_devget(slotbuf, pktlen, 0, sc->ifp, NULL);

				if (NULL == m) {
					/* XXX dropped. should count this */
					printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>NO MBUFS (1)\n");
				}

				/* Recover this buffer at the far end of the
				 * reserved trail from prior zero-copy
				 * activity.
				 */
				if_netmap_rxsetslot(sc->nm_host_ctx, &sc->hw_rx_rsvd_begin, slotindex);
			} else {
				/* zero-copy receive */

				m = m_gethdr(M_DONTWAIT, MT_DATA);
				if (NULL == m) {
					if_netmap_bufinfo_unalloc(&sc->rx_bufinfo);
					printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>NO MBUFS (2)\n");
					/* XXX dropped. should count this */

					if_netmap_rxsetslot(sc->nm_host_ctx, &sc->hw_rx_rsvd_begin, slotindex);
				} else {
					bi->nm_index = slotindex;
					
					m->m_pkthdr.len = m->m_len = pktlen;
					m->m_pkthdr.rcvif = sc->ifp;
					m->m_ext.ref_cnt = &bi->refcnt;
					m_extadd(m, slotbuf, if_netmap_rxbufsize(sc->nm_host_ctx),
						 if_netmap_free, sc, bi, 0, EXT_EXTREF);

					new_reserved++;
				}

			}

			if (m) {
				sc->ifp->if_input(sc->ifp, m);
			}
		}

		avail -= n;
		reserved += new_reserved;

		/* Return any netmap buffers freed by the stack to the ring */
		returned = if_netmap_sweep_trail(sc);
		reserved -= returned;

		rv = if_netmap_rxsync(sc->nm_host_ctx, &avail, &cur, &reserved);
		if (rv == -1)
			printf("could not sync rx descriptors after receive\n");

	}
}


static int
if_netmap_setup_interface(struct if_netmap_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->ifp = if_alloc(IFT_ETHER);

	ifp->if_init =  if_netmap_init;
	ifp->if_softc = sc;

	if_initname(ifp, sc->cfg->name, IF_DUNIT_NONE);
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = if_netmap_ioctl;
	ifp->if_start = if_netmap_start;

	/* XXX what values? */
	IFQ_SET_MAXLEN(&ifp->if_snd, if_netmap_txslots(sc->nm_host_ctx));
	ifp->if_snd.ifq_drv_maxlen = if_netmap_txslots(sc->nm_host_ctx);

	IFQ_SET_READY(&ifp->if_snd);

	ifp->if_fib = sc->cfg->cdom;

	ether_ifattach(ifp, sc->addr);
	ifp->if_capabilities = ifp->if_capenable = 0;


	mtx_init(&sc->tx_lock, "txlk", NULL, MTX_DEF);

	if (kthread_add(if_netmap_send, sc, NULL, &sc->tx_thread, 0, 0, "nm_tx: %s", ifp->if_xname)) {
		printf("Could not start transmit thread for %s (%s)\n", ifp->if_xname, sc->host_ifname);
		ether_ifdetach(ifp);
		if_free(ifp);
		return (1);
	}


	if (kthread_add(if_netmap_receive, sc, NULL, &sc->rx_thread, 0, 0, "nm_rx: %s", ifp->if_xname)) {
		printf("Could not start receive thread for %s (%s)\n", ifp->if_xname, sc->host_ifname);
		ether_ifdetach(ifp);
		if_free(ifp);
		return (1);
	}

	if (sc->cfg->cpu >= 0) {
		sched_bind(sc->tx_thread, sc->cfg->cpu);
		sched_bind(sc->rx_thread, sc->cfg->cpu);
	}

	return (0);
}


int
if_netmap_detach(struct uinet_config_if *cfg)
{
	struct if_netmap_softc *sc = cfg->ifdata;

	if (sc) {
		/* XXX ether_ifdetach, stop threads */

		if_netmap_deregister_if(sc->nm_host_ctx);
		
		if_netmap_bufinfo_pool_destroy(&sc->rx_bufinfo);

		uhi_close(sc->fd);

		free(sc, M_DEVBUF);
	}

	return (0);
}


