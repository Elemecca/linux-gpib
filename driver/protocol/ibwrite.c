/***************************************************************************
                              ibwrite.c
                             -------------------

    begin                : Dec 2001
    copyright            : (C) 2001, 2002 by Frank Mori Hess
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

#include <ibprot.h>

/*
 * IBWRT
 * Write cnt bytes of data from buf to the GPIB.  The write
 * operation terminates only on I/O complete. 
 *
 * NOTE:
 *      1.  Prior to beginning the write, the interface is
 *          placed in the controller standby state.
 *      2.  Prior to calling ibwrt, the intended devices as
 *          well as the interface board itself must be
 *          addressed by calling ibcmd.
 */
ssize_t ibwrt(gpib_board_t *board, uint8_t *buf, size_t cnt, int send_eoi)
{
	size_t bytes_sent = 0;
	ssize_t ret = 0;

	if(cnt == 0) return 0;

	board->interface->go_to_standby(board);
	// mark io in progress
	clear_bit(CMPL_NUM, &board->status);
	osStartTimer(board, timeidx);

	ret = board->interface->write(board, buf, cnt, send_eoi);
	if(ret < 0)
	{
		printk("gpib write error\n");
	}else
	{
		buf += ret;
		bytes_sent += ret;
	}

	if(ibstatus(board) & TIMO)
		ret = -ETIMEDOUT;

	osRemoveTimer(board);

	// mark io complete
	set_bit(CMPL_NUM, &board->status);

	if(ret < 0) return ret;
	
	return bytes_sent;
}
