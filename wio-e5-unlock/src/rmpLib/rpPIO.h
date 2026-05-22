#pragma once

#include "rpDMA.h"

enum class erpPIOWaitSource
{
    gpio,
    pin,
    irq,
};

enum class erpPIOJumpCondition
{
    always,
    not_X,
    XNotZeroPostDec,
    not_Y,
    YNotZeroPostDec,
    XNotEqualY,
    pin,
    NotOSRempty,
};

enum class erpPIOOutDestination
{
    pins,
    x,
    y,
    null,
    pindirs,
    pc,
    isr,
    exec,
};

enum class erpPIOMovDestination
{
    pins,
    x,
    y,
    exec,
    pc,
    isr,
    osr,
};

enum class erpPIOMovOperation
{
    none,
    invert,
    bit_reverse,
};

enum class erpPIOMovSource
{
    pins,
    x,
    y,
    null,
    status,
    isr,
    osr,

};

enum class erpPIOInSource
{
	pins,
	x,
	y,
	null,
	isr,
	osr,
};

enum class erpPIOSetDestination
{
	pins,
	pindirs,
	x,
	y,
};

enum class erpPIOCpuIRQSource
{
    irq0,
    rxfifo0_not_empty,

};

enum class erpPIODMASize
{
	dmaSize8=0,
	dmaSize16=1,
	dmaSize32=2,
};


#define RP_PIO_INSTRUCTION_COUNT 32

// wrapper for PIO
class rpPIO
{
    private:
        int m_iSideSetEnabledCount = 0;
        int m_iPIOIndex=0;
        int m_iPIOStateMachineIndex=0;
        int m_iFirstInstruction=0;
        int m_iCurrentInstruction=0 ;
        int m_iWrap=0;
        int m_iWrapTarget=0;


        int m_iInstructionMemoryCache[RP_PIO_INSTRUCTION_COUNT] = {0}; // NEED THIS BECAUSE MEMORY IS WRITE ONLY
        char m_chJumpLabels[RP_PIO_INSTRUCTION_COUNT] = {0};
        char m_chLabels[RP_PIO_INSTRUCTION_COUNT] = {0};

        int returnAddressForLabel(char chLabel);
        void SetCurrentInstruction(unsigned char btSideSet, unsigned char btDelay, int iInstruction, bool bSideSetEnabled);

	    rpDMA obDMARx;
	    rpDMA obDMATx;

		bool m_bSideSetOptionEnable = false;
		bool m_bGPIOBase16Enabled = false;


    public:

	    int  getLastInstruction();

        // instruction encoding
        void encode_begin();
        void encode_end();
        void encode_addLabel(char chLabel);
        void encode_wrapTarget();
        void encode_wrap();
        void encode_out(unsigned char btSideSet, unsigned char btDelay, erpPIOOutDestination eOutDestination, unsigned char bBitCount, bool bSetSetEnabled = false);
        void encode_jmp(unsigned char btSideSet, unsigned char btDelay, erpPIOJumpCondition eJumpCondition, char chLabel, bool bSetSetEnabled = false);
        
	    void encode_set(unsigned char btSideSet,unsigned char btDelay, erpPIOSetDestination eSetDestination, int iValue, bool bSetSetEnabled = false);
		
//	void encode_set_pins_direction(unsigned char btSideSet, unsigned char btDelay, unsigned char bDirection);
  //      void encode_set_pins(unsigned char btSideSet, unsigned char btDelay, unsigned char bData);
        void encode_wait(unsigned char btSideSet, unsigned char btDelay, bool bWaitPolarityIsOne, erpPIOWaitSource eWaitSource, int iIndex, bool bSetSetEnabled = false);
        void encode_nop(unsigned char btSideSet, unsigned char btDelay, bool bSetSetEnabled = false);
        void encode_mov(unsigned char btSideSet, unsigned char btDelay,erpPIOMovDestination eDestination,
                            erpPIOMovOperation eOperation, erpPIOMovSource eSource, bool bSetSetEnabled = false);
	    void encode_in(unsigned char btSideSet, unsigned char btDelay, erpPIOInSource eSource, int iBitCount, bool bSetSetEnabled = false);

	    void encode_irq(unsigned char btSideSet, unsigned char btDelay,bool bClearInt, 
		            bool bWaitInt, int iIntIndex, bool bSetSetEnabled = false);
	
	    void encode_push(unsigned char btSideSet,unsigned char btDelay,bool bIfFull,bool bBlock, bool bSetSetEnabled = false);
	    void encode_pull(unsigned char btSideSet, unsigned char btDelay, bool bIfEmpty, bool bBlock, bool bSetSetEnabled = false);
		
	    void setupFIFOs(bool bUseIn,bool bAutoPush,int iPushThreshold,bool bInShiftRight,
		                bool bUseOut,bool bAutoPull,int iPullThreshold,bool bOutShiftRight);
	    void setupPins(int iSideSetCount, int iSetCount, int iOutCount,
		                int iInBase, int iSideSetBase,int iSetBase,int iOutBase, int iJumpPin, bool bSideSetOptionalEnable);
	    // Configure SM EXECCTRL.STATUS_SEL (bit 4) + STATUS_N (bits 3:0).
	    // sel=1 → status reflects RX FIFO level; n=watermark threshold.
	    void setupStatus(int iSel, int iN);
		
	    void init(int iPIOModuleIndex, int iStateMachineIndex, int iFirstInstruction);

		void setStartInstruction(int iStartInstruction);
	    void start();
	    void stop();

	    void reinitForStartMulti();
	    void startMulti(bool bSM0, bool bSM1, bool bSM2, bool bSM3);

	    void clearFIFOs();
	    void writeTxFIFO(unsigned int iValue);
		int getTxFIFOCount();
	    int getTxFifoDepth();
	    unsigned int  readRxFIFO();
	    int getRxFIFOCount();


	    void enableRxDMA(int iChannel, void * pDestinationAddress, int iMaxCount, erpPIODMASize iDMASize);
	    void abortRxDMA();
	    int getRxDMATransferCount();
	    bool isTxDMAComplete();
	    void enableTxDMA(int iChannel, void * pDestinationAddress, int iMaxCount,erpPIODMASize iDMASize);
	    void abortTxDMA();


	    bool checkAndClearInt(int iIntIndex);
	    void SetupCallBackOnIRQ(void * pCallBack, void * pObject, erpPIOCpuIRQSource eSource);
	    void SetupCallBackOnDMATxIRQ(void* pCallback, void* Object);   
	    void SetupCallBackOnDMARxIRQ(void* pCallback, void* Object);           
        void disableRxDMAIRQ();     

	    unsigned int readYRegisterViaFIFO();

	    unsigned int setupClockPeriod(float fCountInNs);
	    unsigned int setupClockPeriodByDiv(float  fPeriodInNs);

		bool isTXFifoEmpty(void);

		void enableGPIOBase16(bool bEnable);

        bool isRxDMAComplete();

};
