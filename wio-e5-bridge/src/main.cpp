#include <Arduino.h>

// WIO-E5 UART1 pins (to RP2350)
#define BRIDGE_UART_TX PB6
#define BRIDGE_UART_RX PB7

HardwareSerial BridgeSerial(BRIDGE_UART_RX, BRIDGE_UART_TX);

void setup() {
    BridgeSerial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    // Blink LED to confirm firmware is running
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
}
