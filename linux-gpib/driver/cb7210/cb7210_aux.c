/***************************************************************************
                             nec7210/cb7210_aux.c
                             -------------------

    begin                : Nov 2002
    copyright            : (C) 2002 by Frank Mori Hess
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

#include "cb7210.h"
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

int cb7210_line_status( const gpib_board_t *board )
{
	int status = ValidALL;
	int bsr_bits;
	cb7210_private_t *cb_priv;
	nec7210_private_t *nec_priv;
	unsigned long flags;

	cb_priv = board->private_data;
	nec_priv = &cb_priv->nec7210_priv;

	spin_lock_irqsave( &board->spinlock, flags );
	write_byte( nec_priv, page_in_bits( BUS_STATUS_PAGE ), AUXMR);
	bsr_bits = inb( cb_priv->nec7210_priv.iobase + BUS_STATUS );
	spin_unlock_irqrestore( &board->spinlock, flags );

	if( bsr_bits & BSR_REN_BIT )
		status |= BusREN;
	if( bsr_bits & BSR_IFC_BIT )
		status |= BusIFC;
	if( bsr_bits & BSR_SRQ_BIT )
		status |= BusSRQ;
	if( bsr_bits & BSR_EOI_BIT )
		status |= BusEOI;
	if( bsr_bits & BSR_NRFD_BIT )
		status |= BusNRFD;
	if( bsr_bits & BSR_NDAC_BIT )
		status |= BusNDAC;
	if( bsr_bits & BSR_DAV_BIT )
		status |= BusDAV;
	if( bsr_bits & BSR_ATN_BIT )
		status |= BusATN;

	return status;
}
