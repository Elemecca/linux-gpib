/*************************************************************************
 *                      gpib_bitbang.c  -  description                    *
 *                           -------------------                          *
 *  This code has been developed at the Institute of Sensor and Actuator  *
 *  Systems (Technical University of Vienna, Austria) to enable the GPIO  *
 *  lines (e.g. of a raspberry pi) to function as a GPIO master device    *
 *                                                                        *
 *  begin                : March 2016                                     *
 *  copyright            : (C) 2016 Thomas Klima                          *
 *  email                : elektronomikon@gmail.com                       *
 *                                                                        *
 *************************************************************************/

/**************************************************************************
 *  Jan. 2017: widely modified to add SRQ and use interrupts for the      *
 *             long waits.                                                *
 *  Nov. 2020: update of types, structure references, gpio lines preset,  *
 *             documentation.                                             *
 *                                                                        *
 *  by:           Marcello Carla'                                         *
 *  at:           Department of Physics - University of Florence, Italy   *
 *  email:        carla@fi.infn.it                                        *
 *                                                                        *
 **************************************************************************/

/**************************************************************************
 * Mar. 2021:	switch to GPIO descriptor driver interface                *
 *		SN7516x driver option for compatability with raspi_gpib   *
 * by:		Thomas Klima                                              *
 **************************************************************************/


/**************************************************************************
 *                                                                        *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2 of the License, or    *
 *   (at your option) any later version.                                  *
 *                                                                        *
 *************************************************************************/

/*
  limitations:
        works only on RPi
	cannot function as non-CIC system controller with sn7516x_used==1 because
	SN7561B cannot simultaneously make ATN input with IFC and REN as outputs.
  not implemented:
        parallel poll
        return2local
        device support (non master operation)
*/

#define TIMEOUT_US 1000000
#define IRQ_DEBOUNCE_US 1000
#define DELAY 10
#define JIFFY (1000000 / HZ)

#define NAME "gpib_bitbang"
#define HERE  NAME, (char *) __FUNCTION__

/* Debug print levels:
   0 = no debug messages
   1 = functions and errors
   2 = 1 + interrupt, line level and protocol details
*/
#define dbg_printk(level,frm,...) if (debug>=level)	\
               printk(KERN_INFO "%s:%s - " frm, HERE, ## __VA_ARGS__ )

#define LINVAL gpiod_get_value(DAV), \
               gpiod_get_value(NRFD),\
               gpiod_get_value(NDAC),\
               gpiod_get_value(SRQ)
#define LINFMT "DAV: %d  NRFD:%d  NDAC: %d SRQ: %d"
#define UDELAY udelay(DELAY)

#include "gpibP.h"
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>

static int sn7516x_used=1;
module_param(sn7516x_used,int,0660);

/**********************************************
 *  Signal pairing and pin wiring between the *
 *  Raspberry-Pi connector and the GPIB bus   *
 *                                            *
 *               signal           pin wiring  *
 *            GPIB  Pi-gpio     GPIB  ->  RPi *
**********************************************/
typedef enum {
        D01_pin_nr =  20,     /*   1  ->  38  */
        D02_pin_nr =  26,     /*   2  ->  37  */
        D03_pin_nr =  16,     /*   3  ->  36  */
        D04_pin_nr =  19,     /*   4  ->  35  */
        D05_pin_nr =  13,     /*  13  ->  33  */
        D06_pin_nr =  12,     /*  14  ->  32  */
        D07_pin_nr =   6,     /*  15  ->  31  */
        D08_pin_nr =   5,     /*  16  ->  29  */
        EOI_pin_nr =   9,     /*   5  ->  21  */
        DAV_pin_nr =  10,     /*   6  ->  19  */
        NRFD_pin_nr = 24,     /*   7  ->  18  */
        NDAC_pin_nr = 23,     /*   8  ->  16  */
        IFC_pin_nr =  22,     /*   9  ->  15  */
        SRQ_pin_nr =  11,     /*  10  ->  23  */
        _ATN_pin_nr = 25,     /*  11  ->  22  */
        REN_pin_nr =  27,     /*  17  ->  13  */
/*
 *  GROUND PINS
 *    12,18,19,20,21,22,23,24  => 14,20,25,30,34,39
 */

/*
 *  These lines are used to control the external
 *  SN75160/161 driver chips when sn7516x_used==1
 *  Not used when sn7516x_used==0. In this case there is
 *  reduced fan out; currently tested up to 4 devices.
 */

/*
 *         	 Pi GPIO        RPI   75161B 75160B   Description      */
        PE_pin_nr =    7,    /*  26  ->   nc     11   Pullup Enable    */
        DC_pin_nr =    8,    /*  24  ->   12     nc   Directon control */
        TE_pin_nr =   18,    /*  12  ->    2      1   Talk Enable      */
        ACT_LED_pin_nr = 4,  /*   7  ->  LED  */
} lines_t;

/*
 * GPIO descriptors
 */

struct gpio_desc *D01, *D02, *D03, *D04, *D05, *D06, *D07, *D08; // data lines
struct gpio_desc *EOI, *NRFD, *IFC, *_ATN, *REN, *DAV, *NDAC, *SRQ; // control
struct gpio_desc *PE, *DC, *TE; // line driver control pins
struct gpio_desc *ACT_LED;  // status led

/* struct which defines private_data for gpio driver */

typedef struct
{
        int irq_NRFD;
        int irq_NDAC;
        int irq_DAV;
        int irq_SRQ;
	uint8_t eos;       // eos character
        short eos_flags;   // eos mode
        short int end;
        int request;
        int count;
	int direction;
	int t1_delay;
        uint8_t * rbuf;
        uint8_t * wbuf;
} bb_private_t;

inline long int usec_diff(struct timespec64 *a, struct timespec64 *b);
void set_data_lines(uint8_t byte);
uint8_t get_data_lines(void);
void set_data_lines_input(void);
void set_data_lines_output(void);
int check_for_eos(bb_private_t *priv, uint8_t byte);

inline void SET_DIR_WRITE(bb_private_t *priv);
inline void SET_DIR_READ(bb_private_t *priv);

#define DIR_READ 0
#define DIR_WRITE 1

MODULE_LICENSE("GPL");

/****  global variables  ****/

static int debug = GPIB_CONFIG_KERNEL_DEBUG ? 1 : 0;
module_param (debug, int, S_IRUGO | S_IWUSR);

char printable (char x) {
        if (x < 32 || x > 126) return ' ';
        return x;
}

/***************************************************************************
 *                                                                         *
 * READ                                                                    *
 *                                                                         *
 ***************************************************************************/

int bb_read(gpib_board_t *board, uint8_t *buffer, size_t length,
            int *end, size_t *bytes_read)
{
        bb_private_t *priv = board->private_data;

        int retval=0;
	int end_flag;

        dbg_printk(1, "board: %p  lock %d  length: %zu\n",
                board, mutex_is_locked(&board->user_mutex), length);

        priv->end = 0;
        priv->count = 0;
        priv->rbuf = buffer;

        if (length == 0) goto read_end;

        priv->request = length;

        SET_DIR_READ(priv);
        UDELAY;

        dbg_printk (2,".........." LINFMT "\n",LINVAL);

        /* poll loop for data read */

        while (1) {
                gpiod_direction_input(NRFD); // ready for data
		while (1) { // wait for dav low = data valid
			retval = wait_event_interruptible (
				board->wait,(!gpiod_get_value(DAV) || board->status & TIMO)
				);

			if (board->status & TIMO) {
				retval = -ETIMEDOUT;
				dbg_printk (1,"timeout for DAV Lo:  " LINFMT "\n", LINVAL);
				gpiod_direction_output(NRFD, 0); // DIR_READ line sta
				goto read_end;
			}
			if (!retval) break;
		}

                gpiod_direction_output(NRFD, 0); // not ready for data

                priv->rbuf[priv->count++] = get_data_lines();
                priv->end = !gpiod_get_value(EOI);

                dbg_printk (2,LINFMT " count: %3d eoi: %d  val: %2x -> %c\n",
                            LINVAL, priv->count-1, priv->end,
                            priv->rbuf[priv->count-1],
                            printable(priv->rbuf[priv->count-1]));

                gpiod_direction_input(NDAC); // data accepted
		priv->end |= check_for_eos(priv, priv->rbuf[priv->count-1]);

                end_flag = ((priv->count >= priv->request) || priv->end);

		while (1) { // wait for dav high = data not valid
			retval = wait_event_interruptible (
				board->wait,(gpiod_get_value(DAV) || board->status & TIMO)
				);

			if (board->status & TIMO) {
				retval = -ETIMEDOUT;
				dbg_printk (1,"timeout for DAV Hi: " LINFMT "\n", LINVAL);
				gpiod_direction_output(NDAC, 0); // DIR_READ line state
				goto read_end;
			}
			if (!retval) break;
		}

                gpiod_direction_output(NDAC, 0); // data not accepted

		if (end_flag) break;

                udelay (DELAY);
        }

read_end:
        *bytes_read = priv->count;
        *end = priv->end;

        dbg_printk(1,"return: %d  eoi|eos: %d count: %d\n\n", retval, priv->end, priv->count);

        return retval;
}

/***************************************************************************
 *                                                                         *
 *      READ interrupt routine (DAV line)                                  *
 *                                                                         *
 ***************************************************************************/

irqreturn_t bb_DAV_interrupt(int irq, void * arg)
{
        gpib_board_t * board = arg;
	bb_private_t *priv = board->private_data;

        dbg_printk (2,"> %d   st: %4lx dir: %d\n",
		gpiod_get_value(DAV), board->status, priv->direction);

        wake_up_interruptible(&board->wait);

        return IRQ_HANDLED;
}

/***************************************************************************
 *                                                                         *
 * WRITE                                                                   *
 *                                                                         *
 ***************************************************************************/

int bb_write(gpib_board_t *board, uint8_t *buffer, size_t length,
             int send_eoi, size_t *bytes_written)
{
	int i;
	size_t cnt = 0;
        int retval = 0;
	int end;

        bb_private_t *priv = board->private_data;

        dbg_printk(1,"board %p  lock %d  length: %zu\n",
                board, mutex_is_locked(&board->user_mutex), length);

        if (debug>1) {
                dbg_printk(2,"<%zu %s>\n", length, (send_eoi)?"w.EOI":" ");
                for (i=0; i < length; i++) {
                        dbg_printk(2,"%3d  0x%x->%c\n", i, buffer[i], printable(buffer[i]));
                }
        }

        priv->count = 0;
        if (length == 0) goto write_end;
        priv->end = send_eoi;

        SET_DIR_WRITE(priv);

        dbg_printk(2,"NRFD: %d   NDAC: %d\n",
                    gpiod_get_value(NRFD), gpiod_get_value(NDAC));

        /*  poll loop for data write */

	while (1) {
		while (1) {
			// wait for ready for data
			retval = wait_event_interruptible (
				board->wait,
				(gpiod_get_value(NRFD) || (board->status & TIMO)));
			if (board->status & TIMO) {
				retval = -ETIMEDOUT;
				dbg_printk (1,"timeout for NRFD Hi " LINFMT "\n", LINVAL);
				goto write_end;
			}
			if (!retval) break;
		}

		dbg_printk(2,"sending %zu\n", cnt);

                set_data_lines(buffer[cnt++]); // put the data on the lines

		end = (cnt == length);
		if (end && send_eoi) {
                        gpiod_direction_output(EOI, 0); // Assert EIO
		}

		// ndelay(priv->t1_delay); // wait for data to settle on the lines

		gpiod_direction_output(DAV, 0); // Data available

		while (1) { // wait for data accepted
			retval = wait_event_interruptible(
				board->wait,
				(gpiod_get_value(NDAC) || (board->status & TIMO)));
			if (board->status & TIMO) {
				retval = -ETIMEDOUT;
				dbg_printk (1,"timeout for NDAC Hi " LINFMT "\n", LINVAL);
				gpiod_direction_output(DAV, 1); // DIR_WRITE line state
				goto write_end;
			}
			if (!retval) break;
		}

                dbg_printk(2,"accepted %zu\n", cnt-1);

                UDELAY;
                gpiod_direction_output(DAV, 1); // Data not available
		if (end) {
			if (send_eoi)
				gpiod_direction_output(EOI, 1); // De-assert EIO
			break; // done
		}

        }
        retval = cnt;

write_end:
        *bytes_written = cnt;

        dbg_printk(1,"sent %zu bytes.\r\n\r\n", *bytes_written);

        return retval;
}

/***************************************************************************
 *                                                                         *
 *      WRITE interrupt routine (NRFD line)                                *
 *                                                                         *
 ***************************************************************************/

irqreturn_t bb_NRFD_interrupt(int irq, void * arg)
{
        gpib_board_t * board = arg;
	bb_private_t *priv = board->private_data;

        dbg_printk (2,"> %d   st: %4lx dir: %d\n",
		gpiod_get_value(NRFD), board->status, priv->direction);

        wake_up_interruptible(&board->wait);

        return IRQ_HANDLED;
}

/***************************************************************************
 *                                                                         *
 *      WRITE interrupt routine (NDAC line)                                *
 *                                                                         *
 ***************************************************************************/

irqreturn_t bb_NDAC_interrupt(int irq, void * arg)
{
        gpib_board_t * board = arg;

        	bb_private_t *priv = board->private_data;

        dbg_printk (2,"> %d   st: %4lx dir: %d\n",
		gpiod_get_value(NDAC), board->status, priv->direction);

        wake_up_interruptible(&board->wait);

        return IRQ_HANDLED;
}

/***************************************************************************
 *                                                                         *
 *      interrupt routine for SRQ line                                     *
 *                                                                         *
 ***************************************************************************/

irqreturn_t bb_SRQ_interrupt(int irq, void * arg)
{
        gpib_board_t  * board = arg;

        int val = gpiod_get_value(SRQ);

        dbg_printk(2,"  -> %d   st: %4lx\n", val, board->status);

        if (!val) set_bit(SRQI_NUM, &board->status);  /* set_bit() is atomic */

        wake_up_interruptible(&board->wait);

        return IRQ_HANDLED;
}

int bb_command(gpib_board_t *board, uint8_t *buffer,
                   size_t length, size_t *bytes_written)
{
        size_t ret,i;

	dbg_printk(1,"%p  %p\n", buffer, board->buffer);

        gpiod_direction_output(_ATN, 0);

        if (debug>1) {
                dbg_printk(2,"CMD(%zu):\n", length);
                for (i=0; i < length; i++) {
			dbg_printk(2,"0x%x=%s%d\n", buffer[i],
				(buffer[i] & 0x40)?"TLK":"LSN",buffer[i]&0x1F);
		}
        }

        ret = bb_write(board, buffer, length, 0, bytes_written); // no eoi
        gpiod_direction_output(_ATN, 1);

        return ret;
}

/***************************************************************************
 *                                                                         *
 * STATUS Management                                                       *
 *                                                                         *
 ***************************************************************************/


int bb_take_control(gpib_board_t *board, int synchronous)
{
        UDELAY;
	dbg_printk(1,"%d\n", synchronous);
        gpiod_direction_output(_ATN, 0);
        set_bit(CIC_NUM, &board->status);
        return 0;
}

int bb_go_to_standby(gpib_board_t *board)
{
	dbg_printk(1,"\n");
        UDELAY;
        gpiod_direction_output(_ATN, 1);
        return 0;
}

void bb_request_system_control(gpib_board_t *board, int request_control )
{
	dbg_printk(1,"%d\n", request_control);
        UDELAY;
        if (request_control)
                set_bit(CIC_NUM, &board->status);
        else
                clear_bit(CIC_NUM, &board->status);
}

void bb_interface_clear(gpib_board_t *board, int assert)
{
        UDELAY;
	dbg_printk(1,"%d\n", assert);
        if (assert)
                gpiod_direction_output(IFC, 0);
        else
                gpiod_direction_output(IFC, 1);
}

void bb_remote_enable(gpib_board_t *board, int enable)
{
	dbg_printk(1,"%d\n", enable);
        UDELAY;
        if (enable) {
                set_bit(REM_NUM, &board->status);
                gpiod_direction_output(REN, 0);
        } else {
                clear_bit(REM_NUM, &board->status);
                gpiod_direction_output(REN, 1);
        }
}

int bb_enable_eos(gpib_board_t *board, uint8_t eos_byte, int compare_8_bits)
{
        bb_private_t *priv = board->private_data;
        dbg_printk(1,"%s\n", "EOS_en");
        priv->eos = eos_byte;
        priv->eos_flags = REOS;
        if (compare_8_bits) priv->eos_flags |= BIN;

        return 0;
}

void bb_disable_eos(gpib_board_t *board)
{
        bb_private_t *priv = board->private_data;
        dbg_printk(1,"\n");
        priv->eos_flags &= ~REOS;
}

unsigned int bb_update_status(gpib_board_t *board, unsigned int clear_mask )
{
        board->status &= ~clear_mask;

        if (gpiod_get_value(SRQ)) {                    /* SRQ asserted low */
                clear_bit (SRQI_NUM, &board->status);
        } else {
                set_bit (SRQI_NUM, &board->status);
        }

        dbg_printk(1,"0x%lx mask 0x%x\n",board->status, clear_mask);

        return board->status;
}

int bb_primary_address(gpib_board_t *board, unsigned int address)
{
        dbg_printk(1,"%d\n", address);
        board->pad = address;
        return 0;
}

int bb_secondary_address(gpib_board_t *board, unsigned int address, int enable)
{
        dbg_printk(1,"%d %d\n", address, enable);
        if (enable)
                board->sad = address;
        return 0;
}

int bb_parallel_poll(gpib_board_t *board, uint8_t *result)
{
        dbg_printk(1,"%s\n", "not implemented");
        return -ENOSYS;
}
void bb_parallel_poll_configure(gpib_board_t *board, uint8_t config )
{
	dbg_printk(1,"%s\n", "not implemented");
}
void bb_parallel_poll_response(gpib_board_t *board, int ist )
{
}
void bb_serial_poll_response(gpib_board_t *board, uint8_t status)
{
        dbg_printk(1,"%s\n", "not implemented");
}
uint8_t bb_serial_poll_status(gpib_board_t *board )
{
        dbg_printk(1,"%s\n", "not implemented");
        return 0; // -ENOSYS;
}
unsigned int bb_t1_delay( gpib_board_t *board,  unsigned int nano_sec )
{
	bb_private_t *priv = board->private_data;
	unsigned int retval;

	if ( nano_sec <= 350 ) retval = priv->t1_delay = 350;
	else if ( nano_sec <= 1100 ) retval = priv->t1_delay = 1100;
	else retval = priv->t1_delay = 2000;

	dbg_printk(1,"t1 delay set to %d nanosec\n", priv->t1_delay);

	return retval;
}

void bb_return_to_local(gpib_board_t *board )
{
        dbg_printk(1,"%s\n", "not implemented");
}

int bb_line_status(const gpib_board_t *board )
{
	int line_status = ValidALL;

        if (gpiod_get_value(REN) == 0) line_status |= BusREN;
        if (gpiod_get_value(IFC) == 0) line_status |= BusIFC;
        if (gpiod_get_value(NDAC) == 0) line_status |= BusNDAC;
        if (gpiod_get_value(NRFD) == 0) line_status |= BusNRFD;
        if (gpiod_get_value(DAV) == 0) line_status |= BusDAV;
        if (gpiod_get_value(EOI) == 0) line_status |= BusEOI;
        if (gpiod_get_value(_ATN) == 0) line_status |= BusATN;
        if (gpiod_get_value(SRQ) == 0) line_status |= BusSRQ;

        dbg_printk(1,"status lines: %4x\n", line_status);

            return line_status;
}

/***************************************************************************
 *                                                                         *
 * Module Management                                                       *
 *                                                                         *
 ***************************************************************************/

static int allocate_private(gpib_board_t *board)
{
        board->private_data = kmalloc(sizeof(bb_private_t), GFP_KERNEL);
        if (board->private_data == NULL)
                return -1;
        memset(board->private_data, 0, sizeof(bb_private_t));
        return 0;
}

static void free_private(gpib_board_t *board)
{
        if (board->private_data) {
                kfree(board->private_data);
                board->private_data = NULL;
        }
}

int bb_get_irq(gpib_board_t *board, char * name, int irq,
	irq_handler_t handler, unsigned long flags)
{
      struct timespec64 before, after;

      ktime_get_ts64(&before);
      if (request_irq(irq, handler, flags, "NAME", board)) {
	      printk("gpib: can't request IRQ for %s %d\n", name,irq);
	      return -1;
      }
      ktime_get_ts64(&after);
      dbg_printk(2,"%s:%s - IRQ for DAV in %ld us\n", HERE, usec_diff(&after, &before));

      return 0;
}

static void allocate_gpios(void)
{
	D01 = gpio_to_desc(D01_pin_nr);
        D02 = gpio_to_desc(D02_pin_nr);
        D03 = gpio_to_desc(D03_pin_nr);
        D04 = gpio_to_desc(D04_pin_nr);
        D05 = gpio_to_desc(D05_pin_nr);
        D06 = gpio_to_desc(D06_pin_nr);
        D07 = gpio_to_desc(D07_pin_nr);
        D08 = gpio_to_desc(D08_pin_nr);

        EOI = gpio_to_desc(EOI_pin_nr);
        NRFD = gpio_to_desc(NRFD_pin_nr);
        IFC = gpio_to_desc(IFC_pin_nr);
        _ATN = gpio_to_desc(_ATN_pin_nr);
        REN = gpio_to_desc(REN_pin_nr);
        DAV = gpio_to_desc(DAV_pin_nr);
        NDAC = gpio_to_desc(NDAC_pin_nr);
        SRQ = gpio_to_desc(SRQ_pin_nr);

	// All lines passive to start with
	set_data_lines_input();

	gpiod_direction_input(IFC);
        gpiod_direction_input(_ATN);
        gpiod_direction_input(REN);
        gpiod_direction_input(SRQ);
        gpiod_direction_input(DAV);
        gpiod_direction_input(EOI);
	gpiod_direction_input(NRFD);
        gpiod_direction_input(NDAC);

	if (sn7516x_used) {
		PE = gpio_to_desc(PE_pin_nr);
		DC = gpio_to_desc(DC_pin_nr);
		TE = gpio_to_desc(TE_pin_nr);
	}
}

static void release_gpios(void)
{
        gpiod_put(D01);
        gpiod_put(D02);
        gpiod_put(D03);
        gpiod_put(D04);
        gpiod_put(D05);
        gpiod_put(D06);
        gpiod_put(D07);
        gpiod_put(D08);
        gpiod_put(EOI);
        gpiod_put(NRFD);
        gpiod_put(IFC);
        gpiod_put(_ATN);
        gpiod_put(REN);
        gpiod_put(DAV);
        gpiod_put(NDAC);
        gpiod_put(SRQ);
	if (sn7516x_used) {
		gpiod_put(PE);
		gpiod_put(DC);
		gpiod_put(TE);
	}
}

int bb_attach(gpib_board_t *board, const gpib_board_config_t *config)
{
        bb_private_t *priv;
  	int retval;

        dbg_printk(1,"%s\n", "Enter ...");

        board->status = 0;
	if (!board->master) {
		printk("gpib: gpio_bitbang driver must be master\n");
		return -1;
	}

        if (allocate_private(board)) return -ENOMEM;
        priv = board->private_data;
	priv->direction = -1;
	priv->t1_delay = 2000;

	allocate_gpios();

	if (sn7516x_used) {
	/* Configure SN7516X control lines.
	 * drive ATN, IFC and REN as outputs only when master
         * i.e. system controller. In this mode can only be the CIC
	 * When not master then enable device mode ATN, IFC & REN as inputs
         */
		gpiod_direction_output(DC,0);
		gpiod_direction_output(TE,1);
		gpiod_direction_output(PE,1);
	}


        priv->irq_NRFD = gpiod_to_irq(NRFD);
        priv->irq_NDAC = gpiod_to_irq(NDAC);
        priv->irq_DAV = gpiod_to_irq(DAV);
        priv->irq_SRQ = gpiod_to_irq(SRQ);

        dbg_printk(2,"%s:%s - IRQ's: DAV: %d NRFD: %d NDAC: %d SRQ %d\n", HERE,
		priv->irq_DAV, priv->irq_NRFD, priv->irq_NDAC, priv->irq_SRQ);

        /* request DAV interrupt for read */
	if (bb_get_irq(board, "DAV", priv->irq_DAV, bb_DAV_interrupt,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) {
		retval = -1;
                goto exit;
        }

        /* request NRFD interrupt for write */
	if (bb_get_irq(board, "NRFD", priv->irq_NRFD, bb_NRFD_interrupt,
			IRQF_TRIGGER_RISING)) {
		retval = -1;
                goto exit;
        }

        /* request NDAC interrupt for write */
	if (bb_get_irq(board, "NDAC", priv->irq_NDAC, bb_NDAC_interrupt,
			IRQF_TRIGGER_RISING)) {
		retval = -1;
                goto exit;
        }

        /* request SRQ interrupt for Service Request */
	if (bb_get_irq(board, "SRQ", priv->irq_SRQ, bb_SRQ_interrupt,
			IRQF_TRIGGER_FALLING)) {
		retval = -1;
                goto exit;
        }

        /* done */

	SET_DIR_WRITE(priv); // drive DAV and EOI false, enable NRFD and NDAC

        dbg_printk(0,"attached board index: %d\n", board->minor);

        return 0;
exit:
	release_gpios();
	free_private(board);
	dbg_printk(0,"attach failed for board index: %d\n", board->minor);
	return retval;
}

void bb_detach(gpib_board_t *board)
{
	struct timespec64 before, after;
        bb_private_t *priv = board->private_data;

        dbg_printk(1,"%s\n", "enter... ");

	if (priv->irq_DAV) {
		ktime_get_ts64(&before);
                free_irq(priv->irq_DAV, board);
		ktime_get_ts64(&after);
		dbg_printk(2,"IRQ DAV free in %ld us\n", usec_diff(&after, &before));
        }

        if (priv->irq_NRFD) {
		ktime_get_ts64(&before);
                free_irq(priv->irq_NRFD, board);
		ktime_get_ts64(&after);
		dbg_printk(2,"IRQ NRFD free in %ld us\n", usec_diff(&after, &before));
        }

	if (priv->irq_NDAC) {
		ktime_get_ts64(&before);
                free_irq(priv->irq_NDAC, board);
		ktime_get_ts64(&after);
		dbg_printk(2,"IRQ NDAC free in %ld us\n", usec_diff(&after, &before));
        }

        if (priv->irq_SRQ) {
		ktime_get_ts64(&before);
		free_irq(priv->irq_SRQ, board);
		ktime_get_ts64(&after);
		dbg_printk(2,"IRQ DAV free in %ld us\n", usec_diff(&after, &before));
        }

	release_gpios();

        free_private(board);

	dbg_printk(0,"detached board index: %d\n", board->minor);

}

gpib_interface_t bb_interface =
{
        name:                     NAME,
        attach:                   bb_attach,
        detach:                   bb_detach,
        read:                     bb_read,
        write:                    bb_write,
        command:                  bb_command,
        take_control:             bb_take_control,
        go_to_standby:            bb_go_to_standby,
        request_system_control:   bb_request_system_control,
        interface_clear:          bb_interface_clear,
        remote_enable:            bb_remote_enable,
        enable_eos:               bb_enable_eos,
        disable_eos:              bb_disable_eos,
        parallel_poll:            bb_parallel_poll,
        parallel_poll_configure:  bb_parallel_poll_configure,
        parallel_poll_response:   bb_parallel_poll_response,
        line_status:              bb_line_status,
        update_status:            bb_update_status,
        primary_address:          bb_primary_address,
        secondary_address:        bb_secondary_address,
        serial_poll_response:     bb_serial_poll_response,
        serial_poll_status:       bb_serial_poll_status,
        t1_delay:                 bb_t1_delay,
        return_to_local:          bb_return_to_local,
};

static int __init bb_init_module(void)
{
	ACT_LED = gpio_to_desc(ACT_LED_pin_nr);
	gpiod_direction_output(ACT_LED, 1); // show module is loaded

        gpib_register_driver(&bb_interface, THIS_MODULE);

        dbg_printk(1,"module loaded%s!", (sn7516x_used)?" with SN7516x driver support":"");
        return 0;
}

static void __exit bb_exit_module(void)
{
	gpiod_direction_input(ACT_LED);
        gpiod_put(ACT_LED);

        dbg_printk(1,"%s\n", "module unloaded!");

        gpib_unregister_driver(&bb_interface);
}

module_init(bb_init_module);
module_exit(bb_exit_module);



/***************************************************************************
 *                                                                         *
 * UTILITY Functions                                                       *
 *                                                                         *
 ***************************************************************************/

inline long int usec_diff (struct timespec64 * a, struct timespec64 * b)
{
        return ((a->tv_sec - b->tv_sec)*1000000 +
                (a->tv_nsec - b->tv_nsec)/1000);
}

int check_for_eos(bb_private_t *priv, uint8_t byte)
{
        static const uint8_t sevenBitCompareMask = 0x7f;

        if ((priv->eos_flags & REOS) == 0) return 0;

        if (priv->eos_flags & BIN) {
                if (priv->eos == byte)
                        return 1;
        } else {
                if ((priv->eos & sevenBitCompareMask) ==
                        (byte & sevenBitCompareMask))
                        return 1;
        }
        return 0;
}

void set_data_lines_output()
{
        gpiod_direction_output(D01, 1);
        gpiod_direction_output(D02, 1);
        gpiod_direction_output(D03, 1);
        gpiod_direction_output(D04, 1);
        gpiod_direction_output(D05, 1);
        gpiod_direction_output(D06, 1);
        gpiod_direction_output(D07, 1);
        gpiod_direction_output(D08, 1);
}

void set_data_lines(uint8_t byte)
{
        gpiod_set_value(D01, !(byte & 0x01));
        gpiod_set_value(D02, !(byte & 0x02));
        gpiod_set_value(D03, !(byte & 0x04));
        gpiod_set_value(D04, !(byte & 0x08));
        gpiod_set_value(D05, !(byte & 0x10));
        gpiod_set_value(D06, !(byte & 0x20));
        gpiod_set_value(D07, !(byte & 0x40));
        gpiod_set_value(D08, !(byte & 0x80));
}

uint8_t get_data_lines(void)
{
        uint8_t ret;
        ret = gpiod_get_value(D01);
        ret |= gpiod_get_value(D02) << 1;
        ret |= gpiod_get_value(D03) << 2;
        ret |= gpiod_get_value(D04) << 3;
        ret |= gpiod_get_value(D05) << 4;
        ret |= gpiod_get_value(D06) << 5;
        ret |= gpiod_get_value(D07) << 6;
        ret |= gpiod_get_value(D08) << 7;
        return ~ret;
}

void set_data_lines_input(void)
{
        gpiod_direction_input(D01);
        gpiod_direction_input(D02);
        gpiod_direction_input(D03);
        gpiod_direction_input(D04);
        gpiod_direction_input(D05);
        gpiod_direction_input(D06);
        gpiod_direction_input(D07);
        gpiod_direction_input(D08);
}

inline void SET_DIR_WRITE(bb_private_t *priv)
{
	if (priv->direction == DIR_WRITE)
		return;

	UDELAY;

	disable_irq(priv->irq_DAV);
        gpiod_direction_output(DAV, 1);
        gpiod_direction_output(EOI, 1);

        gpiod_direction_input(NRFD);
        gpiod_direction_input(NDAC);

	if (priv->direction == DIR_READ) { // enable lines disabled in set_dir_read
		enable_irq(priv->irq_NRFD);
		enable_irq(priv->irq_NDAC);
	}

        set_data_lines_output();

	if (sn7516x_used) {
                /* set data lines to transmit on sn75160b */
		gpiod_set_value(PE, 1);
		/* set NDAC and NRFD to receive and DAV to transmit on sn75161b */
		gpiod_set_value(TE, 1);
	}

	priv->direction = DIR_WRITE;
}

inline void SET_DIR_READ(bb_private_t *priv)
{
	if (priv->direction == DIR_READ)
		return;

	UDELAY;

	disable_irq(priv->irq_NRFD);
	disable_irq(priv->irq_NDAC);
	gpiod_direction_output(NRFD, 0);  // hold off the talker
        gpiod_direction_output(NDAC, 0);  // data not accepted

	gpiod_direction_input(DAV);
        gpiod_direction_input(EOI);

	if (priv->direction == DIR_WRITE) {// enable line disabled in set_dir_write
		enable_irq(priv->irq_DAV);
	}

	set_data_lines_input();

	if (sn7516x_used) {
                /* set data lines to receive on sn75160b */
		gpiod_set_value(PE, 0);
		/* set NDAC and NRFD to transmit and DAV to receive on sn75161b */
		gpiod_set_value(TE, 0);
	}
	priv->direction = DIR_READ;
}
