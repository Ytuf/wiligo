

#include "rpDMA.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/platform_defs.h"
#include <cassert>


rpDMA::~rpDMA()
{
	if (m_bClaimed) {
		dma_channel_unclaim(m_iDMAChannel);
	}
}

void rpDMA::init(int iChannel)
{
	dma_channel_claim(iChannel);
	m_iDMAChannel = iChannel;
	m_bClaimed = true;
}

void rpDMA::initClaimFreeDMAChannel()
{
	m_iDMAChannel = dma_claim_unused_channel(true);
	m_bClaimed = true;
}

void rpDMA::setupTransfer(void * pDestinationAddress,
	void * pSourceAddress,
	bool bAutoIncrementDestination,
	bool bAutoIncrementSource,
	rpDMATransferDataSize eTransferSize,
	int iCount)
{

	dma_channel_hw_t * pDMA = dma_channel_hw_addr(m_iDMAChannel);

	dma_channel_set_read_addr(m_iDMAChannel, pSourceAddress, false);
	dma_channel_set_write_addr(m_iDMAChannel, pDestinationAddress, false);
	dma_channel_set_trans_count(m_iDMAChannel, iCount, false);

	if (bAutoIncrementDestination)
		pDMA->ctrl_trig |= (1 << 5);
	else
		pDMA->ctrl_trig &= ~(1 << 5);

	if (bAutoIncrementSource)
		pDMA->ctrl_trig |= (1 << 4);
	else
		pDMA->ctrl_trig &= ~(1 << 4);

	pDMA->ctrl_trig &= ~(0x3 << 2);
	switch (eTransferSize)
	{
		case rpDMATransferDataSize::byte:
			break;
		case rpDMATransferDataSize::halfword:
			pDMA->ctrl_trig |= (1 << 2);
			break;
		case rpDMATransferDataSize::word:
			pDMA->ctrl_trig |= (2 << 2);
			break;
	}


}

void rpDMA::enableCRCCalc()
{
	// mode
 //* 0x0 | Calculate a CRC-32 (IEEE802.3 polynomial)
 //* 0x1 | Calculate a CRC-32 (IEEE802.3 polynomial) with bit reversed data
	dma_sniffer_enable(m_iDMAChannel,0,true);
}

unsigned int rpDMA::getCRCValue()
{
	unsigned int * pValue = (unsigned int * ) (DMA_BASE + DMA_SNIFF_DATA_OFFSET);
	return *pValue;
}

void rpDMA::start()
{
	dma_channel_hw_t * pDMA = dma_channel_hw_addr(m_iDMAChannel);
	pDMA->ctrl_trig |= 0x1; // enable
	dma_channel_start(m_iDMAChannel);
}

void rpDMA::abort()
{
	dma_channel_abort(m_iDMAChannel);
}


bool rpDMA::isComplete()
{
	return !dma_channel_is_busy(m_iDMAChannel);
}


void rpDMA::setupPeripheralRequest(rpDMAPeripheralRequest ePeripheral)
{
	dma_channel_hw_t * pDMA = dma_channel_hw_addr(m_iDMAChannel);

	unsigned int iValue = pDMA->ctrl_trig;
	iValue &= ~(0x3F << 15); // Clear TREQBITS

#ifdef PICO_RP2350
	switch (ePeripheral) {
		case rpDMAPeripheralRequest::pio0Tx0:
			iValue |= 0 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx1:
			iValue |= 1 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx2:
			iValue |= 2 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx3:
			iValue |= 3 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx0:
			iValue |= 4 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx1:
			iValue |= 5 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx2:
			iValue |= 6 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx3:
			iValue |= 7 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx0:
			iValue |= 8 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx1:
			iValue |= 9 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx2:
			iValue |= 10 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx3:
			iValue |= 11 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx0:
			iValue |= 12 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx1:
			iValue |= 13 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx2:
			iValue |= 14 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx3:
			iValue |= 15 << 15;
			break;

		case rpDMAPeripheralRequest::pio2Tx0:
			iValue |= 16 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Tx1:
			iValue |= 17 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Tx2:
			iValue |= 18 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Tx3:
			iValue |= 19 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Rx0:
			iValue |= 20 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Rx1:
			iValue |= 21 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Rx2:
			iValue |= 22 << 15;
			break;
		case rpDMAPeripheralRequest::pio2Rx3:
			iValue |= 23 << 15;
			break;
		case rpDMAPeripheralRequest::uart0Tx:
			iValue |= 28 << 15;
			break;
		case rpDMAPeripheralRequest::uart0Rx:
			iValue |= 29 << 15;
			break;
		case rpDMAPeripheralRequest::uart1Tx:
			iValue |= 30 << 15;
			break;
		case rpDMAPeripheralRequest::uart1Rx:
			iValue |= 31 << 15;
			break;

		default:
			assert(0); //NEED TO IMPLEMENT THE OTHER DMA DREQS
			break;
	}
	//DREQ_SPI0_TX
#else
	switch (ePeripheral)
	{
		case rpDMAPeripheralRequest::pio0Tx0:
		iValue |= 0 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx1:
		iValue |= 1 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx2:
		iValue |= 2 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Tx3:
		iValue |= 3 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx0:
		iValue |= 4 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx1:
		iValue |= 5 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx2:
		iValue |= 6 << 15;
			break;
		case rpDMAPeripheralRequest::pio0Rx3:
		iValue |= 7 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx0:
		iValue |= 8 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx1:
		iValue |= 9 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx2:
		iValue |= 10 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Tx3:
		iValue |= 11 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx0:
		iValue |= 12 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx1:
		iValue |= 13 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx2:
		iValue |= 14 << 15;
			break;
		case rpDMAPeripheralRequest::pio1Rx3:
		iValue |= 15 << 15;
			break;
		case rpDMAPeripheralRequest::spi0Tx:
		iValue |= 16 << 15;
			break;
		case rpDMAPeripheralRequest::spi0Rx:
		iValue |= 17 << 15;
			break;
		case rpDMAPeripheralRequest::spi1Tx:
		iValue |= 18 << 15;
			break;
		case rpDMAPeripheralRequest::spi1Rx:
		iValue |= 19 << 15;
			break;
		case rpDMAPeripheralRequest::uart0Tx:
		iValue |= 20 << 15;
			break;
		case rpDMAPeripheralRequest::uart0Rx:
		iValue |= 21 << 15;
			break;
		case rpDMAPeripheralRequest::uart1Tx:
		iValue |= 22 << 15;
			break;
		case rpDMAPeripheralRequest::uart1Rx:
		iValue |= 23 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap0:
		iValue |= 24 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap1:
		iValue |= 25 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap2:
		iValue |= 26 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap3:
		iValue |= 27 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap4:
		iValue |= 28 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap5:
		iValue |= 29 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap6:
		iValue |= 30 << 15;
			break;
		case rpDMAPeripheralRequest::pwm_wrap7:
		iValue |= 31 << 15;
			break;
		case rpDMAPeripheralRequest::i2c0Tx:
		iValue |= 32 << 15;
			break;
		case rpDMAPeripheralRequest::i2c0Rx:
		iValue |= 33 << 15;
			break;
		case rpDMAPeripheralRequest::i2c1Tx :
		iValue |= 34 << 15;
			break;
		case rpDMAPeripheralRequest::i2c1Rx:
		iValue |= 35 << 15;
			break;
		case rpDMAPeripheralRequest::adc:
		iValue |= 36 << 15;
			break;
		case rpDMAPeripheralRequest::xipstream:
		iValue |= 37 << 15;
			break;
		case rpDMAPeripheralRequest::xipssitx:
		iValue |= 38 << 15;
			break;
		case rpDMAPeripheralRequest::xipssirx:
		iValue |= 39 << 15;
			break;
	}
#endif
	pDMA->ctrl_trig |= iValue;
}


int rpDMA::getTransferCount()
{
	dma_channel_hw_t * pDMA = dma_channel_hw_addr(m_iDMAChannel);
	return pDMA->transfer_count;
}

typedef void (*callback_fn)(void *data);

typedef struct {
	bool active;
    callback_fn callback;
    void *data;
} DMA_Callbacks;

static volatile DMA_Callbacks dma_registered_callbacks[NUM_DMA_CHANNELS] = {0};


void irq_handler_dma0() 
{
	uint32_t status = dma_hw->ints0;
	for(unsigned int i=0; i<NUM_DMA_CHANNELS; i++)
	{
		if(dma_registered_callbacks[i].active == false)
			continue;

		if((status & (1u << i)) && dma_registered_callbacks[i].callback)
		{
			dma_registered_callbacks[i].callback(dma_registered_callbacks[i].data);
		}
		dma_hw->ints0 = (1u << i);
	}
}

void rpDMA::SetupCallBack(void* pCallback, void* Object)
{
	if(pCallback == nullptr)
		return;

	//We will use IRQ0 for core 0 and IRQ1 for core 1
	dma_registered_callbacks[m_iDMAChannel].active = true;
	dma_registered_callbacks[m_iDMAChannel].callback = (callback_fn)pCallback;
	dma_registered_callbacks[m_iDMAChannel].data = Object;

    dma_channel_set_irq0_enabled(m_iDMAChannel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, irq_handler_dma0);
	
	irq_clear(DMA_IRQ_0);
    irq_set_enabled(DMA_IRQ_0, true);
}

void rpDMA::RemoveCallback()
{
	dma_registered_callbacks[m_iDMAChannel].active = false;
	dma_registered_callbacks[m_iDMAChannel].callback = nullptr;
	dma_registered_callbacks[m_iDMAChannel].data = nullptr;
	dma_channel_set_irq0_enabled(m_iDMAChannel, false);

	irq_clear(DMA_IRQ_0);
	irq_set_enabled(DMA_IRQ_0, false);
}