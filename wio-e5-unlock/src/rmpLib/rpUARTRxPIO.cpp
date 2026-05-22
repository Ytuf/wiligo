//
// Created by daver on 1/27/2025.
//

#include "rpUARTRxPIO.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"



/*

; Minimum viable 8n1 UART receiver. Wait for the start bit, then sample 8 bits
; with the correct timing.
; IN pin 0 is mapped to the GPIO used as UART RX.
; Autopush must be enabled, with a threshold of 8.

    wait 0 pin 0        ; Wait for start bit
    set x, 7 [10]       ; Preload bit counter, delay until eye of first data bit
bitloop:                ; Loop 8 times
    in pins, 1          ; Sample data
    jmp x-- bitloop [6] ; Each iteration is 8 cycles

% c-sdk {
#include "hardware/clocks.h"
#include "hardware/gpio.h"

static inline void uart_rx_mini_program_init(PIO pio, uint sm, uint offset, uint pin, uint baud) {
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);
    pio_gpio_init(pio, pin);
    gpio_pull_up(pin);

    pio_sm_config c = uart_rx_mini_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin); // for WAIT, IN
    // Shift to right, autopush enabled
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    // SM transmits 1 bit per 8 execution cycles.
    float div = (float)clock_get_hz(clk_sys) / (8 * baud);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
%}

.program uart_rx

; Slightly more fleshed-out 8n1 UART receiver which handles framing errors and
; break conditions more gracefully.
; IN pin 0 and JMP pin are both mapped to the GPIO used as UART RX.

start:
    wait 0 pin 0        ; Stall until start bit is asserted
    set x, 7    [10]    ; Preload bit counter, then delay until halfway through
bitloop:                ; the first data bit (12 cycles incl wait, set).
    in pins, 1          ; Shift data bit into ISR
    jmp x-- bitloop [6] ; Loop 8 times, each loop iteration is 8 cycles
    jmp pin good_stop   ; Check stop bit (should be high)

    irq 4 rel           ; Either a framing error or a break. Set a sticky flag,
    wait 1 pin 0        ; and wait for line to return to idle state.
    jmp start           ; Don't push data if we didn't see good framing.

good_stop:              ; No delay before returning to start; a little slack is
    push                ; important in case the TX clock is slightly too fast.


% c-sdk {
static inline void uart_rx_program_init(PIO pio, uint sm, uint offset, uint pin, uint baud) {
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);
    pio_gpio_init(pio, pin);
    gpio_pull_up(pin);

    pio_sm_config c = uart_rx_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin); // for WAIT, IN
    sm_config_set_jmp_pin(&c, pin); // for JMP
    // Shift to right, autopush disabled
    sm_config_set_in_shift(&c, true, false, 32);
    // Deeper FIFO as we're not doing any TX
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    // SM transmits 1 bit per 8 execution cycles.
    float div = (float)clock_get_hz(clk_sys) / (8 * baud);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static inline char uart_rx_program_getc(PIO pio, uint sm) {
    // 8-bit read from the uppermost byte of the FIFO, as data is left-justified
    io_rw_8 *rxfifo_shift = (io_rw_8*)&pio->rxf[sm] + 3;
    while (pio_sm_is_rx_fifo_empty(pio, sm))
        tight_loop_contents();
    return (char)*rxfifo_shift;
}

%}
*/


void rpUARTRxPIO::init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate)
{
    init(iPin, iPIOModule, iStateMachineIndex, iStartInstruction, iBaudRate, /*iRtsPin=*/-1);
}

void rpUARTRxPIO::init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate, int iRtsPin)
{
    m_iRtsPin = iRtsPin;
    obPIO.init(iPIOModule,iStateMachineIndex,iStartInstruction);

    float div = (float)clock_get_hz(clk_sys) / (8.0 * (float)iBaudRate);

    obPIO.setupClockPeriodByDiv(div);

#ifdef FLEXINGATE
    obPIO.setupFIFOs(true, true, 9, true, false, false, 32, true);
#else
    obPIO.setupFIFOs(true, true, 8, true, false, false, 32, true);
#endif

    if (iRtsPin >= 0) {
        // OUT pin = RTS, width 1, so 'mov pins, !status' drives the RTS line.
        obPIO.setupPins(0, 0, 1, iPin, 0, 0, iRtsPin, iPin, false);
        // STATUS_SEL=1 → RX FIFO; STATUS_N=6 → ready when fewer than 6 bytes queued.
        obPIO.setupStatus(/*iSel=*/1, /*iN=*/6);
    } else {
        obPIO.setupPins(0, 0, 0, iPin, 0, 0, 0, iPin, false);
    }

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
    if (iRtsPin >= 0) {
        switch (iPIOModule) {
            case 0:  gpio_set_function(iRtsPin, GPIO_FUNC_PIO0); break;
            case 1:  gpio_set_function(iRtsPin, GPIO_FUNC_PIO1); break;
            default: gpio_set_function(iRtsPin, GPIO_FUNC_PIO2); break;
        }
    }

    #define RXPIO_START     'a'
    #define RXPIO_BITLOOP   'b'
    #define RXPIO_GOODSTOP  'c'

    obPIO.encode_begin();
    obPIO.encode_wrapTarget();
    if (iRtsPin >= 0) {
        // mov pins, !status — drives RTS based on RX FIFO occupancy each loop.
        obPIO.encode_mov(0, 0, erpPIOMovDestination::pins,
                         erpPIOMovOperation::invert, erpPIOMovSource::status);
    }

    /*
   start:
   wait 0 pin 0        ; Stall until start bit is asserted
   set x, 7    [10]    ; Preload bit counter, then delay until halfway through
bitloop:                ; the first data bit (12 cycles incl wait, set).
   in pins, 1          ; Shift data bit into ISR
   jmp x-- bitloop [6] ; Loop 8 times, each loop iteration is 8 cycles
   jmp pin good_stop   ; Check stop bit (should be high)

   irq 4 rel           ; Either a framing error or a break. Set a sticky flag,
   wait 1 pin 0        ; and wait for line to return to idle state.
   jmp start           ; Don't push data if we didn't see good framing.

good_stop:              ; No delay before returning to start; a little slack is
   push                ; important in case the TX clock is slightly too fast.


    obPIO.encode_addLabel(RXPIO_START);
    obPIO.encode_wait(0,0,0,erpPIOWaitSource::pin,iPin);
    obPIO.encode_set(0,10,erpPIOSetDestination::x,7);
    obPIO.encode_addLabel(RXPIO_BITLOOP);
    obPIO.encode_in(0,0,erpPIOInSource::pins,1);
    obPIO.encode_jmp(0,6,erpPIOJumpCondition::XNotZeroPostDec,RXPIO_BITLOOP);
    obPIO.encode_jmp(0,0,erpPIOJumpCondition::pin,RXPIO_GOODSTOP);
    obPIO.encode_irq(0,0,0,0,4);
    obPIO.encode_wait(0,0,1,erpPIOWaitSource::pin,iPin);
    obPIO.encode_jmp(0,0,erpPIOJumpCondition::always,RXPIO_START);
    obPIO.encode_addLabel(RXPIO_GOODSTOP);
    obPIO.encode_push(0,0,0,1);  ///
    */
    /*
    wait 0 pin 0        ; Wait for start bit
    set x, 7 [10]       ; Preload bit counter, delay until eye of first data bit
bitloop:                ; Loop 8 times
    in pins, 1          ; Sample data
    jmp x-- bitloop [6] ; Each iteration is 8 cycles
     **/

#ifdef FLEXINGATE

    obPIO.encode_wait(0,0,1,erpPIOWaitSource::pin,0); // wait for stop bit
    obPIO.encode_wait(0,0,0,erpPIOWaitSource::pin,0);
    obPIO.encode_set(0,10,erpPIOSetDestination::x,7);  // sample the stop bit
    obPIO.encode_addLabel(RXPIO_BITLOOP);
    obPIO.encode_in(0,0,erpPIOInSource::pins,1);
    obPIO.encode_jmp(0,6,erpPIOJumpCondition::XNotZeroPostDec,RXPIO_BITLOOP);
    obPIO.encode_nop(0,0);
    obPIO.encode_in(0,0,erpPIOInSource::pins,1);
    //obPIO.encode_nop(0,0);


#else


    obPIO.encode_wait(0,0,0,erpPIOWaitSource::pin,0);
    obPIO.encode_set(0,10,erpPIOSetDestination::x,7);
    obPIO.encode_addLabel(RXPIO_BITLOOP);
    obPIO.encode_in(0,0,erpPIOInSource::pins,1);
    obPIO.encode_jmp(0,6,erpPIOJumpCondition::XNotZeroPostDec,RXPIO_BITLOOP);
#endif



    obPIO.encode_wrap();
    obPIO.encode_end();


    obPIO.start();
}


int __not_in_flash_func(rpUARTRxPIO::readByteWithBreak)(unsigned char & btBuffer, int & iIsBreak)
{

    unsigned int data = obPIO.readRxFIFO();
    iIsBreak = 1;
#ifdef FLEXINGATE
        btBuffer = (data >> 23) & 0xff;
#else
        btBuffer = data >> 24;
#endif
    if (data & 0x80'00'00'00)
        iIsBreak = 0;
    return 0;
}

int __not_in_flash_func(rpUARTRxPIO::readBytes)(unsigned char* btBuffer, int iLength) {
    for (int i=0; i<iLength; i++) {
        unsigned int data = obPIO.readRxFIFO();
#ifdef FLEXINGATE
        btBuffer[i] = data >> 23;
#else
        btBuffer[i] = data >> 24;
#endif
    }
    return 0;
}

int __not_in_flash_func(rpUARTRxPIO::bytesAvailable()) {
    return obPIO.getRxFIFOCount();
}
