/***************************************************************************
                          init.c  -  description
                             -------------------
 board specific initialization stuff

    begin                : Dec 2001
    copyright            : (C) 2001 by Frank Mori Hess, and unknown author(s)
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

#include "board.h"
#include <linux/ioport.h>
#include <linux/sched.h>
#include <asm/dma.h>

unsigned long ibbase = IBBASE;
uint8 ibirq = IBIRQ;
uint8 ibdma = IBDMA;
unsigned long remapped_ibbase = 0;

// flags to indicate if various resources have been allocated
static unsigned int ioports_allocated = 0, iomem_allocated = 0,
	irq_allocated = 0, dma_allocated = 0, pcmcia_initialized = 0;

// nec7210 has 8 registers
static const int nec7210_num_registers = 8;
// size of modbus pci memory io region
static const int iomem_size = 0x2000;

void board_reset(void)
{
#ifdef MODBUS_PCI
	GPIBout(0x20, 0xff); /* enable controller mode */
#endif

	GPIBout(AUXMR, AUX_CR);                     /* 7210 chip reset */
        /*GPIBout(INTRT, 1);*/

	GPIBin(CPTR);                           /* clear registers by reading */
	GPIBin(ISR1);
	GPIBin(ISR2);

	GPIBout(IMR1, 0);                           /* disable all interrupts */
	GPIBout(IMR2, 0);
	GPIBout(SPMR, 0);

	GPIBout(ADR,(PAD & LOMASK));                /* set GPIB address; MTA=PAD|100, MLA=PAD|040 */
#if (SAD)
	GPIBout(ADR, HR_ARS | (SAD & LOMASK));      /* enable secondary addressing */
	GPIBout(ADMR, HR_TRM1 | HR_TRM0 | HR_ADM1);
#else
	GPIBout(ADR, HR_ARS | HR_DT | HR_DL);       /* disable secondary addressing */
	GPIBout(ADMR, HR_TRM1 | HR_TRM0 | HR_ADM0);
#endif

	GPIBout(EOSR, 0);
	GPIBout(AUXMR, ICR | 5);                    /* set internal counter register N= 8 */
	GPIBout(AUXMR, PPR | HR_PPU);               /* parallel poll unconfigure */
	GPIBout(AUXMR, auxrabits);

	GPIBout(AUXMR, AUXRB | 0);                  /* set INT pin to active high */
	GPIBout(AUXMR, AUXRE | 0);
}

int board_attach(void)
{
	unsigned int i, err;
	int isr_flags = 0;

	// nothing is allocated yet
	ioports_allocated = iomem_allocated = irq_allocated = 
		dma_allocated = pcmcia_initialized = 0;
#ifdef INES_PCMCIA
	pcmcia_init_module();
	pcmcia_initialized = 1;
#endif
#if defined(MODBUS_PCI) || defined(INES_PCI)
   bd_PCIInfo();
#endif

#ifdef NIPCIIa
	switch( ibbase ){

		case 0x02e1:
		case 0x22e1:
		case 0x42e1:
		case 0x62e1:
			break;
	   default:
	     printk("PCIIa base range invalid, must be one of [0246]2e1 is %lx \n", ibbase);
             return(0);
           break;
	}

        if( ibirq < 2 || ibirq > 7 ){
	  printk("Illegal Interrupt Level \n");
          return(0);
	}
#endif
#ifdef MODBUS_PCI
	// modbus uses io memory instead of ioports
	if(check_mem_region(ibbase, iomem_size))
	{
		printk("gpib: memory io region already in use");
		return -1;
	}
	request_mem_region(ibbase, iomem_size, "gpib");
	remapped_ibbase = (unsigned long) ioremap(ibbase, iomem_size);
	iomem_allocated = 1;
#else
	/* nec7210 registers can be spread out to varying degrees, so allocate
	 * each one seperately.  Some boards have extra registers that I haven't
	 * bothered to reserve.  fmhess */
	err = 0;
	for(i = 0; i < nec7210_num_registers; i++)
	{
		if(check_region(ibbase + i * NEC7210_REG_OFFSET, 1))
			err++;
	}
	if(err)
	{
		printk("gpib: ioports are already in use");
		return -1;
	}
	for(i = 0; i < nec7210_num_registers; i++)
	{
		request_region(ibbase + i * NEC7210_REG_OFFSET, 1, "gpib");
	}
	ioports_allocated = 1;
#endif
	// install interrupt handler
#if USEINTS
#if defined(MODBUS_PCI) || defined(INES_PCI)
	isr_flags |= SA_SHIRQ;
#endif
	if( request_irq(ibirq, ibintr, isr_flags, "gpib", NULL))
	{
		printk("gpib: can't request IRQ %d\n", ibirq);
		return -1;
	}
	irq_allocated = 1;
#if defined(MODBUS_PCI) || defined(INES_PCI)
	pci_EnableIRQ();
#endif
#endif
	// request isa dma channel
#if DMAOP
	if( request_dma( ibdma, "gpib" ) )
	{
		printk("gpib: can't request DMA %d\n",ibdma );
		return -1;
	}
	dma_allocated = 1;
#endif
	board_reset();

	GPIBout(AUXMR, AUX_PON);

	return 0;
}

void board_detach(void)
{
	int i;
	if(dma_allocated)
	{
		free_dma(ibdma);
		dma_allocated = 0;
	}
	if(irq_allocated)
	{
		free_irq(ibirq, 0);
		irq_allocated = 0;
	}
	if(ioports_allocated || iomem_allocated)
	{
		board_reset();
#if defined(MODBUS_PCI) || defined(INES_PCI)
		pci_DisableIRQ();
#endif
	}
	if(ioports_allocated)
	{
		for(i = 0; i < nec7210_num_registers; i++)
			release_region(ibbase + i * NEC7210_REG_OFFSET, 1);
		ioports_allocated = 0;
	}
	if(iomem_allocated)
	{
		iounmap((void*) remapped_ibbase);
		release_mem_region(ibbase, iomem_size);
		iomem_allocated = 0;
	}
	if(pcmcia_initialized)
	{
#ifdef INES_PCMCIA
		pcmcia_cleanup_module();
#endif
		pcmcia_initialized = 0;
	}
}












