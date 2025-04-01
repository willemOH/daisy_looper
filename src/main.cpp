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

//#define LOGG // start serial over USB Logger class

DaisySeed hardware;

SdmmcHandler sdcard;
FatFSInterface fsi;
WavWriter<16384> writer;
bool saved = true;

FIL fp;
bool stereo = true;

#define BUFFER_LENGTH (48000 * 349) // 349.52 secs; 48k * 2 (stereo) * 2  (16-bit or 2 bytes per sample) = 192k/s
int16_t DSY_SDRAM_BSS Buffer[BUFFER_LENGTH];

float bufferIndex = 0;
//uint32_t length = 48000;
size_t length = 48000;

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

    float currentChannel = s162f(Buffer[indexInt]);
    float nextChannel = s162f(Buffer[indexInt + 2]); //if currentChannel is left channel, nextChannel will be left channel. Same with right
    
    // Linear interpolation (creating value halfway between samples)
    float sig = currentChannel + (nextChannel - currentChannel) * indexFraction;

    return {sig}; 
}

void SetBufferValue(uint32_t index, StereoPair signal){
    Buffer[index] = f2s16(signal.left);
    Buffer[index + 1] = f2s16(signal.right);
}

/* adds given file to the buffer. Only supports 16bit PCM 48kHz. 
	 * If Stereo samples are interleaved left then right.
	 * return 0: succesful, 1: file read failed, 2: invalid format, 3: file too large
	*/ 
int SetSample(TCHAR *fname) {
    UINT bytesread;
    WAV_FormatTypeDef wav_data; 
    
    memset(Buffer, 0, BUFFER_LENGTH);
    
    if(f_open(&fp, fname, (FA_OPEN_EXISTING | FA_READ)) == FR_OK) {
        // Populate the WAV Info
        if(f_read(&fp, (void *)&wav_data, sizeof(WAV_FormatTypeDef), &bytesread) != FR_OK) return 1;	
    } else return 4;

    if (wav_data.SampleRate != 48000 || wav_data.BitPerSample != 16) return 2;
    if (wav_data.SubCHunk2Size > BUFFER_LENGTH || wav_data.NbrChannels > 2) return 3;
    stereo = wav_data.NbrChannels == 2;

    if (f_lseek(&fp, sizeof(WAV_FormatTypeDef)) != FR_OK) return 5;
    
    if(f_read(&fp, Buffer, wav_data.SubCHunk2Size, &bytesread) != FR_OK)return 6;
    length = bytesread / (wav_data.BitPerSample/8) / (stereo ? 2 : 1);

    f_close(&fp);
    return 0;
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
    //StereoPair sampled = StereoPair{0, 0};
    float left = 0;
    float right = 0;
    for (size_t i = 0; i < size; i += 2)
        {
            if (record){
                SetBufferValue(bufferIndex * 2.0f, StereoPair{in[i], in[i+1]}); //* 2 to access the buffer as if it contained single left and right channel structs
                bufferIndex += 1.0f; 
                length = bufferIndex;

                float sampleArray[2] = {in[i], in[i+1]};
                writer.Sample(sampleArray); // Call the Sample function with the array
            }
            else if(play){
                if (bufferIndex < length)
                    {
                        right = GetBufferValueInterpolated(bufferIndex * 2.0f); 
                        left = GetBufferValueInterpolated(bufferIndex * 2.0f + 1.0f);
                        bufferIndex += 1.0f;
                    }     
                else{
                    bufferIndex = 0.0f;
                    }
                }      
        out[i] = left + in[i];
        out[i + 1] = right + in[i + 1]; 
        }          
}

int main(void)
{
    hardware.Configure();
    hardware.Init();

    // logging over serial USB
	#ifdef LOGG
	hardware.StartLog(true); 
	hardware.PrintLine("monitoring started");
	#endif
    
    size_t blocksize = 4;

    sysSampleRate = hardware.AudioSampleRate();

    //FillBuffer(sysSampleRate);
    
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

    char sampleName[] = "loop.wav"; //read to buffer
    int error = SetSample(sampleName);
    hardware.PrintLine("setsample error: %d", error);

    WavWriter<16384>::Config config;
    config.samplerate = sysSampleRate; 
    config.channels = 2;          
    config.bitspersample = 16;    
    writer.Init(config);

     

    //update loop
    for(;;)
    { 
        recButton.Debounce();
        playButton.Debounce();
        hardware.SetLed(recButton.Pressed()); //onboard led indicates recording state
        if (recButton.Pressed()){
            writer.Write();
        }
        if (recButton.RisingEdge()){ //resets buffer index when recording begins
            // Open WAV file
            writer.OpenFile("loop.wav");
            bufferIndex = 0;
        }
        if (recButton.FallingEdge()){
            saved = false;
        }
        record = recButton.Pressed();
        if (playButton.RisingEdge()){ //toggles playback
            play = !play; 
        } 
        
        if(!saved){
            writer.SaveFile();
            hardware.PrintLine("file saved");
            saved = true;
        }
    }
}
