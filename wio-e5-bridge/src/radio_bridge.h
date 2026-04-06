#pragma once

#include <stdint.h>

int radio_bridge_init(void);
int radio_bridge_configure(const uint8_t *payload, uint16_t len);
int radio_bridge_set_dio(const uint8_t *payload, uint16_t len);
int radio_bridge_transmit(const uint8_t *data, uint16_t len);
int radio_bridge_start_receive(void);
int radio_bridge_sleep(void);
int radio_bridge_standby(void);
int radio_bridge_check_rx(uint8_t *buf, uint16_t buf_size, float *rssi, float *snr);
int radio_bridge_check_tx_done(void);
