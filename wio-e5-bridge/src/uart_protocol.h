#pragma once

#include <stdint.h>
#include <string.h>

// Frame sync bytes
#define UART_RADIO_SYNC1 0xAA
#define UART_RADIO_SYNC2 0x55

// Commands (Host -> Bridge)
#define CMD_RADIO_CONFIGURE   0x01
#define CMD_RADIO_TX          0x02
#define CMD_RADIO_RX_START    0x03
#define CMD_RADIO_SLEEP       0x04
#define CMD_RADIO_STANDBY     0x05
#define CMD_RADIO_GET_STATUS  0x06
#define CMD_RADIO_SET_DIO     0x07

// Responses (Bridge -> Host)
#define RSP_ACK               0x81
#define RSP_RX_PACKET         0x82
#define RSP_TX_DONE           0x83
#define RSP_STATUS            0x84
#define RSP_ERROR             0x8F

// Status codes
#define STATUS_OK             0x00
#define STATUS_ERR_UNKNOWN    0x01
#define STATUS_ERR_TIMEOUT    0x02
#define STATUS_ERR_CRC        0x03
#define STATUS_ERR_BUSY       0x04

// Radio states (in STATUS response)
#define RADIO_STATE_IDLE      0x00
#define RADIO_STATE_RX        0x01
#define RADIO_STATE_TX        0x02
#define RADIO_STATE_SLEEP     0x03

// Frame limits
#define UART_RADIO_MAX_PAYLOAD 256
#define UART_RADIO_HEADER_SIZE 5   // SYNC1 + SYNC2 + CMD + LEN_HI + LEN_LO
#define UART_RADIO_CRC_SIZE    1

// CONFIGURE payload offsets (12 bytes total)
#define CFG_FREQ_OFFSET    0   // uint32_t, Hz
#define CFG_BW_OFFSET      4   // uint8_t, encoded (0=125k, 1=250k, 2=500k)
#define CFG_SF_OFFSET      5   // uint8_t, 6-12
#define CFG_CR_OFFSET      6   // uint8_t, 5-8
#define CFG_POWER_OFFSET   7   // int8_t, dBm
#define CFG_PREAMBLE_OFFSET 8  // uint16_t
#define CFG_SYNCWORD_OFFSET 10 // uint16_t
#define CFG_PAYLOAD_SIZE   12

// SET_DIO payload (2 bytes)
#define DIO_RF_SWITCH_OFFSET  0  // uint8_t, 1=DIO2 as RF switch
#define DIO_TCXO_OFFSET       1  // uint8_t, TCXO voltage * 10 (e.g., 18 = 1.8V)
#define DIO_PAYLOAD_SIZE      2

// Parser state — one instance per UART so a half-frame on one serial
// is not clobbered by a complete frame on the other.
typedef enum {
    RX_WAIT_SYNC1,
    RX_WAIT_SYNC2,
    RX_WAIT_CMD,
    RX_WAIT_LEN_HI,
    RX_WAIT_LEN_LO,
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC
} rx_state_t;

typedef struct {
    rx_state_t state;
    uint8_t cmd;
    uint16_t payload_len;
    uint16_t payload_idx;
    uint8_t payload[UART_RADIO_MAX_PAYLOAD];
} uart_proto_ctx_t;

// CRC8 (CCITT polynomial 0x07)
static inline uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// Build a protocol frame into buf. Returns total frame length.
static inline uint16_t uart_proto_build_frame(uint8_t *buf, uint8_t cmd,
                                               const uint8_t *payload, uint16_t payload_len) {
    buf[0] = UART_RADIO_SYNC1;
    buf[1] = UART_RADIO_SYNC2;
    buf[2] = cmd;
    buf[3] = (payload_len >> 8) & 0xFF;
    buf[4] = payload_len & 0xFF;
    if (payload_len > 0 && payload != NULL)
        memcpy(&buf[5], payload, payload_len);

    uint8_t crc_data[3 + UART_RADIO_MAX_PAYLOAD];
    crc_data[0] = cmd;
    crc_data[1] = buf[3];
    crc_data[2] = buf[4];
    if (payload_len > 0 && payload != NULL)
        memcpy(&crc_data[3], payload, payload_len);

    buf[5 + payload_len] = crc8(crc_data, 3 + payload_len);
    return UART_RADIO_HEADER_SIZE + payload_len + UART_RADIO_CRC_SIZE;
}
