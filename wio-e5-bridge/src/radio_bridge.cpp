#include "radio_bridge.h"
#include "uart_protocol.h"
#include <RadioLib.h>
#include <SubGhz.h>

STM32WLx radio = new STM32WLx_Module();

static volatile bool tx_done_flag = false;
static volatile bool rx_done_flag = false;
static bool radio_begun = false;

volatile int32_t  g_radio_begin_result   __attribute__((used)) = 0x7FFFFFFF;
volatile int32_t  g_radio_standby_result __attribute__((used)) = 0x7FFFFFFF;
volatile uint32_t g_rcc_csr_pre          __attribute__((used)) = 0;
volatile uint32_t g_rcc_csr_post         __attribute__((used)) = 0;
volatile uint32_t g_pwr_sr2_pre          __attribute__((used)) = 0;
volatile uint8_t  g_version_str[16]      __attribute__((used)) = {0};
volatile uint32_t g_version_read_status  __attribute__((used)) = 0;

volatile int32_t  g_configure_status __attribute__((used)) = 0x7FFFFFFF;
volatile uint32_t g_cfg_freq_hz      __attribute__((used)) = 0;
volatile uint8_t  g_cfg_bw_code      __attribute__((used)) = 0;
volatile uint8_t  g_cfg_sf           __attribute__((used)) = 0;
volatile uint8_t  g_cfg_cr           __attribute__((used)) = 0;
volatile int8_t   g_cfg_power        __attribute__((used)) = 0;
volatile uint16_t g_cfg_preamble     __attribute__((used)) = 0;
volatile uint16_t g_cfg_syncword     __attribute__((used)) = 0;
volatile int32_t  g_cfg_sw_status    __attribute__((used)) = 0x7FFFFFFF;

volatile uint32_t g_rx_isr_count    __attribute__((used)) = 0;
volatile uint32_t g_rx_check_calls  __attribute__((used)) = 0;
volatile uint32_t g_rx_check_errors __attribute__((used)) = 0;
volatile int32_t  g_last_rx_len     __attribute__((used)) = 0;

volatile uint32_t g_diag_polls    __attribute__((used)) = 0;
volatile uint8_t  g_chip_status   __attribute__((used)) = 0;
volatile uint16_t g_chip_irq      __attribute__((used)) = 0;
volatile uint8_t  g_chip_rssi_raw __attribute__((used)) = 0;
volatile uint8_t  g_chip_rssi_max __attribute__((used)) = 0xFF;
volatile uint32_t g_preamble_seen __attribute__((used)) = 0;

static void diag_read_version_raw(void) {
    g_rcc_csr_pre = RCC->CSR;
    g_pwr_sr2_pre = PWR->SR2;

    LL_RCC_RF_EnableReset();
    delay(2);
    LL_RCC_RF_DisableReset();
    delay(10);

    g_rcc_csr_post = RCC->CSR;

    // SX126x ReadRegister: opcode 0x1D, addr_hi, addr_lo, NOP, then N data bytes.
    SubGhz.SPI.beginTransaction(SubGhz.spi_settings);
    SubGhz.setNssActive(true);

    SubGhz.SPI.transfer(0x1D);
    SubGhz.SPI.transfer(0x03);
    SubGhz.SPI.transfer(0x20);
    SubGhz.SPI.transfer(0x00);

    for (int i = 0; i < 16; i++) {
        g_version_str[i] = SubGhz.SPI.transfer(0x00);
    }
    g_version_read_status = 1;

    SubGhz.setNssActive(false);
    SubGhz.SPI.endTransaction();
}

// STM32WLx routes both TxDone and RxDone through DIO1 and RadioLib gives us a
// single callback; track which mode the radio is in so we can demux them.
static volatile bool tx_in_progress = false;
static void dio1_isr(void) {
    if (tx_in_progress) {
        tx_done_flag = true;
        tx_in_progress = false;
    } else {
        rx_done_flag = true;
        g_rx_isr_count++;
    }
}

volatile uint32_t g_pwr_sr2_pre_begin __attribute__((used)) = 0;
volatile uint32_t g_pwr_sr2_post_diag __attribute__((used)) = 0;

// WIO-E5 antenna switch is an external SP3T on PA4/PA5, NOT DIO2.
// IDLE: PA4=L PA5=L     RX: PA4=H PA5=L     TX_HP: PA4=L PA5=H
static const uint32_t rfswitch_pins[] = {
    PA4, PA5, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC,
};
static const Module::RfSwitchMode_t rfswitch_table[] = {
    {STM32WLx::MODE_IDLE,  {LOW,  LOW}},
    {STM32WLx::MODE_RX,    {HIGH, LOW}},
    {STM32WLx::MODE_TX_HP, {LOW,  HIGH}},
    END_OF_MODE_TABLE,
};

int radio_bridge_init(void) {
    // RF switch table MUST be registered before begin() or setOutputPower
    // returns INVALID_OUTPUT_POWER (-13).
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);

    // WIO-E5 TCXO is 1.7 V. 1.8 V causes enough drift to break RX CRC.
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 10, 8, 1.7f);
    g_radio_begin_result = state;
    if (state != RADIOLIB_ERR_NONE) return state;

    radio.setRxBoostedGainMode(true);

    // SX1262 RX sensitivity patch (set bit 0 of reg 0x8B5) — Semtech-recommended.
    {
        uint8_t v = 0;
        if (radio.readRegister(0x08B5, &v, 1) == RADIOLIB_ERR_NONE) {
            v |= 0x01;
            radio.writeRegister(0x08B5, &v, 1);
        }
    }

    radio.setCRC(RADIOLIB_SX126X_LORA_CRC_ON);

    radio.setDio1Action(dio1_isr);
    radio_begun = true;
    return RADIOLIB_ERR_NONE;
}

int radio_bridge_set_dio(const uint8_t *payload, uint16_t len) {
    if (len < DIO_PAYLOAD_SIZE) return -1;
    (void)payload;
    // Force TCXO = 1.7 V regardless of host-supplied value (Display CPU encodes
    // 1.8 V, which is wrong for WIO-E5).
    int state = radio.setTCXO(1.7f);
    if (state != RADIOLIB_ERR_NONE) return state;
    // DIO2-as-RF-switch is wrong on STM32WLx; the antenna switch is PA4/PA5.
    radio.setDio2AsRfSwitch(false);
    return state;
}

int radio_bridge_configure(const uint8_t *payload, uint16_t len) {
    if (len < CFG_PAYLOAD_SIZE) { g_configure_status = -1; return -1; }
    // Display CPU encodes multi-byte fields big-endian.
    uint32_t freq_hz = ((uint32_t)payload[CFG_FREQ_OFFSET + 0] << 24) |
                       ((uint32_t)payload[CFG_FREQ_OFFSET + 1] << 16) |
                       ((uint32_t)payload[CFG_FREQ_OFFSET + 2] <<  8) |
                       ((uint32_t)payload[CFG_FREQ_OFFSET + 3]);
    float freq_mhz = (float)freq_hz / 1000000.0f;
    uint8_t bw_code = payload[CFG_BW_OFFSET];
    float bw;
    switch (bw_code) {
        case 0: bw = 125.0; break;
        case 1: bw = 250.0; break;
        case 2: bw = 500.0; break;
        default: bw = 125.0; break;
    }
    uint8_t sf = payload[CFG_SF_OFFSET];
    uint8_t cr = payload[CFG_CR_OFFSET];
    int8_t power = (int8_t)payload[CFG_POWER_OFFSET];
    // STM32WLx HP-mode hardware range is -9 .. +22 dBm; RadioLib doesn't clamp.
    if (power > 22)  power = 22;
    if (power < -9)  power = -9;
    uint16_t preamble = ((uint16_t)payload[CFG_PREAMBLE_OFFSET + 0] << 8) |
                        ((uint16_t)payload[CFG_PREAMBLE_OFFSET + 1]);
    uint16_t syncword = ((uint16_t)payload[CFG_SYNCWORD_OFFSET + 0] << 8) |
                        ((uint16_t)payload[CFG_SYNCWORD_OFFSET + 1]);

    g_cfg_freq_hz  = freq_hz;
    g_cfg_bw_code  = bw_code;
    g_cfg_sf       = sf;
    g_cfg_cr       = cr;
    g_cfg_power    = power;
    g_cfg_preamble = preamble;
    g_cfg_syncword = syncword;

    int state;
    state = radio.setFrequency(freq_mhz);       if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    radio.calibrateImage(freq_mhz);
    state = radio.setBandwidth(bw);             if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    state = radio.setSpreadingFactor(sf);       if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    state = radio.setCodingRate(cr);            if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    state = radio.setOutputPower(power);        if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    state = radio.setPreambleLength(preamble);  if (state != RADIOLIB_ERR_NONE) { g_configure_status = state; return state; }
    // Display sends 16-bit sync field; Meshtastic LoRa sync word is the high byte.
    state = radio.setSyncWord((uint8_t)((syncword >> 8) & 0xFF));
    g_cfg_sw_status = state;
    g_configure_status = state;
    return state;
}

int radio_bridge_transmit(const uint8_t *data, uint16_t len) {
    tx_done_flag = false;
    tx_in_progress = true;
    return radio.startTransmit((uint8_t *)data, len);
}

int radio_bridge_check_tx_done(void) {
    if (tx_done_flag) {
        tx_done_flag = false;
        radio.finishTransmit();
        radio.startReceive();
        return 1;
    }
    return 0;
}

int radio_bridge_start_receive(void) {
    rx_done_flag = false;
    return radio.startReceive();
}

int radio_bridge_check_rx(uint8_t *buf, uint16_t buf_size, float *rssi, float *snr) {
    if (!rx_done_flag) return 0;
    rx_done_flag = false;
    g_rx_check_calls++;
    int len = radio.getPacketLength();
    // Every error path must call startReceive() — STM32WLx DIO1 is level-
    // triggered and the NVIC line is re-enabled by clearIrqStatus, which
    // only runs inside startReceive / readData.
    if (len <= 0 || (uint16_t)len > buf_size) {
        g_rx_check_errors++;
        g_last_rx_len = len;
        radio.startReceive();
        return -1;
    }
    int state = radio.readData(buf, len);
    if (state != RADIOLIB_ERR_NONE) {
        g_rx_check_errors++;
        g_last_rx_len = state;
        radio.startReceive();
        return -1;
    }
    *rssi = radio.getRSSI();
    *snr = radio.getSNR();
    g_last_rx_len = len;
    radio.startReceive();
    return len;
}

int radio_bridge_sleep(void) { return radio.sleep(); }
int radio_bridge_standby(void) { return radio.standby(); }

// GetStatus (0xC0) and GetRssiInst (0x15) are passive opcodes — safe to call
// while RX is armed without disturbing ChipMode.
void radio_bridge_diag_poll(void) {
    g_diag_polls++;

    SubGhz.SPI.beginTransaction(SubGhz.spi_settings);
    SubGhz.setNssActive(true);
    SubGhz.SPI.transfer(0xC0);
    g_chip_status = SubGhz.SPI.transfer(0x00);
    SubGhz.setNssActive(false);
    SubGhz.SPI.endTransaction();

    SubGhz.SPI.beginTransaction(SubGhz.spi_settings);
    SubGhz.setNssActive(true);
    SubGhz.SPI.transfer(0x15);
    SubGhz.SPI.transfer(0x00);
    uint8_t rssi_raw = SubGhz.SPI.transfer(0x00);
    SubGhz.setNssActive(false);
    SubGhz.SPI.endTransaction();
    g_chip_rssi_raw = rssi_raw;
    if (rssi_raw < g_chip_rssi_max) g_chip_rssi_max = rssi_raw;

    uint16_t irq = radio.getIrqFlags();
    g_chip_irq = irq;
    if (irq & 0x0004) g_preamble_seen++;
}
