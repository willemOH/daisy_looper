#include "daisy_seed.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisySeed hardware;

int main(void)
{
    bool led_state;
    led_state = true;

    hardware.Configure();
    hardware.Init();

    for(;;)
    {
        hardware.SetLed(led_state);

        led_state = !led_state;

        System::Delay(500);
    }
}
