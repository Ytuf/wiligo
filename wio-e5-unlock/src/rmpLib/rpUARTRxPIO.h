//
// Created by daver on 1/27/2025.
//

#ifndef RPUARTRXPIO_H
#define RPUARTRXPIO_H

#include "rpPIO.h"


class rpUARTRxPIO {
    private:
        int m_iRtsPin = -1;
    public:

        rpPIO obPIO;

        void init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate);
        void init(int iPin, int iPIOModule, int iStateMachineIndex, int iStartInstruction, int iBaudRate, int iRtsPin);

        int readByteWithBreak(unsigned char & btByte, int & iIsBreak);
        int readBytes(unsigned char* btBuffer, int iLength);
        int bytesAvailable();

        int getRtsPin() const { return m_iRtsPin; }
};



#endif //RPUARTRXPIO_H
