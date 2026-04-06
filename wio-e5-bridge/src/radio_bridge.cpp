#include "radio_bridge.h"
#include "uart_protocol.h"
#include <RadioLib.h>

STM32WLx radio = new STM32WLx_Module();

static volatile bool tx_done_flag = false;
static volatile bool rx_done_flag = false;

static void tx_done_isr(void) { tx_done_flag = true; }
static void rx_done_isr(void) { rx_done_flag = true; }

int radio_bridge_init(void) {
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 10);
    if (state != RADIOLIB_ERR_NONE) return state;
    radio.setDio1Action(rx_done_isr);
    return RADIOLIB_ERR_NONE;
}

int radio_bridge_set_dio(const uint8_t *payload, uint16_t len) {
    if (len < DIO_PAYLOAD_SIZE) return -1;
    uint8_t rf_switch = payload[DIO_RF_SWITCH_OFFSET];
    uint8_t tcxo_raw = payload[DIO_TCXO_OFFSET];
    float tcxo_voltage = (float)tcxo_raw / 10.0f;
    int state = RADIOLIB_ERR_NONE;
    if (tcxo_voltage > 0) {
        state = radio.setTCXO(tcxo_voltage);
        if (state != RADIOLIB_ERR_NONE) return state;
    }
    if (rf_switch) radio.setDio2AsRfSwitch(true);
    return state;
}

int radio_bridge_configure(const uint8_t *payload, uint16_t len) {
    if (len < CFG_PAYLOAD_SIZE) return -1;
    uint32_t freq_hz;
    memcpy(&freq_hz, &payload[CFG_FREQ_OFFSET], 4);
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
    uint16_t preamble;
    memcpy(&preamble, &payload[CFG_PREAMBLE_OFFSET], 2);
    uint16_t syncword;
    memcpy(&syncword, &payload[CFG_SYNCWORD_OFFSET], 2);

    int state;
    state = radio.setFrequency(freq_mhz); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setBandwidth(bw); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setSpreadingFactor(sf); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setCodingRate(cr); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setOutputPower(power); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setPreambleLength(preamble); if (state != RADIOLIB_ERR_NONE) return state;
    state = radio.setSyncWord(syncword & 0xFF, (syncword >> 8) & 0xFF);
    return state;
}

int radio_bridge_transmit(const uint8_t *data, uint16_t len) {
    tx_done_flag = false;
    return radio.startTransmit((uint8_t *)data, len);
}

int radio_bridge_check_tx_done(void) {
    if (tx_done_flag) {
        tx_done_flag = false;
        radio.finishTransmit();
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
    int len = radio.getPacketLength();
    if (len <= 0 || (uint16_t)len > buf_size) return -1;
    int state = radio.readData(buf, len);
    if (state != RADIOLIB_ERR_NONE) return -1;
    *rssi = radio.getRSSI();
    *snr = radio.getSNR();
    radio.startReceive();
    return len;
}

int radio_bridge_sleep(void) { return radio.sleep(); }
int radio_bridge_standby(void) { return radio.standby(); }
