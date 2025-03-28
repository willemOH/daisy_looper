#include "daisy_seed.h"
#include "daisysp.h"

//ostensibly needed for sd writing
#include <stdio.h>
#include <string.h>
#include "daisy_core.h"
#include "util/wav_format.h"
#include "ff.h"
#include "fatfs.h"

using namespace daisy;
using namespace daisysp;

DaisySeed hardware;

SdmmcHandler sdcard;
FatFSInterface fsi;
WavWriter<16384> writer;
bool saved = true;

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

                float sampleArray[2] = {in[i], in[i+1]};
                writer.Sample(sampleArray); // Call the Sample function with the array
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

    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    if (sdcard.Init(sd_cfg) != SdmmcHandler::Result::OK)
    {
        hardware.PrintLine("SD card initialization failed");
        return 1;
    }
    if (fsi.Init(FatFSInterface::Config::MEDIA_SD) != FatFSInterface::Result::OK)
    {
        hardware.PrintLine("File system initialization failed");
        return 1;
    }
    if (f_mount(&fsi.GetSDFileSystem(), "/", 1) != FR_OK)
    {
        hardware.PrintLine("File system mount failed");
        return 1;
    }

    System::Delay(100);

    WavWriter<16384>::Config config;
    config.samplerate = sysSampleRate; 
    config.channels = 2;          
    config.bitspersample = 16;    
    writer.Init(config);

     // Open WAV file
     writer.OpenFile("loop.wav");

    //update loop
    for(;;)
    { 
        recButton.Debounce();
        playButton.Debounce();
        hardware.SetLed(recButton.Pressed()); //onboard led indicates recording state
        if (recButton.RisingEdge()){ //resets buffer index when recording begins
            bufferIndex = 0;
        }
        if (recButton.FallingEdge()){
            saved = false;
        }
        record = recButton.Pressed();
        if (playButton.RisingEdge()){ //toggles playback
            play = !play; 
        } 
        
        writer.Write();
        if(!saved){
            writer.SaveFile();
            hardware.PrintLine("file saved");
            saved = true;
        }
    }
}
