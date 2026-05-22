#pragma once

#include "rpDMA.h"
#include "rpTypes.h"
#include "rpTimeStamp.h"

class rpUART 
{
private:
	
	int m_iUARTModuleIndex = 0;
	int m_iUARTRXPin = 1;
	int m_iUARTTXPin = 0;
	int m_iUARTCTSPin = 2;
	int m_iUARTRTSPin = 3;

    #define TXBUFF_MAX  1024
    bool m_bCurrentBufferIsA = true;
    int m_iUARTBufferTxACount = 0;
    int m_iUARTBufferTxBCount = 0;
    int m_iUARTBufferTxBRemaining = 0;
    int m_iUARTBufferTxARemaining = 0;
    unsigned char btUARTBufferTxA[TXBUFF_MAX];
    unsigned char btUARTBufferTxB[TXBUFF_MAX];

    bool txFIFOEmpty();

	bool m_bUseTxDMA  = false;
    rpDMA obDMATx;
    bool m_bDMAInProcess = false;
    void  setupDMATransmit(bool bUseBuffA);

public:
	bool m_bHadOneWriteTime = false;
	rptype::u64 m_iLastUpdateTimeUs=0;
	rpTimeStamp obTimeStamp;
	int m_iWriteTimeouts=0;
	bool bUseAuxPins = false;

	void txDataOld(const char * pData, int iLength);
	bool txData(const  unsigned char  * pData, unsigned int iLength);

    bool addTxData(const char * pData, int iLength);
    bool hasRoom(int iNumberBytes);
    void process();

	unsigned int m_iAcutalBaudRate =0;
	
	rpUART() = default;
	rpUART(int iUARTModule, int iPIN_UARTTx, int iPIN_UARTRx, int iPIN_UART_CTS, int iPIN_UART_RTS);

	void setPins(int iUARTModule, int iPIN_UARTTx, int iPIN_UARTRx, int iPIN_UART_CTS, int iPIN_UART_RTS);

	bool init(unsigned int iBaudRate, bool bEnableHS_CTS,  bool bEnableHS_RTS, unsigned int iDataBits,
			unsigned int iStopBits, unsigned int iParity, bool bUseTxDMA);
	
	int rxDataCount();
	int rxReadData(unsigned char * pData, int iLength);
	
	void reclaimPinsForSpecialFunction(bool bEnableHS_CTS, bool bEnableHS_RTS);
};
