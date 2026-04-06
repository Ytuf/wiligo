#include <Arduino.h>
#include "uart_protocol.h"
#include "radio_bridge.h"

// Forward declarations for parser functions
extern "C" {
    int uart_proto_parse_byte(uint8_t byte, uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len);
    void uart_proto_reset(void);
}

HardwareSerial BridgeSerial(PB7, PB6);

static uint8_t tx_buf[UART_RADIO_HEADER_SIZE + UART_RADIO_MAX_PAYLOAD + UART_RADIO_CRC_SIZE];

static void send_response(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    uint16_t frame_len = uart_proto_build_frame(tx_buf, cmd, payload, len);
    BridgeSerial.write(tx_buf, frame_len);
}

static void send_ack(uint8_t original_cmd, uint8_t status) {
    uint8_t payload[2] = { original_cmd, status };
    send_response(RSP_ACK, payload, 2);
}

static void handle_command(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    int result;
    switch (cmd) {
    case CMD_RADIO_SET_DIO:
        result = radio_bridge_set_dio(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_CONFIGURE:
        result = radio_bridge_configure(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_TX:
        result = radio_bridge_transmit(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_RX_START:
        result = radio_bridge_start_receive();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_SLEEP:
        result = radio_bridge_sleep();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_STANDBY:
        result = radio_bridge_standby();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_GET_STATUS:
        send_ack(cmd, STATUS_OK);
        break;
    default:
        send_response(RSP_ERROR, (const uint8_t[]){STATUS_ERR_UNKNOWN}, 1);
        break;
    }
}

void setup() {
    BridgeSerial.begin(115200);
    uart_proto_reset();

    int result = radio_bridge_init();
    if (result != 0) {
        pinMode(LED_BUILTIN, OUTPUT);
        while (1) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }
}

void loop() {
    while (BridgeSerial.available()) {
        uint8_t byte = BridgeSerial.read();
        uint8_t cmd;
        uint8_t payload[UART_RADIO_MAX_PAYLOAD];
        uint16_t payload_len;
        if (uart_proto_parse_byte(byte, &cmd, payload, &payload_len)) {
            handle_command(cmd, payload, payload_len);
        }
    }

    if (radio_bridge_check_tx_done()) {
        send_response(RSP_TX_DONE, NULL, 0);
    }

    uint8_t rx_buf[UART_RADIO_MAX_PAYLOAD];
    float rssi, snr;
    int rx_len = radio_bridge_check_rx(rx_buf, sizeof(rx_buf), &rssi, &snr);
    if (rx_len > 0) {
        uint8_t rsp_payload[3 + UART_RADIO_MAX_PAYLOAD];
        int16_t rssi_int = (int16_t)(rssi * 10);
        int8_t snr_int = (int8_t)(snr * 4);
        memcpy(&rsp_payload[0], &rssi_int, 2);
        rsp_payload[2] = (uint8_t)snr_int;
        memcpy(&rsp_payload[3], rx_buf, rx_len);
        send_response(RSP_RX_PACKET, rsp_payload, 3 + rx_len);
    }
}
