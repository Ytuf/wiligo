#include "uart_protocol.h"

// Per-context byte parser. Returns 1 when a complete frame is parsed, 0 otherwise.
int uart_proto_parse_byte_ctx(uart_proto_ctx_t *ctx, uint8_t byte,
                              uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len) {
    switch (ctx->state) {
    case RX_WAIT_SYNC1:
        if (byte == UART_RADIO_SYNC1)
            ctx->state = RX_WAIT_SYNC2;
        break;

    case RX_WAIT_SYNC2:
        if (byte == UART_RADIO_SYNC2)
            ctx->state = RX_WAIT_CMD;
        else
            ctx->state = RX_WAIT_SYNC1;
        break;

    case RX_WAIT_CMD:
        ctx->cmd = byte;
        ctx->state = RX_WAIT_LEN_HI;
        break;

    case RX_WAIT_LEN_HI:
        ctx->payload_len = (uint16_t)byte << 8;
        ctx->state = RX_WAIT_LEN_LO;
        break;

    case RX_WAIT_LEN_LO:
        ctx->payload_len |= byte;
        ctx->payload_idx = 0;
        if (ctx->payload_len > UART_RADIO_MAX_PAYLOAD) {
            ctx->state = RX_WAIT_SYNC1;
        } else if (ctx->payload_len == 0) {
            ctx->state = RX_WAIT_CRC;
        } else {
            ctx->state = RX_WAIT_PAYLOAD;
        }
        break;

    case RX_WAIT_PAYLOAD:
        ctx->payload[ctx->payload_idx++] = byte;
        if (ctx->payload_idx >= ctx->payload_len)
            ctx->state = RX_WAIT_CRC;
        break;

    case RX_WAIT_CRC: {
        uint8_t crc_buf[3 + UART_RADIO_MAX_PAYLOAD];
        crc_buf[0] = ctx->cmd;
        crc_buf[1] = (ctx->payload_len >> 8) & 0xFF;
        crc_buf[2] = ctx->payload_len & 0xFF;
        if (ctx->payload_len > 0)
            memcpy(&crc_buf[3], ctx->payload, ctx->payload_len);

        uint8_t expected_crc = crc8(crc_buf, 3 + ctx->payload_len);
        ctx->state = RX_WAIT_SYNC1;

        if (byte == expected_crc) {
            *out_cmd = ctx->cmd;
            *out_len = ctx->payload_len;
            if (ctx->payload_len > 0)
                memcpy(out_payload, ctx->payload, ctx->payload_len);
            return 1;
        }
        break;
    }
    }
    return 0;
}

void uart_proto_reset_ctx(uart_proto_ctx_t *ctx) {
    ctx->state = RX_WAIT_SYNC1;
}

// Legacy single-context API — kept so prior callers keep working.
static uart_proto_ctx_t g_default_ctx;

int uart_proto_parse_byte(uint8_t byte, uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len) {
    return uart_proto_parse_byte_ctx(&g_default_ctx, byte, out_cmd, out_payload, out_len);
}

void uart_proto_reset(void) {
    uart_proto_reset_ctx(&g_default_ctx);
}
