
#include "rpGPIO.h"

#include "hardware/gpio.h"

//static char event_str[128];
struct GPIO_manager {
    int iGPIO;
    rpGPIO * pGPIO;
};

struct GPIO_manager g_gpios[32] = {0};

void gpio_callback(unsigned int gpio, uint32_t events) {
    // Put the GPIO event(s) that just happened into event_str
    // so we can print it
    // gpio_event_string(event_str, events);
    // printf("GPIO %d %s\n", gpio, event_str);
}

rpGPIO::rpGPIO()
{
    iPinNumber = 25;
}

rpGPIO::rpGPIO(int iPin)
{
    iPinNumber = iPin;
}

void rpGPIO::setDirection(bool bMakeOutput)
{
    if (bMakeOutput)
        gpio_set_dir(iPinNumber,GPIO_OUT);
    else
        gpio_set_dir(iPinNumber,GPIO_IN);

}

void rpGPIO::init(bool bMakeOutput, bool bInitalValue, rpGPIOPullResistor rpPull)
{
    gpio_init(iPinNumber);

	switch (rpPull)
	{
	    case rpGPIOPullResistor::down:
		    gpio_pull_down(iPinNumber);
		    break;
	    case rpGPIOPullResistor::up:
		    gpio_pull_up(iPinNumber);
		    break;
	    case rpGPIOPullResistor::none:
		    gpio_disable_pulls(iPinNumber);
		    break;
	}

	set(bInitalValue);

    if (bMakeOutput)
        gpio_set_dir(iPinNumber,GPIO_OUT);
    else
        gpio_set_dir(iPinNumber,GPIO_IN);



}

void rpGPIO::set(bool bValue)
{
    if (bValue)
        gpio_put(iPinNumber, 1);
     else
        gpio_put(iPinNumber, 0);

}
void rpGPIO::setPin(int iPin)
{
    iPinNumber = iPin;
}

bool rpGPIO::get()
{
    return gpio_get(iPinNumber);
}

void rpGPIO::EnableEvents()
{
    gpio_set_irq_enabled_with_callback(2, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
}

void rpGPIO::toggle()
{
     gpio_put(iPinNumber, !gpio_get(iPinNumber));
}
