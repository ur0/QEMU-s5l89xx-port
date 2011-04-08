/*
 * Synopsys DesignWareCore for USB OTG.
 *
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "sysbus.h"
#include "qemu-common.h"
#include "qemu-timer.h"
#include "usb.h"
#include "net.h"
#include "irq.h"
#include "hw.h"
#include "usb_synopsys.h"
#include "tcp_usb.h"

#define DEVICE_NAME		"usb_synopsys"

// Maximums supported by OIB
#define USB_NUM_ENDPOINTS	8
#define USB_NUM_FIFOS		16

#define RX_FIFO_DEPTH				0x1C0
#define TX_FIFO_DEPTH				0x1C0
#define TX_FIFO_STARTADDR			0x200
#define PERIODIC_TX_FIFO_STARTADDR	0x21B
#define PERIODIC_TX_FIFO_DEPTH		0x100

// Registers
#define GOTGCTL		0x0
#define GOTGINT		0x4
#define GAHBCFG		0x8
#define GUSBCFG		0xC
#define GRSTCTL		0x10
#define GINTSTS		0x14
#define GINTMSK		0x18
#define GRXFSIZ		0x24
#define GNPTXFSIZ	0x28
#define GNPTXFSTS	0x2C
#define GHWCFG1		0x44
#define GHWCFG2		0x48
#define GHWCFG3		0x4C
#define GHWCFG4		0x50
#define DIEPTXF(x)	(0x100 + (4*(x)))
#define DCFG		0x800
#define DCTL		0x804
#define DSTS		0x808
#define DIEPMSK		0x810
#define DOEPMSK		0x814
#define DAINTSTS	0x818
#define DAINTMSK	0x81C
#define DTKNQR1		0x820
#define DTKNQR2		0x824
#define DTKNQR3		0x830
#define DTKNQR4		0x834
#define USB_INREGS	0x900
#define USB_OUTREGS	0xB00
#define USB_EPREGS_SIZE 0x200

#define PCGCCTL     0xE00

#define PCGCCTL_ONOFF_MASK  3   // bits 0, 1
#define PCGCCTL_ON          0
#define PCGCCTL_OFF         1

#define GOTGCTL_BSESSIONVALID (1 << 19)
#define GOTGCTL_SESSIONREQUEST (1 << 1)

#define GAHBCFG_DMAEN (1 << 5)
#define GAHBCFG_BSTLEN_SINGLE (0 << 1)
#define GAHBCFG_BSTLEN_INCR (1 << 1)
#define GAHBCFG_BSTLEN_INCR4 (3 << 1)
#define GAHBCFG_BSTLEN_INCR8 (5 << 1)
#define GAHBCFG_BSTLEN_INCR16 (7 << 1)
#define GAHBCFG_MASKINT 0x1

#define GUSBCFG_TURNAROUND_MASK 0xF
#define GUSBCFG_TURNAROUND_SHIFT 10
#define GUSBCFG_HNPENABLE (1 << 9)
#define GUSBCFG_SRPENABLE (1 << 8)
#define GUSBCFG_PHYIF16BIT (1 << 3)
#define USB_UNKNOWNREG1_START 0x1708

#define GHWCFG2_TKNDEPTH_SHIFT	26
#define GHWCFG2_TKNDEPTH_MASK	0xF
#define GHWCFG2_NUM_ENDPOINTS_SHIFT	10
#define GHWCFG2_NUM_ENDPOINTS_MASK	0xf

#define GHWCFG4_DED_FIFO_EN			(1 << 25)

#define GRSTCTL_AHBIDLE			(1 << 31)
#define GRSTCTL_TXFFLUSH		(1 << 5)
#define GRSTCTL_TXFFNUM_SHIFT	6
#define GRSTCTL_TXFFNUM_MASK	0x1f
#define GRSTCTL_CORESOFTRESET	0x1
#define GRSTCTL_TKNFLUSH		3

#define GINTMSK_NONE        0x0
#define GINTMSK_OTG         (1 << 2)
#define GINTMSK_SOF         (1 << 3)
#define GINTMSK_GINNAKEFF   (1 << 6)
#define GINTMSK_GOUTNAKEFF  (1 << 7)
#define GINTMSK_SUSPEND     (1 << 11)
#define GINTMSK_RESET       (1 << 12)
#define GINTMSK_ENUMDONE    (1 << 13)
#define GINTMSK_EPMIS       (1 << 17)
#define GINTMSK_INEP        (1 << 18)
#define GINTMSK_OEP         (1 << 19)
#define GINTMSK_DISCONNECT  (1 << 29)
#define GINTMSK_RESUME      (1 << 31)

#define GOTGINT_SESENDDET	(1 << 2)

#define FIFO_DEPTH_SHIFT 16

#define GNPTXFSTS_GET_TXQSPCAVAIL(x) GET_BITS(x, 16, 8)

#define GHWCFG4_DED_FIFO_EN         (1 << 25)

#define DAINT_ALL                   0xFFFFFFFF
#define DAINT_NONE                  0
#define DAINT_OUT_SHIFT             16
#define DAINT_IN_SHIFT              0

#define DCTL_SFTDISCONNECT			0x2
#define DCTL_PROGRAMDONE			(1 << 11)
#define DCTL_CGOUTNAK				(1 << 10)
#define DCTL_SGOUTNAK				(1 << 9)
#define DCTL_CGNPINNAK				(1 << 8)
#define DCTL_SGNPINNAK				(1 << 7)

#define DSTS_GET_SPEED(x) GET_BITS(x, 1, 2)

#define DCFG_NZSTSOUTHSHK           (1 << 2)
#define DCFG_EPMSCNT                (1 << 18)
#define DCFG_HISPEED                0x0
#define DCFG_FULLSPEED              0x1
#define DCFG_DEVICEADDR_UNSHIFTED_MASK 0x7F
#define DCFG_DEVICEADDR_SHIFT 4
#define DCFG_DEVICEADDRMSK (DCFG_DEVICEADDR_UNSHIFTED_MASK << DCFG_DEVICEADDR_SHIFT)
#define DCFG_ACTIVE_EP_COUNT_MASK	0x1f
#define DCFG_ACTIVE_EP_COUNT_SHIFT	18

#define DOEPTSIZ0_SUPCNT_MASK 0x3
#define DOEPTSIZ0_SUPCNT_SHIFT 29
#define DOEPTSIZ0_PKTCNT_MASK 0x1
#define DEPTSIZ0_XFERSIZ_MASK 0x7F
#define DIEPTSIZ_MC_MASK 0x3
#define DIEPTSIZ_MC_SHIFT 29
#define DEPTSIZ_PKTCNT_MASK 0x3FF
#define DEPTSIZ_PKTCNT_SHIFT 19
#define DEPTSIZ_XFERSIZ_MASK 0x1FFFF

// ENDPOINT_DIRECTIONS register has two bits per endpoint. 0, 1 for endpoint 0. 1, 2 for end point 1, etc.
#define USB_EP_DIRECTION(ep) (USBDirection)(2-((GET_REG(USB + GHWCFG1) >> ((ep) * 2)) & 0x3))
#define USB_ENDPOINT_DIRECTIONS_BIDIR 0
#define USB_ENDPOINT_DIRECTIONS_IN 1
#define USB_ENDPOINT_DIRECTIONS_OUT 2

#define USB_START_DELAYUS 10000
#define USB_SFTDISCONNECT_DELAYUS 4000
#define USB_ONOFFSTART_DELAYUS 100
#define USB_RESETWAITFINISH_DELAYUS 1000
#define USB_SFTCONNECT_DELAYUS 250
#define USB_PROGRAMDONE_DELAYUS 10

#define USB_EPCON_ENABLE		(1 << 31)
#define USB_EPCON_DISABLE		(1 << 30)
#define USB_EPCON_SETD0PID		(1 << 28)
#define USB_EPCON_SETNAK		(1 << 27)
#define USB_EPCON_CLEARNAK		(1 << 26)
#define USB_EPCON_TXFNUM_MASK	0xf
#define USB_EPCON_TXFNUM_SHIFT	22
#define USB_EPCON_STALL			(1 << 21)
#define USB_EPCON_TYPE_MASK		0x3
#define USB_EPCON_TYPE_SHIFT	18
#define USB_EPCON_NAKSTS		(1 << 17)
#define USB_EPCON_ACTIVE		(1 << 15)
#define USB_EPCON_NEXTEP_MASK	0xF
#define USB_EPCON_NEXTEP_SHIFT	11
#define USB_EPCON_MPS_MASK		0x7FF

#define USB_EPINT_INEPNakEff 0x40
#define USB_EPINT_INTknEPMis 0x20
#define USB_EPINT_INTknTXFEmp 0x10
#define USB_EPINT_TimeOUT 0x8
#define USB_EPINT_AHBErr 0x4
#define USB_EPINT_EPDisbld 0x2
#define USB_EPINT_XferCompl 0x1

#define USB_EPINT_Back2BackSetup (1 << 6)
#define USB_EPINT_OUTTknEPDis 0x10
#define USB_EPINT_SetUp 0x8
#define USB_EPINT_EpDisbld 0x1
#define USB_EPINT_NONE 0
#define USB_EPINT_ALL 0xFFFFFFFF

#define USB_2_0 0x0200

#define USB_HIGHSPEED 0
#define USB_FULLSPEED 1
#define USB_LOWSPEED 2
#define USB_FULLSPEED_48_MHZ 3

#define USB_CONTROLEP 0

typedef struct _synopsys_usb_ep_state
{
	uint32_t control;
	uint32_t tx_size;
	uint32_t fifo;
	uint32_t interrupt_status;

	target_phys_addr_t dma_address;
	target_phys_addr_t dma_buffer;

} synopsys_usb_ep_state;

typedef struct _synopsys_usb_state
{
	SysBusDevice busdev;
	qemu_irq irq;

	char *server_host;
	uint32_t server_port;
	tcp_usb_state_t tcp_state;

	uint32_t ghwcfg1;
	uint32_t ghwcfg2;
	uint32_t ghwcfg3;
	uint32_t ghwcfg4;

	uint32_t grxfsiz;
	uint32_t gnptxfsiz;

	uint32_t grstctl;
	uint32_t gintmsk;
	uint32_t gintsts;

	uint32_t dptxfsiz[USB_NUM_FIFOS];

	uint32_t dcfg;
	uint32_t dsts;
	uint32_t daintmsk;
	uint32_t daintsts;

	synopsys_usb_ep_state in_eps[USB_NUM_ENDPOINTS];
	synopsys_usb_ep_state out_eps[USB_NUM_ENDPOINTS];

	uint8_t fifos[0x100 * (USB_NUM_FIFOS+1)];

} synopsys_usb_state;

static inline size_t synopsys_usb_tx_fifo_start(synopsys_usb_state *_state, uint32_t _fifo)
{
	if(_fifo == 0)
		return _state->gnptxfsiz >> 16;
	else
		return _state->dptxfsiz[_fifo-1] >> 16;
}

static inline size_t synopsys_usb_tx_fifo_size(synopsys_usb_state *_state, uint32_t _fifo)
{
	if(_fifo == 0)
		return _state->gnptxfsiz & 0xFFFF;
	else
		return _state->dptxfsiz[_fifo-1] & 0xFFFF;
}

static void synopsys_usb_update_irq(synopsys_usb_state *_state)
{
	_state->daintsts = 0;
	_state->gintsts &= ~(GINTMSK_OEP | GINTMSK_INEP);

	int i;
	for(i = 0; i < USB_NUM_ENDPOINTS; i++)
	{
		if(_state->out_eps[i].interrupt_status)
		{
			_state->daintsts |= 1 << (i+DAINT_OUT_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_OUT_SHIFT)))
				_state->gintsts |= GINTMSK_OEP;
		}

		if(_state->in_eps[i].interrupt_status)
		{
			_state->daintsts |= 1 << (i+DAINT_IN_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_IN_SHIFT)))
				_state->gintsts |= GINTMSK_OEP;
		}
	}

	if(_state->gintmsk & _state->gintsts)
		qemu_irq_raise(_state->irq);
	else
		qemu_irq_lower(_state->irq);
}

static void synopsys_usb_update_ep(synopsys_usb_state *_state, synopsys_usb_ep_state *_ep)
{
	if(_ep->control & USB_EPCON_SETNAK)
	{
		_ep->control |= USB_EPCON_NAKSTS;
		_ep->interrupt_status |= USB_EPINT_INEPNakEff;
		_ep->control &=~ USB_EPCON_SETNAK;
	}

	if(_ep->control & USB_EPCON_DISABLE)
	{
		_ep->interrupt_status |= USB_EPINT_EPDisbld;
		_ep->control &=~ (USB_EPCON_DISABLE | USB_EPCON_ENABLE);
	}
}

static void synopsys_usb_in_ep_done(tcp_usb_state_t *_tcp_state, uint8_t _ep, size_t _amt, void *_arg)
{
	synopsys_usb_state *state = _arg;
	synopsys_usb_ep_state *eps = &state->in_eps[_ep];

	size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
	eps->tx_size = (eps->tx_size &~ DEPTSIZ_XFERSIZ_MASK)
					| ((sz-_amt) & DEPTSIZ_XFERSIZ_MASK);
	eps->control &=~ USB_EPCON_ENABLE;
	eps->interrupt_status |= USB_EPINT_XferCompl;

	fprintf(stderr, "usb_synopsys: IN transfer complete!\n");

	synopsys_usb_update_irq(state);
}

static void synopsys_usb_update_in_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->in_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
	{
		// Do IN transfer!
		
		size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
		size_t amtDone = sz;

		if(eps->fifo >= USB_NUM_FIFOS)
			hw_error("usb_synopsys: USB transfer on non-existant FIFO %d!\n", eps->fifo);

		size_t txfz = synopsys_usb_tx_fifo_size(_state, eps->fifo);
		if(amtDone > txfz)
			amtDone = txfz;

		size_t txfs = synopsys_usb_tx_fifo_start(_state, eps->fifo);
		if(txfs + txfz > sizeof(_state->fifos))
			hw_error("usb_synopsys: USB transfer would overflow FIFO buffer!\n");

		if(eps->dma_address)
		{
			cpu_physical_memory_read(eps->dma_address, &_state->fifos[txfs], amtDone);
			eps->dma_address += amtDone;
		}

		if(!tcp_usb_okay(&_state->tcp_state))
			tcp_usb_send(&_state->tcp_state, _ep, (char*)&_state->fifos[txfs], amtDone,
					synopsys_usb_in_ep_done, _state);
		else
			synopsys_usb_in_ep_done(&_state->tcp_state, _ep, amtDone, _state);
	}
}

static void synopsys_usb_out_ep_done(tcp_usb_state_t *_tcp_state, uint8_t _ep, size_t _amt, void *_arg)
{
	synopsys_usb_state *state = _arg;
	synopsys_usb_ep_state *eps = &state->out_eps[_ep];

	size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;

	if(eps->dma_address)
	{
		cpu_physical_memory_write(eps->dma_address, state->fifos, _amt);
		eps->dma_address += _amt;
	}

	eps->tx_size = (eps->tx_size &~ DEPTSIZ_XFERSIZ_MASK)
					| ((sz-_amt) & DEPTSIZ_XFERSIZ_MASK);
	eps->control &=~ USB_EPCON_ENABLE;
	eps->interrupt_status |= USB_EPINT_XferCompl;

	fprintf(stderr, "usb_synopsys: OUT transfer complete!\n");

	synopsys_usb_update_irq(state);
}

static void synopsys_usb_update_out_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->out_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
	{
		// Do OUT transfer!
		
		if(tcp_usb_okay(&_state->tcp_state))
		{
			fprintf(stderr, "usb_synopsys: OUT transfer queued!\n");
			return;
		}
			
		size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
		size_t amtDone = sz;
		size_t rxfz = _state->grxfsiz;
		if(amtDone > rxfz)
			amtDone = rxfz;

		if(rxfz > sizeof(_state->fifos))
			hw_error("usb_synopsys: USB transfer would overflow FIFO buffer!\n");

		tcp_usb_recv(&_state->tcp_state, _ep, (char*)_state->fifos, amtDone,
				synopsys_usb_out_ep_done, _state);
	}
}

static uint32_t synopsys_usb_in_ep_read(synopsys_usb_state *_state, uint8_t _ep, target_phys_addr_t _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Tried to read from disabled EP %d.\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->in_eps[_ep].control;

    case 0x08:
        return _state->in_eps[_ep].interrupt_status;

    case 0x10:
        return _state->in_eps[_ep].tx_size;

    case 0x14:
        return _state->in_eps[_ep].dma_address;

    case 0x1C:
        return _state->in_eps[_ep].dma_buffer;

    default:
        hw_error("usb_synopsys: bad ep read offset 0x" TARGET_FMT_plx "\n", _addr);
		break;
    }

	return 0;
}

static uint32_t synopsys_usb_out_ep_read(synopsys_usb_state *_state, int _ep, target_phys_addr_t _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Tried to read from disabled EP %d.\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->out_eps[_ep].control;

    case 0x08:
        return _state->out_eps[_ep].interrupt_status;

    case 0x10:
        return _state->out_eps[_ep].tx_size;

    case 0x14:
        return _state->out_eps[_ep].dma_address;

    case 0x1C:
        return _state->out_eps[_ep].dma_buffer;

    default:
        hw_error("usb_synopsys: bad ep read offset 0x" TARGET_FMT_plx "\n", _addr);
		break;
    }

	return 0;
}

static uint32_t synopsys_usb_read(void *_arg, target_phys_addr_t _addr)
{
	synopsys_usb_state *state = _arg;

	switch(_addr)
	{
	case GRSTCTL:
		return state->grstctl;

	case GHWCFG1:
		return state->ghwcfg1;

	case GHWCFG2:
		return state->ghwcfg2;

	case GHWCFG3:
		return state->ghwcfg3;

	case GHWCFG4:
		return state->ghwcfg4;

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		return synopsys_usb_in_ep_read(state, _addr >> 5, _addr & 0x1f);

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		return synopsys_usb_out_ep_read(state, _addr >> 5, _addr & 0x1f);
	}

	return 0;
}

static void synopsys_usb_in_ep_write(synopsys_usb_state *_state, int _ep, target_phys_addr_t _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Wrote to disabled EP %d.\n", _ep);
		return;
	}

    switch (_addr)
	{
    case 0x00:
		_state->in_eps[_ep].control = _val;
		synopsys_usb_update_in_ep(_state, _ep);
		return;

    case 0x08:
        _state->in_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->in_eps[_ep].tx_size = _val;
		return;

    case 0x14:
        _state->in_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->in_eps[_ep].dma_buffer = _val;
		return;

    default:
        hw_error("usb_synopsys: bad ep write offset 0x" TARGET_FMT_plx "\n", _addr);
		break;
    }
}

static void synopsys_usb_out_ep_write(synopsys_usb_state *_state, int _ep, target_phys_addr_t _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Wrote to disabled EP %d.\n", _ep);
		return;
	}

    switch (_addr)
	{
	case 0x00:
        _state->out_eps[_ep].control = _val;
		synopsys_usb_update_out_ep(_state, _ep);
		return;

    case 0x08:
        _state->out_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->out_eps[_ep].tx_size = _val;
		return;

    case 0x14:
        _state->out_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->out_eps[_ep].dma_buffer = _val;
		return;

    default:
        hw_error("usb_synopsys: bad ep write offset 0x" TARGET_FMT_plx "\n", _addr);
		break;
    }
}

static void synopsys_usb_write(void *_arg, target_phys_addr_t _addr, uint32_t _val)
{
	synopsys_usb_state *state = _arg;

	switch(_addr)
	{
	case GRSTCTL:
		if(_val & GRSTCTL_CORESOFTRESET)
		{
			// TODO: Do some reset stuff?
			state->grstctl &= ~GRSTCTL_CORESOFTRESET;
			state->grstctl |= GRSTCTL_AHBIDLE;
		}
		else if(_val == 0)
			state->grstctl = _val;

		return;

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		synopsys_usb_in_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		synopsys_usb_out_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;
	}
}

static CPUReadMemoryFunc *synopsys_usb_readfn[] = {
    synopsys_usb_read,
    synopsys_usb_read,
    synopsys_usb_read
};

static CPUWriteMemoryFunc *synopsys_usb_writefn[] = {
    synopsys_usb_write,
    synopsys_usb_write,
    synopsys_usb_write
};

static void synopsys_usb_initial_reset(DeviceState *dev)
{
	synopsys_usb_state *state =
		FROM_SYSBUS(synopsys_usb_state, sysbus_from_qdev(dev));

	// Values from iPhone 2G.
	state->ghwcfg1 = 0;
	state->ghwcfg2 = 0x7a8f60d0;
	state->ghwcfg3 = 0x082000e8;
	state->ghwcfg4 = 0x01f08024;

	state->gintmsk = 0;
	state->daintmsk = 0;

	synopsys_usb_update_irq(state);
}

static int synopsys_usb_init(SysBusDevice *dev)
{
	synopsys_usb_state *state =
		FROM_SYSBUS(synopsys_usb_state, dev);

	tcp_usb_init(&state->tcp_state);
	if(state->server_host)
	{
		printf("Connecting to USB server at %s:%d...\n",
				state->server_host, state->server_port);

		if(tcp_usb_connect(&state->tcp_state, state->server_host, state->server_port))
			hw_error("Failed to connect to USB server.\n");
	}

    int iomemtype = cpu_register_io_memory(synopsys_usb_readfn,
                               synopsys_usb_writefn, state, DEVICE_LITTLE_ENDIAN);

    sysbus_init_mmio(dev, 0x100000, iomemtype);
    sysbus_init_irq(dev, &state->irq);

	synopsys_usb_initial_reset(&state->busdev.qdev);
	return 0;
}

static SysBusDeviceInfo synopsys_usb_info = {
    .init = synopsys_usb_init,
    .qdev.name  = DEVICE_NAME,
    .qdev.size  = sizeof(synopsys_usb_state),
    .qdev.reset = synopsys_usb_initial_reset,
    .qdev.props = (Property[]) {
		DEFINE_PROP_STRING("host", synopsys_usb_state, server_host),
		DEFINE_PROP_UINT32("port", synopsys_usb_state, server_port, 7642),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void synopsys_usb_register(void)
{
    sysbus_register_withprop(&synopsys_usb_info);
}
device_init(synopsys_usb_register);

// Helper for adding to a machine
void register_synopsys_usb(target_phys_addr_t _addr, qemu_irq _irq)
{
    DeviceState *dev = qdev_create(NULL, DEVICE_NAME);
	qdev_init_nofail(dev);

	SysBusDevice *sdev = sysbus_from_qdev(dev);
    sysbus_mmio_map(sdev, 0, _addr);
    sysbus_connect_irq(sdev, 0, _irq);
}
