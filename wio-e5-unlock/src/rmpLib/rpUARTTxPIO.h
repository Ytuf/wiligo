//
// Created by daver on 1/27/2025.
//

#ifndef RPUARTTXPIO_H
#define RPUARTTXPIO_H

#include "rpPIO.h"

class rpUARTTxPIO {
private:
    int m_iPIOModule = 0;
    int m_iPin = 25;
    int m_iCtsPin = -1;
    int m_iTxFifoDepth = 4;   // populated from hardware in init()
public:

    rpPIO obPIO;

    void init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate);
    void init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate, int iCtsPin);
    void setupPinFunction();

    int writeBytes(const unsigned char* btBuffer, int iLength);
    int bytesRoomAvailable();

    int getCtsPin() const { return m_iCtsPin; }
};



#endif //RPUARTTXPIO_H
