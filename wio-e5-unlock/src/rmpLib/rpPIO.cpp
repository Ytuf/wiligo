
#include "rpPIO.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/dma.h"

#include "stdlib.h"
#include "stdio.h"

static PIO getPIOFromIndex(uint8_t m_iPIOIndex) {
	if (m_iPIOIndex == 0) {
		return pio0;
	}
	else if (m_iPIOIndex == 1) {
		return pio1;
	}
	else {
		return pio2;
	}
}


void rpPIO::encode_wrap()
{
    m_iWrap = m_iCurrentInstruction-1;
}

void rpPIO::encode_wrapTarget()
{
   m_iWrapTarget = m_iCurrentInstruction;
}

int rpPIO::returnAddressForLabel(char chLabel)
{
    int iCount;
	for (iCount = m_iFirstInstruction; iCount < m_iCurrentInstruction; iCount++)
    {
        if (chLabel == m_chLabels[iCount])
            return iCount;
    }
    return -1;
}

int rpPIO::getLastInstruction()
{
	return m_iCurrentInstruction;
}

void rpPIO::encode_end()
{
    PIO pio = getPIOFromIndex(m_iPIOIndex);
    int iCount;

    pio_sm_set_wrap (pio,m_iPIOStateMachineIndex,m_iWrapTarget, m_iWrap);

    // setup the jump targets
	for (iCount = m_iFirstInstruction; iCount < m_iCurrentInstruction; iCount++)
    {
        if (m_chJumpLabels[iCount])
        {
            m_iInstructionMemoryCache[iCount] |= (returnAddressForLabel(m_chJumpLabels[iCount]) & 0x1f);
        }
    }

    // we have to do this becasue the instructions are write only
	for (iCount = m_iFirstInstruction; iCount < m_iCurrentInstruction; iCount++) 
    {
        pio->instr_mem[iCount] = m_iInstructionMemoryCache[iCount];
    }

}

void rpPIO::encode_addLabel(char chLabel)
{
    m_chLabels[m_iCurrentInstruction] = chLabel;
}

void rpPIO::encode_begin()
{
    // reset the current instruction
    m_iCurrentInstruction = m_iFirstInstruction;
    // clear the address label
    int iCount;
	for (iCount = 0; iCount < RP_PIO_INSTRUCTION_COUNT; iCount++)
    {
        m_chLabels[iCount]=0;
        m_chJumpLabels[iCount]=0;
        m_iInstructionMemoryCache[iCount] = 0;
    }

}

void rpPIO::SetCurrentInstruction(unsigned char btSideSet, unsigned char btDelay, int iInstruction, bool bSideSetEnabled)
{
  

    // encode the side set and delay
    // TODO compile option side set enable encoding
    if (m_iSideSetEnabledCount)
    {
    	if (m_bSideSetOptionEnable)
    	{
    		// side set
    		if (bSideSetEnabled)
			{
				iInstruction |= 0x1000;
			}

    		iInstruction |= btSideSet << (12 - m_iSideSetEnabledCount);
    		// delay
    		iInstruction |= (btDelay<< 8);
    	}
    	else
    	{
    		// side set
    		iInstruction |= btSideSet << (13 - m_iSideSetEnabledCount);
    		// delay
    		iInstruction |= (btDelay<< 8);
    	}
    }
    else
    {
        // all delay
        iInstruction |= (btDelay<< 8);
    }


    m_iInstructionMemoryCache[m_iCurrentInstruction] =  iInstruction;

    m_iCurrentInstruction++; // point to next instruction
}

void rpPIO::encode_nop(unsigned char btSideSet, unsigned char btDelay, bool bSetSetEnabled)
{
    // Assembles to mov y, y . "No operation", has no particular side effect, but a useful vehicle for a side-set
    // operation or an extra delay.

    encode_mov(btSideSet,btDelay,erpPIOMovDestination::y,erpPIOMovOperation::none,erpPIOMovSource::y);

}

void rpPIO::encode_pull(unsigned char btSideSet, unsigned char btDelay,
	bool bIfEmpty, bool bBlock, bool bSetSetEnabled)
{
	unsigned int iTemp = 0x8080; // pull command

	if (bIfEmpty) iTemp |= (1 << 6);
	if (bBlock) iTemp |= (1 << 5);
	
	SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
}

void rpPIO::encode_push(unsigned char btSideSet,
	unsigned char btDelay,
	bool bIfFull,
	bool bBlock, bool bSetSetEnabled)
{
	unsigned int iTemp = 0x8000; // push command

	if (bIfFull) iTemp |= (1 << 6);
	if (bBlock) iTemp |= (1 << 5);
	
	SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
}


void rpPIO::encode_irq(unsigned char btSideSet, unsigned char btDelay, bool bClearInt, 
	                bool bWaitInt, int iIntIndex, bool bSetSetEnabled)
{
	unsigned int iTemp = 0xc000; // irq command

	if (bClearInt) iTemp |= (1 << 6);
	if (bWaitInt) iTemp |= (1 << 5);

	iTemp |= iIntIndex;
	
	SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
	
}


void rpPIO::encode_in(unsigned char btSideSet, unsigned char btDelay, erpPIOInSource eSource, int iBitCount, bool bSetSetEnabled)
{
	unsigned int iTemp = 0x4000; // in command

	switch (eSource)
	{
	    case erpPIOInSource::pins:// 000
		    break;
	    case erpPIOInSource::x:// 001
		    iTemp |= (1 << 5);
		     break;
	    case erpPIOInSource::y:// 010
		    iTemp |= (2 << 5);
		     break;
	    case erpPIOInSource::null:// 011
		    iTemp |= (3 << 5);
		     break;
	    case erpPIOInSource::isr:// 110
		    iTemp |= (6 << 5);
		     break;
	    case erpPIOInSource::osr:// 111
		    iTemp |= (7 << 5);
		     break;
	}

	iTemp |= (iBitCount&0x1f);
	
	SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
	
}

void rpPIO::encode_mov(unsigned char btSideSet, unsigned char btDelay,erpPIOMovDestination eDestination,
                            erpPIOMovOperation eOperation, erpPIOMovSource eSource, bool bSetSetEnabled)
{
    unsigned int iTemp = 0xA000; // mov command

    switch (eDestination)
    {
        case erpPIOMovDestination::pins:// 000
            break;
        case erpPIOMovDestination::x:// 001
            iTemp |= (1 << 5);
            break;
        case erpPIOMovDestination::y:// 010
            iTemp |= (2 << 5);
            break;
        case erpPIOMovDestination::exec:// 100
            iTemp |= (4 << 5);
            break;
        case erpPIOMovDestination::pc:// 101
            iTemp |= (5 << 5);
            break;
        case erpPIOMovDestination::isr:// 110
            iTemp |= (6 << 5);
            break;
        case erpPIOMovDestination::osr:// 111
            iTemp |= (7 << 5);
            break;
    }

    switch (eOperation)
    {
        case erpPIOMovOperation::none: // 000
            break;
        case erpPIOMovOperation::invert: // 001
            iTemp |= (1 << 3);
            break;
        case erpPIOMovOperation::bit_reverse: // 010
            iTemp |= (2 << 3);
            break;
    }

    switch (eSource)
    {
        case erpPIOMovSource::pins: // 000
            break;
        case erpPIOMovSource::x: // 001
            iTemp |=  1;
            break;
        case erpPIOMovSource::y: // 010
            iTemp |=  2;
            break;
        case erpPIOMovSource::null: // 011
            iTemp |=  3;
            break;
        case erpPIOMovSource::status: // 011
            iTemp |=  5;
            break;
        case erpPIOMovSource::isr: // 011
            iTemp |=  6;
            break;
        case erpPIOMovSource::osr: // 111
            iTemp |=  7;
            break;
    }

    SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);

}

void rpPIO::encode_out(unsigned char btSideSet, unsigned char btDelay, erpPIOOutDestination eOutDestination, unsigned char bBitCount, bool bSetSetEnabled)
{
    unsigned int iTemp = 0x6000; // out command


    switch (eOutDestination)
    {
        case erpPIOOutDestination::pins:
            // 000
            break;
        case erpPIOOutDestination::x:
            // 001
            iTemp |= (1 << 5);
            break;
        case erpPIOOutDestination::y:
            // 010
            iTemp |= (2 << 5);
            break;
        case erpPIOOutDestination::null:
            // 011
            iTemp |= (3 << 5);
            break;
        case erpPIOOutDestination::pindirs:
            // 100
            iTemp |= (4 << 5);
            break;
        case erpPIOOutDestination::pc:
            // 101
            iTemp |= (5 << 5);
            break;
        case erpPIOOutDestination::isr:
        // 110
            iTemp |= (6 << 5);
            break;
        case erpPIOOutDestination::exec:
        // 111
            iTemp |= (7 << 5);
            break;
    }

    iTemp |= (0x1f & bBitCount);

    SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
    
}



void rpPIO::encode_jmp(unsigned char btSideSet, unsigned char btDelay,  erpPIOJumpCondition eJumpCondition,  char chAddress, bool bSetSetEnabled)
{
    unsigned int iTemp = 0x0; // jump command


    switch (eJumpCondition)
    {
        case erpPIOJumpCondition::always:
            // condition is ALWAYS (000)
            break;
        case erpPIOJumpCondition::not_X:
            // 001
            iTemp |= (1 << 5);
            break;
        case erpPIOJumpCondition::XNotZeroPostDec:
            // 010
            iTemp |= (2 << 5);
            break;
        case erpPIOJumpCondition::not_Y:
            // 011
            iTemp |= (3 << 5);
            break;
        case erpPIOJumpCondition::YNotZeroPostDec:
            // 100
            iTemp |= (4 << 5);
            break;
        case erpPIOJumpCondition::XNotEqualY:
            // 101
            iTemp |= (5 << 5);
            break;
        case erpPIOJumpCondition::pin:
            // 110
            iTemp |= (6 << 5);
            break;
        case erpPIOJumpCondition::NotOSRempty:
            // 111
            iTemp |= (7 << 5);
            break;
    }

    m_chJumpLabels[m_iCurrentInstruction] = chAddress;
    
    SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
}

void rpPIO::encode_set(unsigned char btSideSet, unsigned char btDelay, 
				erpPIOSetDestination eSetDestination, int iValue, bool bSetSetEnabled)
{
	unsigned int iTemp = 0xE000; // set command

	switch (eSetDestination)
	{
		case erpPIOSetDestination::pins:
			break;
		case erpPIOSetDestination::pindirs:
			// 100
			iTemp |= (4 << 5);
			break;
		case erpPIOSetDestination::x:
			// 001
			iTemp |= (1 << 5);
			break;
		case erpPIOSetDestination::y:
			// 010
			iTemp |= (2 << 5);
			break;
	}


	// destination is PINS (000)
	iTemp |= (0x1f & iValue);

	SetCurrentInstruction(btSideSet, btDelay, iTemp, bSetSetEnabled);
	
}
/*
void rpPIO::encode_set_pins_direction(unsigned char btSideSet, unsigned char btDelay, unsigned char bDirection)
{
    unsigned int iTemp = 0xE000; // set command

    // TODO IMPLEMENT DELAY SIDE SET

    // destination is PINSDIR (100)
    iTemp |= 0x80;

    iTemp |= (0x1f & bDirection);

    SetCurrentInstruction(btSideSet, btDelay, iTemp);
}

void rpPIO::encode_set_pins(unsigned char btSideSet, unsigned char btDelay, unsigned char bData)
{
    unsigned int iTemp = 0xE000; // set command

    // TODO IMPLEMENT DELAY SIDE SET

    // destination is PINS (000)
    iTemp |= (0x1f & bData);

    SetCurrentInstruction(btSideSet, btDelay,iTemp);
}
*/
void rpPIO::encode_wait(unsigned char btSideSet, unsigned char btDelay,  bool bWaitPolarityIsOne, erpPIOWaitSource eWaitSource, int iIndex, bool bSetSetEnabled)
{
    unsigned int iTemp = 0x2000; // wait command

    if (bWaitPolarityIsOne)
        iTemp |= 0x80;

    switch (eWaitSource)
    {
        case erpPIOWaitSource::gpio:
            // 00
            break;
        case erpPIOWaitSource::pin:
            // 01
            iTemp |= (1 << 5);
            break;
        case erpPIOWaitSource::irq:
            // 10
            iTemp |= (2 << 5);
            break;
        default: // reserved
            break;
    }

    // index
    iTemp |= iIndex & 0x1F;

    SetCurrentInstruction(btSideSet, btDelay,iTemp, bSetSetEnabled);

}



void rpPIO::setupPins(int iSideSetCount, int iSetCount, int iOutCount, int iInBase, 
	                    int iSideSetBase, int iSetBase, int iOutBase, int iJumpPin, bool bSideSetOptionalEnable)
{
	
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	m_bSideSetOptionEnable = bSideSetOptionalEnable;


	m_iSideSetEnabledCount = iSideSetCount; // needed to properly encode side set in instructions


	unsigned int uiPinControl = 0;

	// Keep the original absolute GPIO numbers for the SDK pindirs calls below
	// (pio_sm_set_consecutive_pindirs subtracts gpiobase internally).
	int iSideSetBaseAbs = iSideSetBase;
	int iSetBaseAbs     = iSetBase;
	int iOutBaseAbs     = iOutBase;

#ifdef PICO_RP2350
	if (m_bGPIOBase16Enabled)
	{

		pio->gpiobase = 16;
		if (iInBase > 0 ) iInBase-=16;
		if (iSideSetBase > 0 ) iSideSetBase-=16;
		if (iSetBase > 0 ) iSetBase-=16;
		if (iOutBase > 0 ) iOutBase-=16;
		if (iJumpPin > 0 ) iJumpPin-=16;


	}
	else
		pio->gpiobase = 0;

#endif
	// Load SMx_PINCTRL register values

	if (bSideSetOptionalEnable)
		uiPinControl |=  ((iSideSetCount+1) << 29);   // side set count includes the enable bit
	else
		uiPinControl |=  (iSideSetCount << 29);
	uiPinControl |=  (iSetCount << 26);
	uiPinControl |=  (iOutCount << 20);
	uiPinControl |=  (iInBase << 15);
	uiPinControl |=  (iSideSetBase << 10);
	uiPinControl |=  (iSetBase << 5);
	uiPinControl |=  (iOutBase << 0);

	// save the value
	pio->sm[m_iPIOStateMachineIndex].pinctrl  = uiPinControl;
	// jump pin clear and set
	pio->sm[m_iPIOStateMachineIndex].execctrl  &= ~(0x1f << 24);
	pio->sm[m_iPIOStateMachineIndex].execctrl  |= (iJumpPin << 24);
	if (bSideSetOptionalEnable)
		// pio->sm[m_iPIOStateMachineIndex].execctrl = uiPinControl |=  1 << 30;
		pio->sm[m_iPIOStateMachineIndex].execctrl |= 1 << 30;

	if (iSetCount)
		pio_sm_set_consecutive_pindirs(pio, m_iPIOStateMachineIndex, iSetBaseAbs, iSetCount, true);
	if (iOutCount)
		pio_sm_set_consecutive_pindirs(pio, m_iPIOStateMachineIndex, iOutBaseAbs, iOutCount, true);
	if (iSideSetCount)
		pio_sm_set_consecutive_pindirs(pio, m_iPIOStateMachineIndex, iSideSetBaseAbs, iSideSetCount, true);
}

void rpPIO::setupStatus(int iSel, int iN)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	unsigned int v = pio->sm[m_iPIOStateMachineIndex].execctrl;
#ifdef PICO_RP2350
	// RP2350: STATUS_SEL = 2 bits at 6:5 (TX/RX/IRQ), STATUS_N = 5 bits at 4:0.
	v &= ~0x7Fu;
	v |= (iSel & 0x3) << 5;
	v |= iN & 0x1F;
#else
	// RP2040: STATUS_SEL = 1 bit at 4 (TX/RX), STATUS_N = 4 bits at 3:0.
	v &= ~0x1Fu;
	v |= (iSel & 0x1) << 4;
	v |= iN & 0xF;
#endif
	pio->sm[m_iPIOStateMachineIndex].execctrl = v;
}

void rpPIO::clearFIFOs()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	pio_sm_clear_fifos(pio, m_iPIOStateMachineIndex);
}


void rpPIO::setupFIFOs(bool bUseIn, bool bAutoPush, int iPushThreshold, bool bInShiftRight,
	        bool bUseOut, bool bAutoPull, int iPullThreshold,bool bOutShiftRight)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	bool bJoinRx = !bUseOut; // join fifos not used
	bool bJoinTx = !bUseIn;

	// for full 32 bits use 0
	if (iPushThreshold == 32) iPushThreshold = 0;
	if (iPullThreshold == 32) iPullThreshold = 0;


	unsigned int uiShiftControl = 0;

	// Load SMx_SHIFTCTRL register values
	if (bJoinRx)  uiShiftControl |= (1 << 31);
	if (bJoinTx)  uiShiftControl |= (1 << 30);
	uiShiftControl |=  (iPullThreshold << 25);
	uiShiftControl |=  (iPushThreshold << 20);
	if (bOutShiftRight) uiShiftControl |= (1 << 19);
	if (bInShiftRight) uiShiftControl |= (1 << 18);
	if (bAutoPull)  uiShiftControl |= (1 << 17);
	if (bAutoPush)  uiShiftControl |= (1 << 16);

	// save the value
	pio->sm[m_iPIOStateMachineIndex].shiftctrl = uiShiftControl;

	pio_sm_clear_fifos(pio, m_iPIOStateMachineIndex);

}

void rpPIO::init(int iPIOModuleIndex, int iPIOStateMachineIndex, int iFirstInstruction)
{
    m_iPIOIndex = iPIOModuleIndex;
    m_iFirstInstruction = iFirstInstruction;
    m_iPIOStateMachineIndex = iPIOStateMachineIndex;
}

unsigned int rpPIO::readYRegisterViaFIFO()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	//setupFIFOs(true, false, 32, false, false, false, 0, 0);
	pio_sm_exec(pio, m_iPIOStateMachineIndex, pio_encode_in(pio_src_dest::pio_y,32)); // mov y to fifo
	pio_sm_exec(pio, m_iPIOStateMachineIndex, pio_encode_push(false,true));
	return pio_sm_get_blocking(pio, m_iPIOStateMachineIndex);
}

void rpPIO::setStartInstruction(int iStartInstruction)
{
	m_iFirstInstruction = m_iFirstInstruction  + iStartInstruction;

}

void rpPIO::start()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	pio_sm_restart(pio, m_iPIOStateMachineIndex);
	pio_sm_exec(pio, m_iPIOStateMachineIndex, pio_encode_jmp(m_iFirstInstruction));
    pio_sm_set_enabled(pio, m_iPIOStateMachineIndex, true);
}

void rpPIO::reinitForStartMulti()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	pio_sm_restart(pio, m_iPIOStateMachineIndex);
	pio_sm_clkdiv_restart(pio, m_iPIOStateMachineIndex);
	pio_sm_exec(pio, m_iPIOStateMachineIndex, pio_encode_jmp(m_iFirstInstruction));
}

void rpPIO::startMulti(bool bSM0, bool bSM1, bool bSM2, bool bSM3)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	int iSm=0;

	if (bSM0)
		iSm |= 0x1;
	if (bSM1)
		iSm |= 0x2;
	if (bSM2)
		iSm |= 0x4;
	if (bSM3)
		iSm |= 0x8;

	pio->ctrl |= iSm;
}



void rpPIO::stop()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	pio_sm_set_enabled(pio, m_iPIOStateMachineIndex, false);
}

void __not_in_flash_func(rpPIO::writeTxFIFO)(unsigned int iValue)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	//pio_sm_put_blocking(pio, m_iPIOStateMachineIndex, iValue);
	while (pio->fstat & (1u << (PIO_FSTAT_TXFULL_LSB + m_iPIOStateMachineIndex))) {};// while its full
	pio->txf[m_iPIOStateMachineIndex] = iValue;

}

int rpPIO::getTxFIFOCount()

{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	return pio_sm_get_tx_fifo_level(pio, m_iPIOStateMachineIndex);
}

int rpPIO::getTxFifoDepth()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	return (pio->sm[m_iPIOStateMachineIndex].shiftctrl & PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS) ? 8 : 4;
}

void rpPIO::abortRxDMA()
{
	obDMARx.abort();
	obDMARx.RemoveCallback();
}



void rpPIO::enableRxDMA(int iChannel, void * pDestinationAddress, int iMaxCount, erpPIODMASize iDMASize)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	obDMARx.init(iChannel);

	dma_channel_config c = dma_channel_get_default_config(iChannel);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	if (iDMASize == erpPIODMASize::dmaSize8)
		channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	else if (iDMASize == erpPIODMASize::dmaSize16)
		channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	else
		channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_dreq(&c, pio_get_dreq(pio, m_iPIOStateMachineIndex, false));

	unsigned char * pBytes = (unsigned char *) &pio->rxf[m_iPIOStateMachineIndex];
	pBytes += 3; // Offset to get the first byte of each word
	dma_channel_configure(iChannel,
		&c,
		pDestinationAddress,        // Destination pointer
		pBytes, //&pio->rxf[m_iPIOStateMachineIndex],      // Source pointer
		iMaxCount, //capture_size_words, // Number of transfers
		true                // Start immediately
	);
	/*

	unsigned char * pBytes = (unsigned char *) &pio->rxf[m_iPIOStateMachineIndex];
	pBytes += 3;


	obDMA.setupTransfer(pDestinationAddress,
		(void*) pBytes,
		true,
		false,
		rpDMATransferDataSize::byte,
		iMaxCount);

	if (m_iPIOIndex == 0)
	{
		switch (m_iPIOStateMachineIndex)
		{
		case 0:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio0Rx0);
			break;
		case 1:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio0Rx1);
			break;
		case 2:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio0Rx2);
			break;
		case 3:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio0Rx3);
			break;
		}
	}
	else
	{
		switch (m_iPIOStateMachineIndex)
		{
		case 0:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio1Rx0);
			break;
		case 1:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio1Rx1);
			break;
		case 2:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio1Rx2);
			break;
		case 3:
			obDMA.setupPeripheralRequest(rpDMAPeripheralRequest::pio1Rx3);
			break;
		}
	}

	obDMA.start();

	*/
}


bool rpPIO::checkAndClearInt(int iIntIndex)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	if (pio->irq & (1 << iIntIndex))
	{
		pio->irq |= (1 << iIntIndex);
		return true;
	}
	else
	{
		return false;
	}
}



void * g_pCallBackDataPio1=0;
void(* g_pCallBackPio1)(void *);

void * g_pCallBackDataPio0=0;
void(* g_pCallBackPio0)(void *);

void irq_handler_pio0()
{
	g_pCallBackPio0(g_pCallBackDataPio0);
	pio_interrupt_clear(pio0, 0);
}

//int(__stdcall *ICSControlXCallbackDoFunction)(void * pXcontrol, int iEnumFunctionIndex, int iNumItems, int * iTypesArray, void ** pDataArray);
void irq_handler_pio1()
{
	g_pCallBackPio1(g_pCallBackDataPio1);
	pio_interrupt_clear(pio1, 0);
}

void rpPIO::SetupCallBackOnIRQ(void * pCallBack,
	void * pObject, erpPIOCpuIRQSource eSource)
{
	// Configure the processor to run dma_handler() when DMA IRQ 0 is asserted

	PIO pio = getPIOFromIndex(m_iPIOIndex);

	switch (eSource)
	{
		case erpPIOCpuIRQSource::irq0:
			pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
			break;
		case erpPIOCpuIRQSource::rxfifo0_not_empty:
			pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty, true);
			break;
	}
	if (m_iPIOIndex == 0)
	{
		irq_set_exclusive_handler(PIO0_IRQ_0, irq_handler_pio0);
		irq_set_enabled(PIO0_IRQ_0, true);

		g_pCallBackPio0 = (void(*)(void *))pCallBack;
		g_pCallBackDataPio0 = pObject;

	}
	else if (m_iPIOIndex == 1) {
		irq_set_exclusive_handler(PIO1_IRQ_0, irq_handler_pio0);
		irq_set_enabled(PIO1_IRQ_0, true);

		g_pCallBackPio0 = (void(*)(void *))pCallBack;
		g_pCallBackDataPio0 = pObject;
	}
	else
	{
		irq_set_exclusive_handler(PIO2_IRQ_0, irq_handler_pio1);
		irq_set_enabled(PIO2_IRQ_0, true);

		g_pCallBackPio1 = (void(*)(void *))pCallBack;
		g_pCallBackDataPio1 = pObject;

	}


}

void rpPIO::SetupCallBackOnDMARxIRQ(void* pCallback, void* Object)
{
	obDMARx.SetupCallBack(pCallback, Object);
}

void rpPIO::SetupCallBackOnDMATxIRQ(void* pCallback, void* Object)
{
	obDMATx.SetupCallBack(pCallback, Object);
}

void rpPIO::disableRxDMAIRQ()
{
	dma_channel_set_irq0_enabled(obDMARx.getChannel(), false);
	// if (m_iPIOIndex == 0)
	// {
	// 	irq_set_enabled(PIO0_IRQ_0, false);
	// }
	// else
	// {
	// 	irq_set_enabled(PIO1_IRQ_0, false);
	// }
}

int rpPIO::getRxDMATransferCount()
{
	return obDMARx.getTransferCount();
}

unsigned int  __not_in_flash_func(rpPIO::readRxFIFO)()
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	// wait until not empty
	//while (pio->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + m_iPIOStateMachineIndex)) > 0) {};
	// read fifo
	return pio->rxf[m_iPIOStateMachineIndex];

	//return pio_sm_get_blocking(pio, m_iPIOStateMachineIndex);
}

int __not_in_flash_func(rpPIO::getRxFIFOCount)()
	
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	uint bitoffs = PIO_FLEVEL_RX0_LSB + m_iPIOStateMachineIndex * (PIO_FLEVEL_RX1_LSB - PIO_FLEVEL_RX0_LSB);
	const uint32_t mask = PIO_FLEVEL_RX0_BITS >> PIO_FLEVEL_RX0_LSB;
	return (pio->flevel >> bitoffs) & mask;

	//return pio_sm_get_rx_fifo_level(pio, m_iPIOStateMachineIndex);
}

void rpPIO::enableTxDMA(int iChannel, void * pDestinationAddress, int iMaxCount, erpPIODMASize iDMASize)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
	
	obDMATx.init(iChannel);
	
	dma_channel_config c = dma_channel_get_default_config(iChannel);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	if (iDMASize == erpPIODMASize::dmaSize8)
		channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	else if (iDMASize == erpPIODMASize::dmaSize16)
		channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	else
		channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_dreq(&c, pio_get_dreq(pio, m_iPIOStateMachineIndex, true));

	unsigned char * pBytes = (unsigned char *) &pio->txf[m_iPIOStateMachineIndex];
	//pBytes += 3;
	dma_channel_configure(iChannel,
		&c,
		pBytes,        // Destination pointer
		pDestinationAddress, //&pio->rxf[m_iPIOStateMachineIndex],      // Source pointer
		iMaxCount, //capture_size_words, // Number of transfers
		true                // Start immediately
	);
}

bool rpPIO::isTxDMAComplete()
{
	return obDMATx.isComplete();
}

bool rpPIO::isRxDMAComplete()
{
	return obDMARx.isComplete();
}

void rpPIO::abortTxDMA()
{
	obDMATx.abort();
	obDMATx.RemoveCallback();
}

unsigned int rpPIO::setupClockPeriodByDiv(float  fDiv)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);
		
	pio_sm_set_clkdiv(pio, m_iPIOStateMachineIndex, fDiv);
	
	return pio->sm[m_iPIOStateMachineIndex].clkdiv;
	
}

void rpPIO::enableGPIOBase16(bool bEnable)
{
	m_bGPIOBase16Enabled = bEnable;
}

unsigned int rpPIO::setupClockPeriod(float  fPeriodInNs)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

	float div = (float) fPeriodInNs / 8.0; // clock_get_hz(clk_sys);
	
	pio_sm_set_clkdiv(pio, m_iPIOStateMachineIndex, div);
	
	return pio->sm[m_iPIOStateMachineIndex].clkdiv;
}

bool rpPIO::isTXFifoEmpty(void)
{
	PIO pio = getPIOFromIndex(m_iPIOIndex);

    return pio_sm_is_tx_fifo_empty(pio, m_iPIOStateMachineIndex);
}
