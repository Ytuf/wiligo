#pragma once

enum class rpGPIOPullResistor
{
    none,
    up,
    down,
};

class rpGPIO
{
    private:

        int iPinNumber;

    public:

        rpGPIO();
        rpGPIO(int iPin);

	    int pin()
	    {
		    return iPinNumber;
	    }

	    void init(bool bMakeOutput, bool bInitalValue, rpGPIOPullResistor rpPull);

        bool get();
        void set(bool bValue);
        void setDirection(bool bMakeOutput);
        void toggle();

        void EnableEvents();

        void setPin(int iPin);

};
