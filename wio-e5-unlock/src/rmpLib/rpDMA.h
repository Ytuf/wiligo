#pragma once

#include <functional>

enum class rpDMAPeripheralRequest
{
	pio0Tx0,
	pio0Tx1,
	pio0Tx2,
	pio0Tx3,
	pio0Rx0,
	pio0Rx1,
	pio0Rx2,
	pio0Rx3,

	pio1Tx0,
	pio1Tx1,
	pio1Tx2,
	pio1Tx3,
	pio1Rx0,
	pio1Rx1,
	pio1Rx2,
	pio1Rx3,

	pio2Tx0, // pio2 is rp2350 only
	pio2Tx1,
	pio2Tx2,
	pio2Tx3,
	pio2Rx0,
	pio2Rx1,
	pio2Rx2,
	pio2Rx3,

	spi0Tx,
	spi0Rx,

	spi1Tx,
	spi1Rx,

	uart0Tx,
	uart0Rx,

	uart1Tx,
	uart1Rx,

	pwm_wrap0,
	pwm_wrap1,
	pwm_wrap2,
	pwm_wrap3,
	pwm_wrap4,
	pwm_wrap5,
	pwm_wrap6,
	pwm_wrap7,

	i2c0Tx,
	i2c0Rx,

	i2c1Tx,
	i2c1Rx,

	adc,

	xipstream,
	xipssitx,
	xipssirx,
};

enum class rpDMATransferDataSize
{
	byte,
	halfword,
	word,
};


class rpDMA
{
private:
	int m_iDMAChannel = 0;
	int m_iCoreID = 0;
	bool m_bClaimed = false;

public:

	~rpDMA();

	void initClaimFreeDMAChannel();
	void init(int iChannel);

	int getChannel() { return m_iDMAChannel; };

	void setupPeripheralRequest(rpDMAPeripheralRequest ePeripheral);
	void setupTransfer(void * pDestinationAddress, void * pSourceAddress,
		bool bAutoIncrementDestination, bool bAutoIncrementSource, rpDMATransferDataSize eTransferSize, int iCount);
	void start();
	bool isComplete();


	void abort();
	int getTransferCount();

	unsigned int getCRCValue();
	void enableCRCCalc();

	void SetupCallBack(void* pCallback, void* Object);
	void RemoveCallback();
};
