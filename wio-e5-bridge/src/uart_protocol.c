#include "uart_protocol.h"

// RX state machine
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
} rx_context_t;

static rx_context_t rx_ctx;

// Returns 1 when a complete frame is parsed, 0 otherwise.
int uart_proto_parse_byte(uint8_t byte, uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len) {
    switch (rx_ctx.state) {
    case RX_WAIT_SYNC1:
        if (byte == UART_RADIO_SYNC1)
            rx_ctx.state = RX_WAIT_SYNC2;
        break;

    case RX_WAIT_SYNC2:
        if (byte == UART_RADIO_SYNC2)
            rx_ctx.state = RX_WAIT_CMD;
        else
            rx_ctx.state = RX_WAIT_SYNC1;
        break;

    case RX_WAIT_CMD:
        rx_ctx.cmd = byte;
        rx_ctx.state = RX_WAIT_LEN_HI;
        break;

    case RX_WAIT_LEN_HI:
        rx_ctx.payload_len = (uint16_t)byte << 8;
        rx_ctx.state = RX_WAIT_LEN_LO;
        break;

    case RX_WAIT_LEN_LO:
        rx_ctx.payload_len |= byte;
        rx_ctx.payload_idx = 0;
        if (rx_ctx.payload_len > UART_RADIO_MAX_PAYLOAD) {
            rx_ctx.state = RX_WAIT_SYNC1;
        } else if (rx_ctx.payload_len == 0) {
            rx_ctx.state = RX_WAIT_CRC;
        } else {
            rx_ctx.state = RX_WAIT_PAYLOAD;
        }
        break;

    case RX_WAIT_PAYLOAD:
        rx_ctx.payload[rx_ctx.payload_idx++] = byte;
        if (rx_ctx.payload_idx >= rx_ctx.payload_len)
            rx_ctx.state = RX_WAIT_CRC;
        break;

    case RX_WAIT_CRC: {
        uint8_t crc_buf[3 + UART_RADIO_MAX_PAYLOAD];
        crc_buf[0] = rx_ctx.cmd;
        crc_buf[1] = (rx_ctx.payload_len >> 8) & 0xFF;
        crc_buf[2] = rx_ctx.payload_len & 0xFF;
        if (rx_ctx.payload_len > 0)
            memcpy(&crc_buf[3], rx_ctx.payload, rx_ctx.payload_len);

        uint8_t expected_crc = crc8(crc_buf, 3 + rx_ctx.payload_len);
        rx_ctx.state = RX_WAIT_SYNC1;

        if (byte == expected_crc) {
            *out_cmd = rx_ctx.cmd;
            *out_len = rx_ctx.payload_len;
            if (rx_ctx.payload_len > 0)
                memcpy(out_payload, rx_ctx.payload, rx_ctx.payload_len);
            return 1;
        }
        break;
    }
    }
    return 0;
}

void uart_proto_reset(void) {
    rx_ctx.state = RX_WAIT_SYNC1;
}
