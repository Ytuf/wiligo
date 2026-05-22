// Created by bkidwell 5/8/26
// Unified serial communication class supporting both hardware UART and PIO UART

#pragma once

#include "rpUART.h"
#include "rpUARTTxPIO.h"
#include "rpUARTRxPIO.h"

struct rpSerialCommConfig {
    bool bHardwareUART      = false;     // true: hardware UART (uart0/uart1); false: PIO UART
    int  iModuleIndex       = 0;         // hw UART # (0/1) or PIO module # (0/1/2)
    int  iTxPin             = 0;         // GPIO for TX
    int  iRxPin             = 0;         // GPIO for RX
    int  iCtsPin            = -1;        // GPIO for CTS; -1 disables CTS gating
    int  iRtsPin            = -1;        // GPIO for RTS; -1 disables RTS drive
    int  iTxStateMachine    = -1;        // PIO state machine for TX; ignored when bHardwareUART
    int  iRxStateMachine    = -1;        // PIO state machine for RX; ignored when bHardwareUART
    int  iStartInstruction  = 0;         // PIO instruction slot for RX program; chain via prev instance's getLastInstruction()
    unsigned int iBaudRate  = 115200;    // bits/sec
    unsigned int iDataBits  = 8;         // data bits per frame
    unsigned int iStopBits  = 1;         // stop bits per frame
    unsigned int iParity    = 0;         // 0=none, 1=even, 2=odd
    bool bTxAuxPin          = false;     // HW UART: TX pin uses GPIO_FUNC_UART_AUX (F11) instead of standard F2
    bool bRxAuxPin          = false;     // HW UART: RX pin uses GPIO_FUNC_UART_AUX (F11) instead of standard F2
    bool bUseTxDMA          = false;     // drain TX via DMA from a software buffer
    int  iTxDMAChannel      = -1;        // -1 = SDK auto-claim; >=0 = use this specific channel
    bool bUseRxDMA          = true;      // PIO path: feed an endless DMA ring from RX FIFO so >8 byte bursts don't overflow
    int  iRxDMAChannel      = -1;        // -1 = SDK auto-claim; >=0 = use this specific channel
};

class rpSerialComm {
private:
    static constexpr int kPioTxBuffMax = 1024;
    static constexpr int kRxRingBits   = 8;
    static constexpr int kRxRingSize   = 1 << kRxRingBits;  // 256 bytes

    // Everything PIO mode needs.
    struct PioStorage {
        rpUARTTxPIO obUARTTxPIO;
        rpUARTRxPIO obUARTRxPIO;
        unsigned char btBufferTxA[kPioTxBuffMax];
        unsigned char btBufferTxB[kPioTxBuffMax];
        alignas(kRxRingSize) unsigned char btRxRing[kRxRingSize];  // DMA ring mode needs 256-byte aligned base
    };

    // Only one of these two is alive at a time. The union picks whichever is
    // larger and ensures correct alignment for both. init() constructs the
    // right one; the destructor tears it down.
    union UMode {
        rpUART     hw;
        PioStorage pio;
        UMode() {}    // do nothing — init() will construct the chosen variant
        ~UMode() {}   // do nothing — ~rpSerialComm destroys the chosen variant
    };

    rpSerialCommConfig m_cfg;
    UMode m_mode;
    bool  m_bInitialized = false;

    rpDMA m_dmaTx;
    rpDMA m_dmaRx;
    int   m_iRxDmaChannel     	= -1;
    unsigned int m_iRxReadTotal = 0;
	bool  m_bRxDMAEnabled     	= false;
    bool  m_bCurrentBufferIsA 	= true;
    int   m_iBufferTxACount   	= 0;
    int   m_iBufferTxBCount   	= 0;
    bool  m_bDMAInProcess     	= false;

    void setupPioDMATransmit(bool bUseBuffA);
    void setupPioRxDMA();

public:
    rpSerialComm() = default;
    ~rpSerialComm();

    void init(const rpSerialCommConfig& cfg);

    // PIO TX DREQ for a given (module, state-machine) pair.
    static rpDMAPeripheralRequest pioTxDreqFor(int iModuleIndex, int iStateMachine);

    bool txData(const unsigned char* pData, unsigned int iLength);
    int  rxReadData(unsigned char* pData, int iLength);
    int  rxDataCount();

    bool addTxData(const char* pData, int iLength);
    bool hasRoom(int iNumberBytes);
    void process();

    int  bytesRoomAvailable();
    void setupPinFunction();
    int  readByteWithBreak(unsigned char& btByte, int& iIsBreak);

    void reclaimPinsForSpecialFunction(bool bEnableHS_CTS, bool bEnableHS_RTS);

    int getLastInstruction();
};
