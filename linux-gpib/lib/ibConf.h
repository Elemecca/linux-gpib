/***************************************************************************
                              lib/ibConf.h
                             -------------------

    copyright            : (C) 2001,2002 by Frank Mori Hess
    email                : fmhess@users.sourceforge.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef _IBCONF_H
#define _IBCONF_H

#include <unistd.h>
#include <sys/types.h>

/* meaning for flags */

#define CN_SDCL    (1<<1)             /* Send DCL on init                */
#define CN_SLLO    (1<<2)             /* Send LLO on init                */
#define CN_NETWORK (1<<3)             /* is a network device             */
#define CN_AUTOPOLL (1<<4)            /* Auto serial poll devices        */
#define CN_EXCLUSIVE (1<<5)           /* Exclusive use only */

/*---------------------------------------------------------------------- */

struct async_operation
{
	pthread_t thread;	// thread used for asynchronous io operations
	pthread_mutex_t lock;
	uint8_t *buffer;
	volatile long length;
	int error;
	volatile int in_progress;
};


typedef struct ibConfStruct
{
	char name[100];		/* name of the device (for ibfind())     */
	int pad;		// device primary address
	int sad;		// device secodnary address (negative disables)
	char init_string[100];               /* initialization string (optional) */
	int board;                         /* board number                     */
	char eos;                           /* local eos modes                  */
	int eos_flags;
	int flags;                         /* some flags, deprecated          */
	unsigned int usec_timeout;
	unsigned int spoll_usec_timeout;
	unsigned int ppoll_usec_timeout;
	struct async_operation async;	// used by asynchronous operations ibcmda(), ibrda(), etc.
	int ppoll_config;	// current parallel poll configuration
	unsigned send_eoi : 1;	// assert EOI at end of writes
	unsigned is_interface : 1;	// is interface board
	unsigned dev_is_open : 1;
	unsigned board_is_open : 1;
	unsigned has_lock : 1;
	unsigned end : 1;	// EOI asserted or EOS received at end of IO operation */
	unsigned local_lockout : 1;	// send local lockout when device is brought online
	unsigned local_ppc : 1;	// enable local configuration of board's parallel poll response */
	unsigned timed_out : 1;		/* io operation timed out */
	unsigned readdr : 1;	/* useless, exists for compatibility only at present */
} ibConf_t;

/*---------------------------------------------------------------------- */

typedef struct ibBoardStruct {
	char board_type[100];	// name (model) of interface board
	int pad;		// device primary address
	int sad;		// device secodnary address (negative disables)
	unsigned long base;                          /* base configuration               */
	unsigned int irq;
	unsigned int dma;
	int fileno;                        /* device file descriptor           */
	char device[100];	// name of device file ( /dev/gpib0, etc.)
	pthread_t *autopoll_thread;
	unsigned int open_count;	// reference count
	unsigned is_system_controller : 1;	/* board is busmaster or not */
} ibBoard_t;

#endif	/* _IBCONF_H */
















