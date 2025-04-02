#include "daisy_seed.h"
#include "daisysp.h"

//needed for sd writing
#include "daisy_core.h"
#include "util/wav_format.h"
#include "ff.h"
#include "fatfs.h"

using namespace daisy;
using namespace daisysp;

/**
 * This a basic stereo looper that saves the recorded to loop to sd card as wav file 
 * and then reloads to memory at program start. There are buttons for record and play.
 * This sampler/looper is optimized for sd card usage so the buffer is in 16-bit @ 48K
 * not floating point. This is also why there is converting between float and uint16_t types.
 * For more info on all of the wav writing procedures see the WavWriter class.
 */
//#define LOGG // uncomment to start serial over USB Logger class

DaisySeed hardware;

SdmmcHandler sdcard;
FatFSInterface fsi;
WavWriter<16384> writer; //16-bit
bool saved = true;

FIL fp;
bool stereo = true;

#define BUFFER_LENGTH (48000 * 349) // 349.52 secs; 48k * 2 (stereo) * 2  (16-bit or 2 bytes per sample) = 192k/s
int16_t DSY_SDRAM_BSS Buffer[BUFFER_LENGTH]; 

float bufferIndex = 0;
size_t length = 0;
uint32_t recordedLength;

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

/* adds given wav file to the buffer. Only supports 16bit PCM 48kHz. 
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

    //shows if there is a discrepancy between the following values after sample load (if not the same, wavwriter.h needs the bugfix)
    hardware.PrintLine("SubCHunk2Size: %d", wav_data.SubCHunk2Size);
    hardware.PrintLine("Bytes read: %d", bytesread);

    f_close(&fp);
    return 0;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
    AudioHandle::InterleavingOutputBuffer out,
    size_t                                size)
{
    float left = 0;
    float right = 0;
    for (size_t i = 0; i < size; i += 2){
        if (record){
            SetBufferValue(bufferIndex * 2.0f, StereoPair{in[i], in[i+1]}); //* 2 to access the buffer as if it contained single left and right channel structs
            bufferIndex += 1.0f; 
            length = bufferIndex;
            //wav writing
            float sampleArray[2] = {in[i], in[i+1]};
            writer.Sample(sampleArray); 
        }
        else if(play){
            if (bufferIndex < length)
                {
                    right = GetBufferValueInterpolated(bufferIndex * 2.0f); 
                    left = GetBufferValueInterpolated(bufferIndex * 2.0f + 1.0f);
                    bufferIndex += 1.0f; // change to value other than 1 for different playback speed
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
    //hardware initialize
    hardware.Configure();
    hardware.Init();

    // logging over serial USB
	#ifdef LOGG
	hardware.StartLog(true); 
	hardware.PrintLine("monitoring started");
	#endif
    
    //store audio settings
    size_t blocksize = 4;
    sysSampleRate = hardware.AudioSampleRate();

    //buttons
    Switch recButton;
    Switch playButton;
    recButton.Init(hardware.GetPin(25), 1000);
    playButton.Init(hardware.GetPin(28), 1000);

	// start callback
	hardware.SetAudioBlockSize(blocksize);
    hardware.StartAudio(AudioCallback);

    //mount sd card
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

    //read from sd card to buffer
    char sampleName[] = "loop.wav"; 
    int error = SetSample(sampleName);
    hardware.PrintLine("setsample error: %d", error);

    //intialze wavwriter
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
            record = true;
            saved = false;
            writer.Write();
        }
        else{
            record = false;
        }

        if(!record && !saved){
            writer.SaveFile();
            recordedLength = writer.GetLengthSamps();
            saved = true;
        }

        if (recButton.RisingEdge()){
            writer.OpenFile("loop.wav"); // Open WAV file
            bufferIndex = 0; 
        }
    
        if (playButton.RisingEdge()){ //toggles playback
            bufferIndex = 0;
            play = !play; 
        } 
        
    }
}
