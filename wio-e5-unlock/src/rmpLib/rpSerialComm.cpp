#include "rpSerialComm.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include <cassert>
#include <cstring>

void rpSerialComm::init(const rpSerialCommConfig& cfg) {
    m_cfg = cfg;
    if (m_cfg.bHardwareUART) {
        new (&m_mode.hw) rpUART();   // construct the HW variant inside the union
        m_mode.hw.setPins(m_cfg.iModuleIndex, m_cfg.iTxPin, m_cfg.iRxPin,
                          m_cfg.iCtsPin, m_cfg.iRtsPin);
        bool bCts = (m_cfg.iCtsPin >= 0);
        bool bRts = (m_cfg.iRtsPin >= 0);
        m_mode.hw.init(m_cfg.iBaudRate, bCts, bRts,
                       m_cfg.iDataBits, m_cfg.iStopBits, m_cfg.iParity,
                       m_cfg.bUseTxDMA);
        // rpUART::init() sets both TX and RX to GPIO_FUNC_UART. Override per-pin
        // for boards where one (or both) pin only exposes UART on F11.
        if (m_cfg.bTxAuxPin) gpio_set_function(m_cfg.iTxPin, GPIO_FUNC_UART_AUX);
        if (m_cfg.bRxAuxPin) gpio_set_function(m_cfg.iRxPin, GPIO_FUNC_UART_AUX);
    } else {
        new (&m_mode.pio) PioStorage();   // construct the PIO variant inside the union

        // RP2350 PIO addresses a 32-pin window selected by gpiobase (0 → GPIO 0..31,
        // 16 → GPIO 16..47). If any pin is > 31, switch this PIO to the upper window
        // before the per-program init writes pinctrl. Both SMs share gpiobase, so a
        // config that mixes a sub-16 pin with a >31 pin is unrepresentable.
        bool bAnyAbove31 = false;
        bool bAnyBelow16 = false;
        const int pins[] = { m_cfg.iTxPin, m_cfg.iRxPin, m_cfg.iCtsPin, m_cfg.iRtsPin };
        for (int p : pins) {
            if (p < 0) continue;
            if (p > 31)      bAnyAbove31 = true;
            else if (p < 16) bAnyBelow16 = true;
        }
        assert(!(bAnyAbove31 && bAnyBelow16));
        if (bAnyAbove31) {
            m_mode.pio.obUARTTxPIO.obPIO.enableGPIOBase16(true);
            m_mode.pio.obUARTRxPIO.obPIO.enableGPIOBase16(true);
        }

        m_mode.pio.obUARTRxPIO.init(m_cfg.iRxPin, m_cfg.iModuleIndex, m_cfg.iRxStateMachine,
                                    /*startInstr=*/m_cfg.iStartInstruction, m_cfg.iBaudRate, m_cfg.iRtsPin);

        m_mode.pio.obUARTTxPIO.init(m_cfg.iTxPin, m_cfg.iModuleIndex, m_cfg.iTxStateMachine,
                                    /*startInstr=*/m_mode.pio.obUARTRxPIO.obPIO.getLastInstruction(),
                                    m_cfg.iBaudRate, m_cfg.iCtsPin);

        if (m_cfg.bUseTxDMA) {
            m_dmaTx.setupPeripheralRequest(pioTxDreqFor(m_cfg.iModuleIndex, m_cfg.iTxStateMachine));
            if (m_cfg.iTxDMAChannel >= 0) {
                m_dmaTx.init(m_cfg.iTxDMAChannel);
            } else {
                m_dmaTx.initClaimFreeDMAChannel();
            }
        }

        if (m_cfg.bUseRxDMA) {
            if (m_cfg.iRxDMAChannel >= 0) {
                m_dmaRx.init(m_cfg.iRxDMAChannel);
            } else {
                m_dmaRx.initClaimFreeDMAChannel();
            }
            m_iRxDmaChannel = m_dmaRx.getChannel();
            setupPioRxDMA();
            m_bRxDMAEnabled = true;
        }
    }
    m_bInitialized = true;
}

rpSerialComm::~rpSerialComm() {
    if (m_bDMAInProcess) m_dmaTx.abort();
    if (m_bRxDMAEnabled) m_dmaRx.abort();
    if (m_bInitialized) {
        if (m_cfg.bHardwareUART) m_mode.hw.~rpUART();
        else                     m_mode.pio.~PioStorage();
    }
}

rpDMAPeripheralRequest rpSerialComm::pioTxDreqFor(int iModuleIndex, int iStateMachine) {
#ifdef PICO_RP2350
    assert(iModuleIndex >= 0 && iModuleIndex < 3);
    assert(iStateMachine >= 0 && iStateMachine < 4);
    static const rpDMAPeripheralRequest kPioTxDreq[3][4] = {
        {
            rpDMAPeripheralRequest::pio0Tx0,
            rpDMAPeripheralRequest::pio0Tx1,
            rpDMAPeripheralRequest::pio0Tx2,
            rpDMAPeripheralRequest::pio0Tx3
        },
        {
            rpDMAPeripheralRequest::pio1Tx0,
            rpDMAPeripheralRequest::pio1Tx1,
            rpDMAPeripheralRequest::pio1Tx2,
            rpDMAPeripheralRequest::pio1Tx3
        },
        {
            rpDMAPeripheralRequest::pio2Tx0,
            rpDMAPeripheralRequest::pio2Tx1,
            rpDMAPeripheralRequest::pio2Tx2,
            rpDMAPeripheralRequest::pio2Tx3
        }
    };
#else
    assert(iModuleIndex >= 0 && iModuleIndex < 2);
    assert(iStateMachine >= 0 && iStateMachine < 4);
    static const rpDMAPeripheralRequest kPioTxDreq[2][4] = {
        {
            rpDMAPeripheralRequest::pio0Tx0,
            rpDMAPeripheralRequest::pio0Tx1,
            rpDMAPeripheralRequest::pio0Tx2,
            rpDMAPeripheralRequest::pio0Tx3
        },
        {
            rpDMAPeripheralRequest::pio1Tx0,
            rpDMAPeripheralRequest::pio1Tx1,
            rpDMAPeripheralRequest::pio1Tx2,
            rpDMAPeripheralRequest::pio1Tx3
        }
    };
#endif
    return kPioTxDreq[iModuleIndex][iStateMachine];
}

bool rpSerialComm::txData(const unsigned char* pData, unsigned int iLength) {
    if (!m_bInitialized) return false;
    if (m_cfg.bHardwareUART) {
        return m_mode.hw.txData(pData, iLength);
    }

    constexpr unsigned int kMaxSpinPerByte = 1000000;
    unsigned int sent = 0;
    while (sent < iLength) {
        int n = m_mode.pio.obUARTTxPIO.writeBytes(pData + sent, (int)(iLength - sent));
        if (n > 0) {
            sent += (unsigned int)n;
            continue;
        }
        unsigned int spin = 0;
        while (m_mode.pio.obUARTTxPIO.bytesRoomAvailable() == 0) {
            if (++spin > kMaxSpinPerByte) return false;
            tight_loop_contents();
        }
    }
    return true;
}

int rpSerialComm::rxReadData(unsigned char* pData, int iLength) {
    if (!m_bInitialized) return 0;
    if (m_cfg.bHardwareUART) {
        return m_mode.hw.rxReadData(pData, iLength);
    }
    if (m_bRxDMAEnabled) {
        int avail = rxDataCount();
        int n = (iLength < avail) ? iLength : avail;
        if (n <= 0) return 0;
        unsigned int read_off = m_iRxReadTotal & (kRxRingSize - 1);
        // Copy may straddle the ring boundary; do up to two memcpys.
        unsigned int first = kRxRingSize - read_off;
        if ((unsigned int)n <= first) {
            std::memcpy(pData, m_mode.pio.btRxRing + read_off, n);
        } else {
            std::memcpy(pData, m_mode.pio.btRxRing + read_off, first);
            std::memcpy(pData + first, m_mode.pio.btRxRing, n - first);
        }
        m_iRxReadTotal += (unsigned int)n;
        return n;
    }
    int avail = m_mode.pio.obUARTRxPIO.bytesAvailable();
    int n = (iLength < avail) ? iLength : avail;
    return m_mode.pio.obUARTRxPIO.readBytes(pData, n) == 0 ? n : 0;
}

int rpSerialComm::rxDataCount() {
    if (!m_bInitialized) return 0;
    if (m_cfg.bHardwareUART) {
        return m_mode.hw.rxDataCount();
    }
    if (m_bRxDMAEnabled) {

        unsigned int total_written = 0xFFFFFFFFu - (unsigned int)m_dmaRx.getTransferCount();
        unsigned int avail = total_written - m_iRxReadTotal;
        if (avail > (unsigned int)kRxRingSize) {
            m_iRxReadTotal = total_written - (unsigned int)kRxRingSize;
            avail = (unsigned int)kRxRingSize;
        }
        return (int)avail;
    }
    return m_mode.pio.obUARTRxPIO.bytesAvailable();
}

bool rpSerialComm::addTxData(const char* pData, int iLength) {
    if (!m_bInitialized) return false;
    if (m_cfg.bHardwareUART) {
        return m_mode.hw.addTxData(pData, iLength);
    }
    if (!m_cfg.bUseTxDMA) return false;
    if (!hasRoom(iLength)) return false;
    unsigned char* pBuf;
    int* pCount;
    if (m_bCurrentBufferIsA) {
        pBuf = m_mode.pio.btBufferTxA;
        pCount = &m_iBufferTxACount;
    } else {
        pBuf = m_mode.pio.btBufferTxB;
        pCount = &m_iBufferTxBCount;
    }
    std::memcpy(pBuf + *pCount, pData, iLength);
    *pCount += iLength;
    return true;
}

bool rpSerialComm::hasRoom(int iNumberBytes) {
    if (!m_bInitialized) return false;
    if (m_cfg.bHardwareUART) {
        return m_mode.hw.hasRoom(iNumberBytes);
    }
    if (!m_cfg.bUseTxDMA) return false;
    int count = m_bCurrentBufferIsA ? m_iBufferTxACount : m_iBufferTxBCount;
    return (count + iNumberBytes) < kPioTxBuffMax;
}

void rpSerialComm::process() {
    if (!m_bInitialized) return;
    if (m_cfg.bHardwareUART) {
        m_mode.hw.process();
        return;
    }
    if (!m_cfg.bUseTxDMA) return;

    if (m_bDMAInProcess) {
        if (!m_dmaTx.isComplete()) return;
        m_bDMAInProcess = false;
    }

    // Switch active buffer; drain the just-filled side via DMA.
    if (m_bCurrentBufferIsA) {
        if (m_iBufferTxACount > 0) {
            setupPioDMATransmit(true);
            m_bCurrentBufferIsA = false;
        }
    } else {
        if (m_iBufferTxBCount > 0) {
            setupPioDMATransmit(false);
            m_bCurrentBufferIsA = true;
        }
    }
}

void rpSerialComm::setupPioDMATransmit(bool bUseBuffA) {
    PIO pio = pio0;
    switch (m_cfg.iModuleIndex) {
        case 0:  pio = pio0; break;
        case 1:  pio = pio1; break;
#ifdef PICO_RP2350
        case 2:  pio = pio2; break;
#endif
        default: pio = pio0; break;
    }
    unsigned char* pBuf = bUseBuffA ? m_mode.pio.btBufferTxA : m_mode.pio.btBufferTxB;
    int count = bUseBuffA ? m_iBufferTxACount : m_iBufferTxBCount;

    m_dmaTx.setupTransfer((void*)&pio->txf[m_cfg.iTxStateMachine], pBuf,
                          /*bAutoIncrementDestination=*/false,
                          /*bAutoIncrementSource=*/true,
                          rpDMATransferDataSize::byte, count);
    m_dmaTx.start();
    if (bUseBuffA) m_iBufferTxACount = 0;
    else            m_iBufferTxBCount = 0;
    m_bDMAInProcess = true;
}

void rpSerialComm::setupPioRxDMA() {
    PIO pio = pio0;
    switch (m_cfg.iModuleIndex) {
        case 0:  pio = pio0; break;
        case 1:  pio = pio1; break;
#ifdef PICO_RP2350
        case 2:  pio = pio2; break;
#endif
        default: pio = pio0; break;
    }

    dma_channel_config c = dma_channel_get_default_config(m_iRxDmaChannel);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, pio_get_dreq(pio, m_cfg.iRxStateMachine, /*is_tx=*/false));
    channel_config_set_ring(&c, /*write=*/true, kRxRingBits);

    unsigned char* pSrc = (unsigned char*)&pio->rxf[m_cfg.iRxStateMachine];
    pSrc += 3;

    dma_channel_configure(m_iRxDmaChannel, &c,
                          m_mode.pio.btRxRing, pSrc,
                          0xFFFFFFFFu,
                          /*trigger=*/true);
}

int rpSerialComm::getLastInstruction() {
    if (!m_bInitialized || m_cfg.bHardwareUART) return 0;
    return m_mode.pio.obUARTTxPIO.obPIO.getLastInstruction();
}

int rpSerialComm::bytesRoomAvailable() {
    if (!m_bInitialized) return 0;
    if (m_cfg.bHardwareUART) return m_mode.hw.hasRoom(1) ? 1 : 0;
    return m_mode.pio.obUARTTxPIO.bytesRoomAvailable();
}

void rpSerialComm::setupPinFunction() {
    if (!m_bInitialized) return;
    if (m_cfg.bHardwareUART) {
        m_mode.hw.reclaimPinsForSpecialFunction(false, false);
        return;
    }
    m_mode.pio.obUARTTxPIO.setupPinFunction();
}

int rpSerialComm::readByteWithBreak(unsigned char& btByte, int& iIsBreak) {
    if (!m_bInitialized) { iIsBreak = 0; return -1; }
    if (m_cfg.bHardwareUART) {
        iIsBreak = 0;
        return m_mode.hw.rxReadData(&btByte, 1) > 0 ? 0 : -1;
    }
    return m_mode.pio.obUARTRxPIO.readByteWithBreak(btByte, iIsBreak);
}

void rpSerialComm::reclaimPinsForSpecialFunction(bool bEnableHS_CTS, bool bEnableHS_RTS) {
    if (!m_bInitialized) return;
    if (m_cfg.bHardwareUART) {
        m_mode.hw.reclaimPinsForSpecialFunction(bEnableHS_CTS, bEnableHS_RTS);
        if (m_cfg.bTxAuxPin) gpio_set_function(m_cfg.iTxPin, GPIO_FUNC_UART_AUX);
        if (m_cfg.bRxAuxPin) gpio_set_function(m_cfg.iRxPin, GPIO_FUNC_UART_AUX);
        return;
    }
    gpio_function_t fn = GPIO_FUNC_PIO0;
    switch (m_cfg.iModuleIndex) {
        case 0:  fn = GPIO_FUNC_PIO0; break;
        case 1:  fn = GPIO_FUNC_PIO1; break;
#ifdef PICO_RP2350
        case 2:  fn = GPIO_FUNC_PIO2; break;
#endif
        default: fn = GPIO_FUNC_PIO0; break;
    }
    gpio_set_function(m_cfg.iTxPin, fn);
    gpio_set_function(m_cfg.iRxPin, fn);
    if (bEnableHS_CTS && m_cfg.iCtsPin >= 0) gpio_set_function(m_cfg.iCtsPin, fn);
    if (bEnableHS_RTS && m_cfg.iRtsPin >= 0) gpio_set_function(m_cfg.iRtsPin, fn);
}
