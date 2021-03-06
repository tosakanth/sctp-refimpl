/*-
 * Copyright (C) 2002-2006 Cisco Systems Inc,
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* $KAME: sctp_peeloff.h,v 1.6 2005/03/06 16:04:18 itojun Exp $	 */

#ifdef __FreeBSD__
#include <sys/cdefs.h>
__FBSDID("$FreeBSD:$");
#endif

#ifndef __sctp_peeloff_h__
#define __sctp_peeloff_h__

#include <sys/types.h>
#if !defined(__OpenBSD__)
#include <sys/socketvar.h>
#endif
#include <sys/socket.h>

#if defined(HAVE_SCTP_PEELOFF_SOCKOPT)
/* socket option peeloff */
struct sctp_peeloff_opt {
	int s;
	sctp_assoc_t assoc_id;
	int new_sd;
};

#endif				/* HAVE_SCTP_PEELOFF_SOCKOPT */


#if (defined(__APPLE__) && defined(KERNEL))
#ifndef _KERNEL
#define _KERNEL
#endif
#endif

#if defined(_KERNEL)

int sctp_can_peel_off(struct socket *, sctp_assoc_t);
int sctp_do_peeloff(struct socket *, struct socket *, sctp_assoc_t);
struct socket *sctp_get_peeloff(struct socket *, sctp_assoc_t, int *);

#if defined(HAVE_SCTP_PEELOFF_SOCKOPT)
#include <sys/proc.h>
int sctp_peeloff_option(struct proc *p, struct sctp_peeloff_opt *peeloff);

#endif				/* HAVE_SCTP_PEELOFF_SOCKOPT */

#ifdef __APPLE__
/* sctp_peeloff() syscall arguments */
struct sctp_peeloff_args {
	int s;
	caddr_t name;
};

#endif				/* __APPLE__ */

#endif				/* _KERNEL */

#endif
