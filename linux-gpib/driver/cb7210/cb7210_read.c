/***************************************************************************
                             cb7210_read.c
                             -------------------

    copyright            : (C) 2003 by Frank Mori Hess
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
#include <linux/delay.h>

static const int num_finishing_bytes = 2;

static inline int have_fifo_word( const cb7210_private_t *cb_priv )
{
	const nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;

	if( ( ( inb( nec_priv->iobase + HS_STATUS ) ) &
			( HS_RX_MSB_NOT_EMPTY | HS_RX_LSB_NOT_EMPTY ) ) ==
			( HS_RX_MSB_NOT_EMPTY | HS_RX_LSB_NOT_EMPTY ) )
		return 1;
	else
		return 0;
}

static inline void input_fifo_enable( gpib_board_t *board, int enable )
{
	cb7210_private_t *cb_priv = board->private_data;
	nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;
	unsigned long flags;

	spin_lock_irqsave( &board->spinlock, flags );

	if( enable )
	{
		outb( HS_RX_ENABLE | HS_TX_ENABLE | HS_CLR_SRQ_INT |
			HS_CLR_EOI_EMPTY_INT | HS_CLR_HF_INT, nec_priv->iobase + HS_MODE );
		cb_priv->in_fifo_half_full = 0;

		outb( cb_priv->hs_mode_bits & HS_SYS_CONTROL, nec_priv->iobase + HS_MODE );

outb( irq_bits( cb_priv->irq ), nec_priv->iobase + HS_INT_LEVEL );

		cb_priv->hs_mode_bits &= ~HS_ENABLE_MASK;
		cb_priv->hs_mode_bits |= HS_RX_ENABLE;
		outb( cb_priv->hs_mode_bits, nec_priv->iobase + HS_MODE );

		clear_bit( READ_READY_BN, &nec_priv->state );
		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 1 );
	}else
	{
		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 0 );

		cb_priv->hs_mode_bits &= ~HS_ENABLE_MASK;
		outb( cb_priv->hs_mode_bits, nec_priv->iobase + HS_MODE );

		cb7210_internal_interrupt( board );

		if( ( cb7210_line_status( board ) & BusNRFD ) == 0 )
		{
printk("fixing read ready\n");
			clear_bit( READ_READY_BN, &nec_priv->state );
		}
	}

	spin_unlock_irqrestore( &board->spinlock, flags );
}

static ssize_t fifo_read( gpib_board_t *board, cb7210_private_t *cb_priv, uint8_t *buffer,
	size_t length, int *end )
{
	size_t count = 0;
	ssize_t retval = 0;
	nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;
	unsigned long iobase = nec_priv->iobase;
	int hs_status;
	uint16_t word;
	unsigned long flags;
	int swap_bytes = 0;

	*end = 0;

	if( length <= cb7210_fifo_size )
	{
		printk("cb7210: bug! fifo_read() with length < fifo size\n" );
		return -EINVAL;
	}

	while( count + cb7210_fifo_size <= length )
	{
		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 1 );

		if( wait_event_interruptible( board->wait,
			( cb_priv->in_fifo_half_full && have_fifo_word( cb_priv ) ) ||
			test_bit( RECEIVED_END_BN, &nec_priv->state ) ||
			test_bit( TIMO_NUM, &board->status ) ) )
		{
			printk("cb7210: fifo half full wait interrupted\n");
			retval = -ERESTARTSYS;
			nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 0 );
			break;
		}
		if( test_bit( TIMO_NUM, &board->status ) )
		{
			retval = -ETIMEDOUT;
			nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 0 );
			break;
		}

		spin_lock_irqsave( &board->spinlock, flags );
		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 0 );
printk("hs 0x%x, cnt %i\n", inb( nec_priv->iobase + HS_STATUS ), count );

		while( have_fifo_word( cb_priv ) )
		{
			word = inw( iobase + DIR );
			buffer[ count++ ] = ( word >> ( swap_bytes ? 8 : 0 ) ) & 0xff;
			buffer[ count++ ] = ( word >> ( swap_bytes ? 0 : 8 ) ) & 0xff;
		}
		hs_status = inb( nec_priv->iobase + HS_STATUS );
		if( hs_status & HS_RX_LSB_NOT_EMPTY )
		{
			word = inw( iobase + DIR );
			buffer[ count++ ] = word & 0xff;
			swap_bytes = 1;
		}else if( hs_status & HS_RX_MSB_NOT_EMPTY )
		{
			word = inw( iobase + DIR );
			buffer[ count++ ] = ( word >> 8 ) & 0xff;
			swap_bytes = 0;
		}
		cb_priv->in_fifo_half_full = 0;

		spin_unlock_irqrestore( &board->spinlock, flags );

		if( test_bit( RECEIVED_END_BN, &nec_priv->state ) ||
			( hs_status & HS_FIFO_FULL ) )
		{
printk("end or ff hs 0x%x\n", hs_status );
			break;
		}
	}
	if( test_and_clear_bit( RECEIVED_END_BN, &nec_priv->state ) )
	{
		*end = 1;
	}

	return retval ? retval : count;
}

ssize_t cb7210_accel_read( gpib_board_t *board, uint8_t *buffer,
	size_t length, int *end )
{
	ssize_t retval, count = 0;
	cb7210_private_t *cb_priv = board->private_data;
	nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;
	unsigned long flags;

	//XXX deal with lack of eos capability when using fifos
	if( length < cb7210_fifo_size + num_finishing_bytes/* || ( nec_priv->auxa_bits & HR_REOS XXX ) */ )
	{
		return cb7210_read( board, buffer, length, end );
	}
	*end = 0;

	/* release rfd holdoff */
	nec7210_set_handshake_mode( nec_priv, HR_HLDE );
	nec7210_release_rfd_holdoff( nec_priv );

	if( wait_event_interruptible( board->wait,
		test_bit( READ_READY_BN, &nec_priv->state ) ||
		test_bit( TIMO_NUM, &board->status ) ) )
	{
		printk("cb7210: read ready wait interrupted\n");
		return -ERESTARTSYS;
	}
	if( test_bit( TIMO_NUM, &board->status ) )
		return -ETIMEDOUT;

	input_fifo_enable( board, 1 );

	count = fifo_read( board, cb_priv, buffer, length - num_finishing_bytes, end );
	input_fifo_enable( board, 0 );
	if( count < 0 )
		return count;

	if( *end ) return count;

	retval = cb7210_read( board, &buffer[ count ], num_finishing_bytes, end );
	if( retval < 0 ) return retval;
	count += retval;

	return count;
}





