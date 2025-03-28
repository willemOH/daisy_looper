#include "daisy_seed.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisySeed hardware;

#define BUFFER_LENGTH (48000 * 349) // 349.52 secs; 48k * 2 (stereo) * 2  (16-bit or 2 bytes per sample) = 192k/s
float DSY_SDRAM_BSS Buffer[BUFFER_LENGTH];

float bufferIndex = 0;
uint32_t end = 48000;

float sysSampleRate;

bool record = false;
bool play = false;

struct StereoPair{
    float left;
    float right;
};

float GetBufferValueInterpolated(float index){ 
    int32_t indexInt = static_cast<int32_t>(index); //strips decimal
    float indexFraction = index - indexInt; //gets decimal

    float currentChannel = Buffer[indexInt];
    float nextChannel = Buffer[indexInt + 2]; //if currentChannel is left channel, nextChannel will be left channel. Same with right
    
    // Linear interpolation (creating value halfway between samples)
    float sig = currentChannel + (nextChannel - currentChannel) * indexFraction;

    return {sig}; 
}

void SetBufferValue(uint32_t index, StereoPair signal){
    Buffer[index] = signal.left;
    Buffer[index + 1] = signal.right;
}

void FillBuffer(float sampleRate){ //fills buffer with test tone
    Oscillator osc;
    osc.Init(sampleRate);
    osc.SetWaveform(osc.WAVE_TRI);
    osc.SetFreq(440.0f);
    osc.SetAmp(0.1f);

    for (uint32_t i = 0; i < 98000.0f; i++)
    {
        float signal = osc.Process();
        SetBufferValue(i, StereoPair{signal, signal});
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
    AudioHandle::InterleavingOutputBuffer out,
    size_t                                size)
{
    StereoPair sampled = StereoPair{0, 0};
    for (size_t i = 0; i < size; i += 2)
        {
            if (record){
                SetBufferValue(bufferIndex * 2.0f, StereoPair{in[i], in[i+1]}); //* 2 to access the buffer as if it contained single left and right channel structs
                bufferIndex += 1.0f; 
                end = bufferIndex;
            }
            else if(play){
                if (bufferIndex < end)
                    {
                        sampled.right = GetBufferValueInterpolated(bufferIndex * 2.0f); 
                        sampled.left = GetBufferValueInterpolated(bufferIndex * 2.0f + 1.0f);
                        bufferIndex += 1.0f;
                    }     
                else{
                    bufferIndex = 0.0f;
                    }
                }      
        out[i] = sampled.left + in[i];
        out[i + 1] = sampled.right + in[i + 1]; 
        }          
}

int main(void)
{
    hardware.Configure();
    hardware.Init();

    size_t blocksize = 4;

    sysSampleRate = hardware.AudioSampleRate();

    FillBuffer(sysSampleRate);
    
    Switch recButton;
    Switch playButton;
    recButton.Init(hardware.GetPin(25), 1000);
    playButton.Init(hardware.GetPin(28), 1000);

	// start callback
	hardware.SetAudioBlockSize(blocksize);
    hardware.StartAudio(AudioCallback);

    //update loop
    for(;;)
    { 
        recButton.Debounce();
        playButton.Debounce();
        hardware.SetLed(recButton.Pressed()); //onboard led indicates recording state
        if (recButton.RisingEdge()){ //resets buffer index when recording begins
            bufferIndex = 0;
        }
        record = recButton.Pressed();
        if (playButton.RisingEdge()){ //toggles playback
            play = !play; 
        } 
        System::Delay(1);
    }
}
