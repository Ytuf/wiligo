
#include "rpUART.h"

#include <cstdio>
#include <cstring>

#ifdef PC_SIMULATION
#include "simpico/icslib/cicsSerialPort.h"
cicsSerialPort obSerial;
#endif
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "../fwMenuMain.h"

extern fwMenuMain obMenu;



uart_inst_t * getUART(int iModuleIndex)
{
    uart_inst_t * pUart = 0;
    if (iModuleIndex == 0)
    {
        pUart = uart0;
    }
    else
    {
        pUart = uart1;
    }
    return pUart;
}

rpUART::rpUART(int iUARTModule, int iPIN_UARTTx, int iPIN_UARTRx, int iPIN_UART_CTS, int iPIN_UART_RTS)
{
	setPins(iUARTModule, iPIN_UARTTx, iPIN_UARTRx, iPIN_UART_CTS, iPIN_UART_RTS);
}

void rpUART::setPins(int iUARTModule, int iPIN_UARTTx, int iPIN_UARTRx, int iPIN_UART_CTS, int iPIN_UART_RTS)
{
	m_iUARTModuleIndex = iUARTModule;
	m_iUARTTXPin = iPIN_UARTTx;
	m_iUARTRXPin = iPIN_UARTRx;
	m_iUARTCTSPin = iPIN_UART_CTS;
	m_iUARTRTSPin = iPIN_UART_RTS;
}


bool rpUART::init(unsigned int iBaudRate, bool bEnableHS_CTS,  bool bEnableHS_RTS, unsigned int iDataBits, unsigned int iStopBits, unsigned int iParity, bool bUseTxDMA)
{

	#ifdef PC_SIMULATION
		int iFlowControl=0;
		if (bEnableHS_CTS && bEnableHS_RTS)
			iFlowControl = 1;
		obSerial.OpenCommPort(m_iUARTModuleIndex);
		obSerial.SetCommPortSettings(iBaudRate,iDataBits,iParity,iStopBits,iFlowControl);

	#else

	uart_inst_t * pUart = getUART(m_iUARTModuleIndex);

	m_iAcutalBaudRate = uart_init(pUart, iBaudRate);

    reclaimPinsForSpecialFunction(bEnableHS_CTS, bEnableHS_RTS);

	uart_set_hw_flow(pUart, bEnableHS_CTS, bEnableHS_RTS);

	uart_set_fifo_enabled(pUart,true);

	// Set our data format
    uart_parity_t iSetParity = UART_PARITY_NONE;
    if (iParity == 1)
        iSetParity = UART_PARITY_EVEN;
    else if (iParity == 2)
        iSetParity = UART_PARITY_ODD;
	uart_set_format(pUart, iDataBits, iStopBits, iSetParity );//


    if (bUseTxDMA)
    {
        m_bUseTxDMA = bUseTxDMA;
        if (m_iUARTModuleIndex == 0)
            obDMATx.setupPeripheralRequest(rpDMAPeripheralRequest::uart0Tx);
        else
            obDMATx.setupPeripheralRequest(rpDMAPeripheralRequest::uart1Tx);
        obDMATx.initClaimFreeDMAChannel();
    }

#endif
	return true;

}

void rpUART::reclaimPinsForSpecialFunction(bool bEnableHS_CTS, bool bEnableHS_RTS)
{
	if (bUseAuxPins) {
		gpio_set_function(m_iUARTTXPin, GPIO_FUNC_UART_AUX);
		gpio_set_function(m_iUARTRXPin, GPIO_FUNC_UART_AUX);
	}
	else {
		gpio_set_function(m_iUARTTXPin, GPIO_FUNC_UART);
		gpio_set_function(m_iUARTRXPin, GPIO_FUNC_UART);
	}
    if (bEnableHS_CTS) {
        gpio_set_function(m_iUARTCTSPin, GPIO_FUNC_UART);
    }
    if (bEnableHS_RTS)
    {
        gpio_set_function(m_iUARTRTSPin, GPIO_FUNC_UART);
    }
}

bool rpUART::txFIFOEmpty()
{
    uart_inst_t * pUart = getUART(m_iUARTModuleIndex);

    return (uart_get_hw(pUart)->fr & UART_UARTFR_TXFE_BITS);
}


void rpUART::txDataOld(const  char * pData, int iLength)
{
    uart_inst_t * pUart = getUART(m_iUARTModuleIndex);


	uart_write_blocking(pUart, (const uint8_t *)pData,  iLength);
	//for (iCount = 0; iCount < iLength; iCount++)
	//	uart_putc_raw(pUart, pData[iCount]);
	
}

#ifdef PC_SIMULATION

bool rpUART::txData(const  unsigned char * pData, unsigned int iLength)
{
	obSerial.WriteCommPort(pData, iLength);
	return true;
}

#else


bool __not_in_flash_func(rpUART::txData)(const  unsigned char * pData, unsigned int iLength)
{
	uart_inst_t * pUart = getUART(m_iUARTModuleIndex);
	int iCount = 0;
	int iNumBytesToSend=0;
	int iNumBytesToWrite=0;
	bool bWriteTimeOut = false;
	const unsigned char * pUARTTxData = pData;

	bool bFifoEmpty =  (((uart_hw_t *)(pUart))->fr & 0x80) ;
	m_iLastUpdateTimeUs = obTimeStamp.getTimeNow();
	// is the fifo empty - if so we can do up to 32 bytes quickly
	if (iLength <= 32 &&  bFifoEmpty )
	{
		for (iCount=0;iCount<iLength;iCount++)
			((uart_hw_t *)(pUart))->dr = *pData++;
	}
	else
	{
		iNumBytesToSend = iLength;
		while (iNumBytesToSend > 0)
		{
			bFifoEmpty =  (((uart_hw_t *)(pUart))->fr & 0x80);
			if (bFifoEmpty)
			{
				if (iNumBytesToSend >= 32)
					iNumBytesToWrite = 32;
				else
					iNumBytesToWrite = iNumBytesToSend;
				for (iCount=0;iCount<iNumBytesToWrite;iCount++)
					((uart_hw_t *)(pUart))->dr = *pData++;
				iNumBytesToSend -= iNumBytesToWrite;
				if (iNumBytesToSend!=0)
					m_iLastUpdateTimeUs = obTimeStamp.getTimeNow();
			}
			else
			{
				if ((obTimeStamp.getTimeNow() - m_iLastUpdateTimeUs) >= 50'000)
				{
					bWriteTimeOut = true;
					m_iWriteTimeouts++;
					break;
				}
			}

			//m_iLastUpdateTimeNs = obTimeStamp.getTimeNow();
			//uart_write_blocking(pUart, (const uint8_t *)pData,  iLength);
		}
	}

	// reset the write timeout on a successful
	if (!bWriteTimeOut)
		m_iWriteTimeouts =0;
	else
		m_bHadOneWriteTime = true;
#ifndef WILI_TWO_DISPLAY
	if (obMenu.obMenuXUART.m_bUARTLoggerMode && m_iUARTModuleIndex==1)
	{
		char szOutput[64];
		char szBytes[32];

		if (bWriteTimeOut) {
			sprintf(szOutput, "FAIL ");
		}
		else
			sprintf(szOutput, "Tx) ");
		if (iLength > 8)
			iLength = 8;
		for (iCount=0;iCount<iLength;iCount++) {
			sprintf(szBytes, "%02hhX ", pUARTTxData[iCount]);
			strcat(szOutput,szBytes);
		}
		obMenu.obMenuXUART.addLogTxData(szOutput);
	}
#endif

	return true;

}
#endif

#ifdef PC_SIMULATION

int rpUART::rxDataCount()
{
	return obSerial.NumRxBytesAvailable();
}

#else

int __not_in_flash_func(rpUART::rxDataCount)()
{
	uart_inst_t * pUart = getUART(m_iUARTModuleIndex);
	//int iCount = 0;

	if (uart_get_hw(pUart)->fr & (1 << 4))
	{
		return 0;
	}
	else
	{
		return 1;
	}


}

#endif

int  __not_in_flash_func(rpUART::rxReadData)(unsigned char * pData, int iLength)
{

#ifdef PC_SIMULATION
	int iNumberBytesRead;
	obSerial.ReadCommPort(pData,iNumberBytesRead,iLength);
	return iNumberBytesRead;
#else
	uart_inst_t * pUart = getUART(m_iUARTModuleIndex);

	pData[0] = (char) (uart_get_hw(pUart)->dr & 0xFF);

	return 1;
#endif
}

bool rpUART::addTxData(const char * pData, int iLength)
{
    if (m_bUseTxDMA) {
        if (!hasRoom(iLength))
            return false;
        unsigned char * pSourceBuffer;
        int iCount;
        int * pCountOfBytes;
        if (m_bCurrentBufferIsA)
        {
            pSourceBuffer = btUARTBufferTxA;
            pCountOfBytes = &m_iUARTBufferTxACount;
        }
        else
        {
            pSourceBuffer = btUARTBufferTxB;
            pCountOfBytes = &m_iUARTBufferTxBCount;
        }
        for (iCount=0;iCount<iLength;iCount++)
            pSourceBuffer[(*pCountOfBytes)++] = pData[iCount];
    }
    else {
        txData((const unsigned char *)pData,iLength);
    }
    return true;
}

bool rpUART::hasRoom(int iNumberBytes)
{

    if (m_bUseTxDMA) {
        if (m_bCurrentBufferIsA)
        {
            if ((m_iUARTBufferTxACount + iNumberBytes) < TXBUFF_MAX) {
                return true;
            }
            return false;
        }
        else {
            if ((m_iUARTBufferTxBCount + iNumberBytes) < TXBUFF_MAX) {
                return true;
            }
            return false;
        }
    }
    else {
        return true; // grooooossssss
    }
}

void rpUART::process()
{
#ifdef PC_SIMULATION


#else
    uart_inst_t * pUart = getUART(m_iUARTModuleIndex);

    if (m_bUseTxDMA) {
        if (m_bDMAInProcess)
        {
            if (!obDMATx.isComplete())
            {
                return;
            }
            m_bDMAInProcess = false;
        }

        if (m_bCurrentBufferIsA)
        {
            if (m_iUARTBufferTxACount)
            {
                setupDMATransmit(true);
                m_bCurrentBufferIsA = false;
            }
        }
        else
        {
            if (m_iUARTBufferTxBCount)
            {
                setupDMATransmit(false);
                m_bCurrentBufferIsA = true;
            }
        }
    }
#endif
}

void rpUART::setupDMATransmit(bool bUseBuffA)
{

#ifdef PC_SIMULATION


#else
    uart_inst_t * pUart = getUART(m_iUARTModuleIndex);
    unsigned char * pSourceBuffer;
    int iCountOfBytes;

    if (bUseBuffA)
    {
        pSourceBuffer = btUARTBufferTxA;
        iCountOfBytes =m_iUARTBufferTxACount;
    }
    else
    {
        pSourceBuffer = btUARTBufferTxB;
        iCountOfBytes =m_iUARTBufferTxBCount;
    }
    obDMATx.setupTransfer((void *) &(uart_get_hw(pUart)->dr),pSourceBuffer,
                            false, true,rpDMATransferDataSize::byte,iCountOfBytes);
    obDMATx.start();
    if (bUseBuffA)
    {
        m_iUARTBufferTxACount=0;
    }
    else
    {
        m_iUARTBufferTxBCount=0;
    }
    m_bDMAInProcess = true;
	#endif
}

