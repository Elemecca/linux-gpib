/***************************************************************************
                               sys/osfuncs.c
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

#include <ibsys.h>

#include <linux/fcntl.h>

int board_type_ioctl(gpib_device_t *device, unsigned long arg);
int read_ioctl(gpib_device_t *device, unsigned long arg);
int command_ioctl(gpib_device_t *device, unsigned long arg);

#define GIVE_UP(a) {up(&device->mutex); return a;}

int ib_opened=0;
int ib_exclusive=0;

int ibopen(struct inode *inode, struct file *filep)
{
	unsigned int minor = MINOR(inode->i_rdev);

	if(minor >= MAX_NUM_GPIB_DEVICES)
	{
		printk("gpib: invalid minor number of device file\n");
		return -ENODEV;
	}

	if( ib_exclusive )
	{
		return (-EBUSY);
	}

	if ( filep->f_flags & O_EXCL )
	{
		if (ib_opened)
		{
			return (-EBUSY);
		}
		ib_exclusive=1;
	}

	ib_opened++;

	return 0;
}


int ibclose(struct inode *inode, struct file *file)
{
	unsigned int minor = MINOR(inode->i_rdev);
	gpib_device_t *device;

	if(minor >= MAX_NUM_GPIB_DEVICES)
	{
		printk("gpib: invalid minor number of device file\n");
		return -ENODEV;
	}
	device = &device_array[minor];
 
	if (device->online && ib_opened == 1 )
		ibonl(&device_array[minor], 0);
	ib_opened--;

	if( ib_exclusive )
		ib_exclusive = 0;

	return 0;
}

int ibioctl(struct inode *inode, struct file *filep, unsigned int cmd,
 unsigned long arg)
{
	int	retval = 0; 		/* assume everything OK for now */
	ibarg_t m_ibarg,*ibargp;
	int	remain;
	uint8_t 	*userbuf;
	char c;
	ssize_t ret;
	int end_flag = 0;
	unsigned int minor = MINOR(inode->i_rdev);
	gpib_device_t *device;

	if(minor >= MAX_NUM_GPIB_DEVICES)
	{
		printk("gpib: invalid minor number of device file\n");
		return -ENODEV;
	}
	device = &device_array[minor];
	if(cmd == CFCBOARDTYPE)
		return board_type_ioctl(device, arg);
	if(device->interface == NULL)
	{
		printk("gpib: no device configured on /dev/gpib%i\n", minor);
		return -ENODEV;
	}

printk("minor %i ioctl %i\n", minor, cmd);

	switch( cmd )
	{
		case IBRD:
			return read_ioctl(device, arg);
		case IBCMD:
			return command_ioctl(device, arg);
		default:
			break;
	}
	ibargp = (ibarg_t *) &m_ibarg;

	/* Check the arg buffer is readable & writable by the current process */
	retval = verify_area(VERIFY_WRITE, (void *)arg, sizeof(ibarg_t));
	if (retval)
	{
		return (retval);
	}

	retval = verify_area(VERIFY_READ, (void *)arg, sizeof(ibarg_t));
	if (retval)
	{
		return (retval);
	}

	copy_from_user( ibargp , (ibarg_t *) arg , sizeof(ibarg_t));

	ibargp->ib_iberr = EDVR;

// XXX
#if 0
	if( cmd == IBAPWAIT )
	{
		/* special case for IBAPWAIT : does his own locking */
		ibAPWait(ibargp->ib_arg);
		ibargp->ib_ibsta = ibsta;
		ibargp->ib_iberr = iberr;
		ibargp->ib_ibcnt = ibcnt;
		copy_to_user((ibarg_t *) arg, (ibarg_t *) ibargp , sizeof(ibarg_t));

		return retval;
	}
#endif

	/* lock other processes from performing commands */
	retval = down_interruptible(&device->mutex);
	if(retval)
	{
		printk("gpib: ioctl interrupted while waiting on lock\n");
		return -ERESTARTSYS;
	}

//XXX a lot of the content of this switch should be split out into seperate functions
	switch (cmd)
	{
		case IBWRT:	// XXX write should not be an ioclt

			/* Check read access to buffer */
			retval = verify_area(VERIFY_READ, ibargp->ib_buf, ibargp->ib_cnt);
			if (retval)
				GIVE_UP(retval);
			/* Write buffer loads till we empty the user supplied buffer */
			userbuf = ibargp->ib_buf;
			remain = ibargp->ib_cnt;
			while(remain > 0)
			{
				int send_eoi;
				send_eoi = device->buffer_length <= remain && device->send_eoi;
				copy_from_user(device->buffer, userbuf, (device->buffer_length < remain) ?
					device->buffer_length : remain );
				ret = ibwrt(device, device->buffer, (device->buffer_length < remain) ?
					device->buffer_length : remain, send_eoi);
				if(ret < 0)
				{
					retval = -EIO;
					break;
				}
				remain -= ret;
				userbuf += ret;
			};
			ibargp->ib_ibcnt = ibargp->ib_cnt - remain;
			break;
		case IBCMD:
			/* Check read access to buffer */
			retval = verify_area(VERIFY_READ, ibargp->ib_buf, ibargp->ib_cnt);
			if (retval)
				GIVE_UP(retval);

			/* Write buffer loads till we empty the user supplied buffer */
			userbuf = ibargp->ib_buf;
			remain = ibargp->ib_cnt;
			while (remain > 0 && !(ibstatus(device) & (TIMO)))
			{
				copy_from_user(device->buffer, userbuf, (device->buffer_length < remain) ?
					device->buffer_length : remain );
				ret = ibcmd(device, device->buffer, (device->buffer_length < remain) ?
					device->buffer_length : remain );
				if(ret < 0)
				{
					retval = -EIO;
					break;
				}
				remain -= ret;
				userbuf += ret;
			}
			ibargp->ib_ibcnt = ibargp->ib_cnt - remain;

			break;
		case IBWAIT:
			retval = ibwait(device, ibargp->ib_arg);
			break;
		case IBRPP:
			/* Check write access to Poll byte */
			retval = verify_area(VERIFY_WRITE, ibargp->ib_buf, 1);
			if (retval)
				GIVE_UP(retval);

			retval = ibrpp(device, &c);
			put_user( c, ibargp->ib_buf );
			break;
		case IBONL:
			retval = ibonl(device, ibargp->ib_arg);
			break;
		case IBAPE:
			ibAPE(device, ibargp->ib_arg,ibargp->ib_cnt);
			break;
		case IBSIC:
			retval = ibsic(device);
			break;
		case IBSRE:
			retval = ibsre(device, ibargp->ib_arg);
			break;
		case IBGTS:
			retval = ibgts(device);
			break;
		case IBCAC:
			retval = ibcac(device, ibargp->ib_arg);
			break;
		case IBSDBG:
			break;
		case IBLINES:
			retval = iblines(device, &ibargp->ib_ret);
			break;
		case IBPAD:
			retval = ibpad(device, ibargp->ib_arg);
			break;
		case IBSAD:
			retval = ibsad(device, ibargp->ib_arg);
			break;
		case IBTMO:
			retval = ibtmo(device, ibargp->ib_arg);
			break;
		case IBEOT:
			retval = ibeot(device, ibargp->ib_arg);
			break;
		case IBEOS:
			retval = ibeos(device, ibargp->ib_arg);
			break;
		case IBRSV:
			retval = ibrsv(device, ibargp->ib_arg);
			break;
		case DVTRG:
			retval = dvtrg(device, ibargp->ib_arg);
			break;
		case DVCLR:
			retval = dvclr(device, ibargp->ib_arg);
			break;
		case DVRSP:
			/* Check write access to Poll byte */
			retval = verify_area(VERIFY_WRITE, ibargp->ib_buf, 1);
			if (retval)
			{
				GIVE_UP(retval);
			}
			retval = dvrsp(device, ibargp->ib_arg, &c);

			put_user( c, ibargp->ib_buf );

			break;
		case IBAPRSP:
			retval = verify_area(VERIFY_WRITE, ibargp->ib_buf, 1);
			if (retval)
			{
				GIVE_UP(retval);
			}
			retval = ibAPrsp(device, ibargp->ib_arg, &c);
			put_user( c, ibargp->ib_buf );
			break;
		case DVWRT:	// XXX unnecessary, should be in user space lib

			/* Check read access to buffer */
			retval = verify_area(VERIFY_READ, ibargp->ib_buf, ibargp->ib_cnt);
			if (retval)
				GIVE_UP(retval);

			/* Write buffer loads till we empty the user supplied buffer */
			userbuf = ibargp->ib_buf;
			remain = ibargp->ib_cnt;
			while (remain > 0  && !(ibstatus(device) & (TIMO)))
			{
				int send_eoi;
				send_eoi = device->buffer_length <= remain && device->send_eoi;
				copy_from_user(device->buffer, userbuf, (device->buffer_length < remain) ?
					device->buffer_length : remain );
				ret = dvwrt(device, ibargp->ib_arg, device->buffer, (device->buffer_length < remain) ?
					device->buffer_length : remain, send_eoi);
				if(ret < 0)
				{
					retval = -EIO;
					break;
				}
				remain -= ret;
				userbuf += ret;
			}
			ibargp->ib_ibcnt = ibargp->ib_cnt - remain;

			break;

		/* special configuration options */
		case CFCBASE:
			osChngBase(device, ibargp->ib_arg);
			break;
		case CFCIRQ:
			osChngIRQ(device, ibargp->ib_arg);
			break;
		case CFCDMA:
			osChngDMA(device, ibargp->ib_arg);
			break;
		default:
			retval = -ENOTTY;
			break;
	}

	// return status bits
	ibargp->ib_ibsta = ibstatus(device);
	if(retval)
		ibargp->ib_ibsta |= ERR;
	else
		ibargp->ib_ibsta &= ~ERR;
	if(end_flag)
		ibargp->ib_ibsta |= END;
	else
		ibargp->ib_ibsta &= ~END;
	// XXX io is always complete since we don't support asynchronous transfers yet
	ibargp->ib_ibsta |= CMPL;

	copy_to_user((ibarg_t *) arg, (ibarg_t *) ibargp , sizeof(ibarg_t));

	GIVE_UP(retval);
}

int board_type_ioctl(gpib_device_t *device, unsigned long arg)
{
	struct list_head *list_ptr;
	board_type_ioctl_t board;
	int retval;

	retval = copy_from_user(&board, (void*)arg, sizeof(board_type_ioctl_t));
	if(retval)
	{
		return retval;
	}

	device->private_data = NULL;
	device->status = 0;
	device->ibbase = 0;
	device->ibirq = 0;
	device->ibdma = 0;
	device->send_eoi = 1;
	device->master = 1;
	device->online = 0;
	init_waitqueue_head(&device->wait);
	init_MUTEX(&device->mutex);
	spin_lock_init(&device->spinlock);
	init_timer(&device->timer);
	device->interface = NULL;

	device->buffer_length = 0x1000;
	if(device->buffer)
		vfree(device->buffer);
	device->buffer = vmalloc(device->buffer_length);
	if(device->buffer == NULL)
		return -ENOMEM;

	for(list_ptr = registered_drivers.next; list_ptr != &registered_drivers; list_ptr = list_ptr->next)
	{
		gpib_interface_t *interface;

		interface = list_entry(list_ptr, gpib_interface_t, list);
		if(strcmp(interface->name, board.name) == 0)
		{
			device->interface = interface;
			return 0;
		}
	}

	return -EINVAL;
}

int read_ioctl(gpib_device_t *device, unsigned long arg)
{
	read_write_ioctl_t read_cmd;
	uint8_t *userbuf;
	unsigned long remain;
	int end_flag = 0;
	int retval;
	ssize_t ret;

	retval = copy_from_user(&read_cmd, (void*) arg, sizeof(read_cmd));
	if (retval)
		return -EFAULT;

	/* Check write access to buffer */
	retval = verify_area(VERIFY_WRITE, read_cmd.buffer, read_cmd.count);
	if (retval)
		return -EFAULT;

	/* Read buffer loads till we fill the user supplied buffer */
	userbuf = read_cmd.buffer;
	remain = read_cmd.count;
	while(remain > 0 && end_flag == 0)
	{
		ret = ibrd(device, device->buffer, (device->buffer_length < remain) ? device->buffer_length :
			remain, &end_flag);
		if(ret < 0)
		{
			retval = -EIO;
			break;
		}
		copy_to_user(userbuf, device->buffer, ret);
		remain -= ret;
		userbuf += ret;
	}
	read_cmd.count -= remain;

	retval = copy_to_user((void*) arg, &read_cmd, sizeof(read_cmd));
	if(retval) return -EFAULT;

	return 0;
}

int command_ioctl(gpib_device_t *device, unsigned long arg)
{
	read_write_ioctl_t cmd;
	uint8_t *userbuf;
	unsigned long remain;
	int retval;
	ssize_t ret;

	retval = copy_from_user(&cmd, (void*) arg, sizeof(cmd));
	if (retval)
		return -EFAULT;

	/* Check read access to buffer */
	retval = verify_area(VERIFY_READ, cmd.buffer, cmd.count);
	if (retval)
		return -EFAULT;

	/* Write buffer loads till we empty the user supplied buffer */
	userbuf = cmd.buffer;
	remain = cmd.count;
	while (remain > 0 && !(ibstatus(device) & (TIMO)))
	{
		copy_from_user(device->buffer, userbuf, (device->buffer_length < remain) ?
			device->buffer_length : remain );
		ret = ibcmd(device, device->buffer, (device->buffer_length < remain) ?
			device->buffer_length : remain );
		if(ret < 0)
		{
			retval = -EIO;
			break;
		}
		remain -= ret;
		userbuf += ret;
	}

	cmd.count -= remain;

	retval = copy_to_user((void*) arg, &cmd, sizeof(cmd));
	if(retval) return -EFAULT;

	return 0;
}













