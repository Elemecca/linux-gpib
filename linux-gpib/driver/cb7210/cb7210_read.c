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

static inline int have_input_fifo_word( const cb7210_private_t *cb_priv )
{
	const nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;

	if( ( inb( nec_priv->iobase + HS_STATUS ) & ( HS_RX_MSB_NOT_EMPTY | HS_RX_LSB_NOT_EMPTY ) ) ==
		( HS_RX_MSB_NOT_EMPTY | HS_RX_LSB_NOT_EMPTY ) )
		return 1;
	else
		return 0;
}

static inline int have_input_fifo_byte( const cb7210_private_t *cb_priv )
{
	const nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;

	if( ( inb( nec_priv->iobase + HS_STATUS ) & ( HS_RX_MSB_NOT_EMPTY | HS_RX_LSB_NOT_EMPTY ) ) )
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

		cb_priv->hs_mode_bits &= ~HS_ENABLE_MASK;
		cb_priv->hs_mode_bits |= HS_RX_ENABLE;
		outb( cb_priv->hs_mode_bits, nec_priv->iobase + HS_MODE );

		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 1 );
	}else
	{
		nec7210_set_reg_bits( nec_priv, IMR2, HR_DMAI, 0 );

		cb_priv->hs_mode_bits &= ~HS_ENABLE_MASK;
		outb( cb_priv->hs_mode_bits, nec_priv->iobase + HS_MODE );
	}

	spin_unlock_irqrestore( &board->spinlock, flags );
}

static ssize_t pio_read( gpib_board_t *board, cb7210_private_t *cb_priv, uint8_t *buffer, size_t length )
{
	size_t count = 0;
	ssize_t retval = 0;
	nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;
	unsigned long iobase = nec_priv->iobase;

	while( count < length )
	{
		if( wait_event_interruptible( board->wait,
			have_input_fifo_word( cb_priv ) ||
			test_bit( RECEIVED_END_BN, &nec_priv->state ) ||
			test_bit( TIMO_NUM, &board->status ) ) )
		{
			printk("gpib: pio read wait interrupted\n");
			retval = -ERESTARTSYS;
			break;
		}
		if( test_bit( TIMO_NUM, &board->status ) )
			break;

		clear_bit( READ_READY_BN, &nec_priv->state );

		while( have_input_fifo_word( cb_priv ) )
		{
			uint16_t word;

			word = inw( iobase + DIR );
			if( count < length )
				buffer[ count++ ] = word & 0xff;
			if( count < length )
				buffer[ count++ ] = ( word >> 8 ) & 0xff;
		}
		if( test_bit( RECEIVED_END_BN, &nec_priv->state ) )
		{
			while( have_input_fifo_word( cb_priv ) )
			{
				uint16_t word;

				word = inw( iobase + DIR );
				if( count < length )
					buffer[ count++ ] = word & 0xff;
				if( count < length )
					buffer[ count++ ] = ( word >> 8 ) & 0xff;
			}
			if( have_input_fifo_byte( cb_priv ) )
			{
				if( count < length )
					buffer[ count++ ] = inw( iobase + DIR ) & 0xff;
			}
			break;
		}
	}
	if( test_bit( TIMO_NUM, &board->status ) )
		retval = -ETIMEDOUT;

	return retval ? retval : count;
}

ssize_t cb7210_accel_read( gpib_board_t *board, uint8_t *buffer,
	size_t length, int *end )
{
	ssize_t retval = 0;
	cb7210_private_t *cb_priv = board->private_data;
	nec7210_private_t *nec_priv = &cb_priv->nec7210_priv;

	*end = 0;

//XXX deal with lack of eos capability when using fifos

	/* release rfd holdoff */
	outb( AUX_FH, nec_priv->iobase + AUXMR );
	// holdoff on END
	nec7210_set_auxa_bits( nec_priv, HR_HANDSHAKE_MASK, 0 );
	nec7210_set_auxa_bits( nec_priv, HR_HLDE, 1 );

	if( wait_event_interruptible( board->wait,
		test_bit( READ_READY_BN, &nec_priv->state ) ||
		test_bit( TIMO_NUM, &board->status ) ) )
	{
		printk("gpib: accel wait interrupted\n");
		retval = -ERESTARTSYS;
		return retval;
	}
	if( test_bit( TIMO_NUM, &board->status ) )
		return -ETIMEDOUT;

	input_fifo_enable( board, 1 );

	retval = pio_read( board, cb_priv, buffer, length );
	input_fifo_enable( board, 0 );
	if( retval < 0 )
		return retval;

	if( test_and_clear_bit( RECEIVED_END_BN, &nec_priv->state ) )
		*end = 1;

	return retval;
}





