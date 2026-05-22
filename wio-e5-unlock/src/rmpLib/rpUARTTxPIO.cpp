//
// Created by daver on 1/27/2025.
//

#include "rpUARTTxPIO.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"


/*

.side_set 1 opt

; An 8n1 UART transmit program.
; OUT pin 0 and side-set pin 0 are both mapped to UART TX pin.

    pull       side 1 [7]  ; Assert stop bit, or stall with line in idle state
    set x, 7   side 0 [7]  ; Preload bit counter, assert start bit for 8 clocks
bitloop:                   ; This loop will run 8 times (8n1 UART)
    out pins, 1            ; Shift 1 bit from OSR to the first OUT pin
    jmp x-- bitloop   [6]  ; Each loop iteration is 8 cycles.


% c-sdk {
#include "hardware/clocks.h"

static inline void uart_tx_program_init(PIO pio, uint sm, uint offset, uint pin_tx, uint baud) {
    // Tell PIO to initially drive output-high on the selected pin, then map PIO
    // onto that pin with the IO muxes.
    pio_sm_set_pins_with_mask64(pio, sm, 1ull << pin_tx, 1ull << pin_tx);
    pio_sm_set_pindirs_with_mask64(pio, sm, 1ull << pin_tx, 1ull << pin_tx);
    pio_gpio_init(pio, pin_tx);

    pio_sm_config c = uart_tx_program_get_default_config(offset);

    // OUT shifts to right, no autopull
    sm_config_set_out_shift(&c, true, false, 32);

    // We are mapping both OUT and side-set to the same pin, because sometimes
    // we need to assert user data onto the pin (with OUT) and sometimes
    // assert constant values (start/stop bit)
    sm_config_set_out_pins(&c, pin_tx, 1);
    sm_config_set_sideset_pins(&c, pin_tx);

    // We only need TX, so get an 8-deep FIFO!
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // SM transmits 1 bit per 8 execution cycles.
    float div = (float)clock_get_hz(clk_sys) / (8 * baud);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static inline void uart_tx_program_putc(PIO pio, uint sm, char c) {
    pio_sm_put_blocking(pio, sm, (uint32_t)c);
}

static inline void uart_tx_program_puts(PIO pio, uint sm, const char *s) {
    while (*s)
        uart_tx_program_putc(pio, sm, *s++);
}

%}


*
 */

int  __not_in_flash_func(rpUARTTxPIO::writeBytes)(const unsigned char* btBuffer, int iLength)
{
    int iWritten = 0;
    while (iWritten < iLength && bytesRoomAvailable() > 0) {
        obPIO.writeTxFIFO(btBuffer[iWritten]);
        iWritten++;
    }
    return iWritten;
}

int  rpUARTTxPIO::bytesRoomAvailable()
{
    int level = obPIO.getTxFIFOCount();
    if (level >= m_iTxFifoDepth) return 0;
    return m_iTxFifoDepth - level;
}

void __not_in_flash_func(rpUARTTxPIO::setupPinFunction)()
{

    //io_bank0_hw->io[pStep->iArg1_WorkingReg].ctrl =

    if (m_iPIOModule==0)
        //gpio_set_function(m_iPin, GPIO_FUNC_PIO0);
        io_bank0_hw->io[m_iPin].ctrl = GPIO_FUNC_PIO0;
    else if (m_iPIOModule==1)
        io_bank0_hw->io[m_iPin].ctrl = GPIO_FUNC_PIO1;
    else
        io_bank0_hw->io[m_iPin].ctrl =GPIO_FUNC_PIO2;
}

void rpUARTTxPIO::init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate)
{
    init(iPin, iPIOModule, iStateMachineIndex, iStartInstruction, iBaudRate, /*iCtsPin=*/-1);
}

void rpUARTTxPIO::init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate, int iCtsPin)
{
    m_iPIOModule = iPIOModule;
    m_iPin = iPin;
    m_iCtsPin = iCtsPin;

    obPIO.init(iPIOModule,iStateMachineIndex,iStartInstruction);

    float div = (float)clock_get_hz(clk_sys) / (8.0 * (float)iBaudRate);

    obPIO.setupClockPeriodByDiv(div);

    obPIO.setupFIFOs(false, false, 0, false, true, false, 32, true);
    m_iTxFifoDepth = obPIO.getTxFifoDepth();

    // When CTS is enabled, point IN base at the CTS pin so the WAIT instruction
    // (which uses relative-pin form, index 0) reads CTS.
    int iInBase = (iCtsPin >= 0) ? iCtsPin : 0;
    obPIO.setupPins(1, 0, 1, iInBase, iPin, 0, iPin, 0, true);

    switch (iPIOModule) {
        case 0:
            gpio_set_function(iPin, GPIO_FUNC_PIO0);
            break;
        case 1:
            gpio_set_function(iPin, GPIO_FUNC_PIO1);
            break;
        case 2:
        default:
            gpio_set_function(iPin, GPIO_FUNC_PIO2);
            break;
    }
    if (iCtsPin >= 0) {
        switch (iPIOModule) {
            case 0:  gpio_set_function(iCtsPin, GPIO_FUNC_PIO0); break;
            case 1:  gpio_set_function(iCtsPin, GPIO_FUNC_PIO1); break;
            default: gpio_set_function(iCtsPin, GPIO_FUNC_PIO2); break;
        }
    }

    obPIO.encode_begin();
    obPIO.encode_wrapTarget();
    obPIO.encode_addLabel('a');

  /*
   * 8n1 UART transmit program.
   * OUT pin 0 and side-set pin 0 are both mapped to UART TX pin.
   *
   *     pull       side 1 [7]  ; Assert stop bit, or stall with line in idle state
   *  [  wait 1 pin <cts>     ] ; emitted only when iCtsPin >= 0
   *     set x, 7   side 0 [7]  ; Preload bit counter, assert start bit for 8 clocks
   * bitloop:                   ; This loop will run 8 times (8n1 UART)
   *     out pins, 1            ; Shift 1 bit from OSR to the first OUT pin
   *     jmp x-- bitloop   [6]  ; Each loop iteration is 8 cycles.
   */

    obPIO.encode_pull(1,7,false,true,true) ;
    if (iCtsPin >= 0) {
        // Active-low CTS
        // Source = pin (relative to SM IN base; we set IN base = iCtsPin so index = 0).
        obPIO.encode_wait(0, 0, /*polarity=*/false, erpPIOWaitSource::pin, /*index=*/0);
    }
    obPIO.encode_set(0,7,erpPIOSetDestination::x,7, true);
    obPIO.encode_addLabel('b');
    obPIO.encode_out(0,0,erpPIOOutDestination::pins,1);
    obPIO.encode_jmp(0,6,erpPIOJumpCondition::XNotZeroPostDec,'b');


    obPIO.encode_wrap();
    obPIO.encode_end();


    obPIO.start();

}


