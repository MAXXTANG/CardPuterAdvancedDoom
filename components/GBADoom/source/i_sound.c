/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  System interface for sound.
 *
 *-----------------------------------------------------------------------------
 */
#define SOC_I2S_SUPPORTS_DAC 1
#include "doomtype.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s.h"
#include "driver/i2c_master.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include <math.h>
#include <unistd.h>



#include "z_zone.h"

#include "m_swap.h"
#include "i_sound.h"

#include "m_misc.h"
#include "w_wad.h"
#include "lprintf.h"
#include "s_sound.h"

#include "doomdef.h"
#include "doomstat.h"

#include "d_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "global_data.h"
#include "oplplayer.h"
#include "mus2mid.h"

// ES8311 Codec definitions for Cardputer Advanced v1.2
#define ES8311_I2C_ADDR         0x18  // ES8311 I2C address (0x18 or 0x19 depending on pin config)
#define ES8311_CHIP_CONTROL1    0x00
#define ES8311_CHIP_CONTROL2    0x01
#define ES8311_CHIP_POWER       0x02
#define ES8311_ADC_CONTROL      0x03
#define ES8311_DAC_CONTROL17    0x17
#define ES8311_DAC_CONTROL16    0x16
#define ES8311_GPIO_CONTROL     0x44
#define ES8311_SYSTEM_CONTROL   0x0D
#define ES8311_DAC_VOLUME       0x32

// I2C bus handle (shared with keyboard)
extern i2c_master_bus_handle_t i2c_bus_handle_;

static const char* SOUND_TAG = "DOOM_SOUND";
static const music_player_t *music_player = &opl_synth_player;
static bool musicPlaying = false;

//extern SemaphoreHandle_t dmaChannel2Sem;
extern bool audioStarted;

//SemaphoreHandle_t dmaChannel2Sem = NULL;
bool audioStarted = false;

#define SAMPLERATE		11025	// Hz
#define SAMPLESIZE		2   	// 16bit

int snd_card = 1;
int mus_card = 1;
int snd_samplerate = 11025;

int channelsOut = 1;

// Needed for calling the actual sound output.
#define SAMPLECOUNT		512
#define NUM_MIX_CHANNELS		8
// It is 2 for 16bit, and 2 for two channels.
//#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT*4)


// The actual lengths of all sound effects.
int 		lengths[NUMSFX];

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
unsigned char	*mixbuffer;

// The channel data pointers, start and end.
unsigned char*	channels[NUM_MIX_CHANNELS];
unsigned char*	channelsend[NUM_MIX_CHANNELS];

// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_MIX_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
//int 		channelhandles[NUM_MIX_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_MIX_CHANNELS];			

// Hardware left and right channel volume lookup.
//int		channelleftvol_lookup[NUM_MIX_CHANNELS];
//int		channelrightvol_lookup[NUM_MIX_CHANNELS];

//
// This function loads the sound data from the WAD lump,
//  for single sound.
//

void* getsfx(char* sfxname, int* len)
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
      sfxlump = W_GetNumForName("dspistol");
    else
      sfxlump = W_GetNumForName(name);
    
    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    //paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    //paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    //memcpy(  paddedsfx, sfx, size );
    //for (i=size ; i<paddedsize+8 ; i++)
    //    paddedsfx[i] = 128;

    // Remove the cached lump.
    //Z_Free( sfx );
    //W_UnlockLumpNum( sfxlump );

    // Preserve padded length.
    //*len = paddedsize;
    *len = size;
    return (void *) (sfx);

    // Return allocated padded data.
    //return (void *) (paddedsfx + 8);
}


// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int addsfx(int	sfxid, int volume, int step, int seperation)
{
    static unsigned short	handlenums = 0;

    int		i;
    int		rc = -1;
    
    int		oldest = _g->gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
      || sfxid == sfx_sawidl
      || sfxid == sfx_sawful
      || sfxid == sfx_sawhit
      || sfxid == sfx_stnmov
      || sfxid == sfx_pistol	 )
    {
      // Loop all channels, check.
      for (i=0 ; i<NUM_MIX_CHANNELS ; i++)
      {
          // Active, and using the same SFX?
          if ( (channels[i]) && (channelids[i] == sfxid) )
          {
            // Reset.
            channels[i] = 0;
            // We are sure that iff,
            //  there will only be one.
            break;
          }
      }
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_MIX_CHANNELS) && (channels[i]); i++)
    {
      if (channelstart[i] < oldest)
      {
          oldestnum = i;
          oldest = channelstart[i];
      }
    }

    // Tales from the cryptic.
    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    // Probably only happens at startup.
    if (i == NUM_MIX_CHANNELS)
	    slot = oldestnum;
    else
	    slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];
/*
    // Reset current handle number, limited to 0..100.
    if (!handlenums)
	    handlenums = 100;

    // Assign current handle number.
    // Preserved so sounds could be stopped (unused).
    channelhandles[slot] = rc = handlenums++;

    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol = volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol = volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	    I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	    I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channelleftvol_lookup[slot] = leftvol;//&vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = rightvol;//&vol_lookup[rightvol*256];
*/
    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}

void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch)
{
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  _g->snd_SfxVolume = volume;
}


//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  
}	

// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
  printf("I_GetSfxLumpNum(sfxinfo_t* sfx) sfx->name %s\n", sfx->name);
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
  printf("I_StartSound(%d %d %d %d %d %d)\n",id, channel,  vol,  sep,  pitch,  priority);
  //fprintf( stderr, "starting sound %d", id );
  
  // Returns a handle (not used).
  id = addsfx( id, vol, 0, sep );

  // fprintf( stderr, "/handle is %d\n", id );
  
  return id;
}



void I_StopSound (int handle)
{
}


int I_SoundIsPlaying(int handle)
{
    return _g->gametic < handle;
}


int I_AnySoundStillPlaying(void)
{
  return false;
}

// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//

__attribute__((no_instrument_function)) void IRAM_ATTR I_UpdateSound( void );

void IRAM_ATTR I_UpdateSound( void )
{
  // Mix current sound data - 16-bit stereo for ES8311
  unsigned char sample;
  int dl;  // Use int for proper mixing range
  
  // Pointer to output buffer (16-bit stereo interleaved for ES8311)
  int16_t* outptr;

  // Mixing channel index.
  int chan;
    
  int16_t stream[2] = {0};

  // Output pointer for 16-bit stereo interleaved samples
  outptr = (int16_t*)mixbuffer;

  // Mix sounds into the mixing buffer.
  // Loop over SAMPLECOUNT samples
  for (int i = 0; i < SAMPLECOUNT; i++)
  {
    // Reset value
    dl = 0;

    // Mix all active sound channels
    for ( chan = 0; chan < NUM_MIX_CHANNELS; chan++ )
    {
        // Check channel, if active.
        if (channels[ chan ])
        {
          // Get the raw data from the channel. 
          sample = *channels[ chan ];
          // Convert 8-bit unsigned (0-255) to signed and scale by volume
          // Sample: 0-255, center at 128
          int samp_signed = ((int)sample - 128);
          // Scale by volume (0-15) and amplify
          samp_signed = (samp_signed * _g->snd_SfxVolume * 8);
          dl += samp_signed;
          // Increment index
          channels[ chan ] += 1;

          // Check whether we are done.
          if (channels[ chan ] >= channelsend[ chan ])
              channels[ chan ] = 0;
        }
    }

    // Mix music
    if (musicPlaying && _g->snd_MusicVolume > 0)
    {
        music_player->render(&stream, 1); // Returns 2 (stereo) 16bit values per sample
        int m_sample = stream[0]; // [0] and [1] are the same value
        // Scale music by volume (volume 0-15) and amplify to match SFX levels
        int v = (m_sample * _g->snd_MusicVolume * 4) / 15;
        dl += v;
    }
    
    // Clamp to 16-bit signed range
    if(dl > 32767) {
      dl = 32767;
    }
    if(dl < -32768) {
      dl = -32768;
    }
    
    // Write stereo 16-bit sample (same value on both channels)
    *outptr++ = (int16_t)dl;  // Left
    *outptr++ = (int16_t)dl;  // Right (duplicate)
  }
}

void I_ShutdownSound(void)
{
      music_player->shutdown();
  //i2s_driver_uninstall(I2S_NUM_0); //stop & destroy i2s driver
}

extern void __play_sound(unsigned char * buf, unsigned int size);


__attribute__((no_instrument_function)) void IRAM_ATTR updateTask(void *arg);
void IRAM_ATTR updateTask(void *arg) 
{
  //printf("i_sound.c : updateTask()\n");
  size_t bytesWritten;
  while(1)
  {
    //xSemaphoreTake(dmaChannel2Sem, portMAX_DELAY);
    
    
    I_UpdateSound();
  
  
    //printf("sound: updateTask()\n");
    //for(int i = 0; i<SAMPLECOUNT*SAMPLESIZE; i++) {
    //  printf("%d ", mixbuffer[i]);
    //}
    //printf("\n");
    
    // Write stereo 16-bit samples: SAMPLECOUNT samples * 2 channels * 2 bytes per sample
    i2s_write(I2S_NUM_1, mixbuffer, SAMPLECOUNT * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    
    //__play_sound(mixbuffer, SAMPLECOUNT*SAMPLESIZE);
    //xSemaphoreGive(dmaChannel2Sem);
  }
}

#define I2S_DAC_CHANNEL_LEFT_EN 2
#define I2S_MODE_DAC_BUILT_IN  (0x1 << 4)

/**
 * @brief Initialize ES8311 codec via I2C for Cardputer Advanced v1.2
 * Uses the exact register sequence from M5Unified library
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t ES8311_Init(void)
{
    ESP_LOGI(SOUND_TAG, "Initializing ES8311 codec (M5Unified sequence)...");
    
    if (i2c_bus_handle_ == NULL) {
        ESP_LOGE(SOUND_TAG, "I2C bus handle is NULL! Initialize keyboard first.");
        return ESP_FAIL;
    }
    
    esp_err_t ret;
    uint8_t write_buf[2];
    
    // Create I2C device handle for ES8311
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t es8311_handle;
    ret = i2c_master_bus_add_device(i2c_bus_handle_, &dev_cfg, &es8311_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(SOUND_TAG, "Failed to add ES8311 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // M5Unified ES8311 initialization sequence for Cardputer Advanced
    // Register sequence from _speaker_enabled_cb_cardputer_adv()
    
    // 0x00 RESET/ CSM POWER ON
    write_buf[0] = 0x00;
    write_buf[1] = 0x80;
    ret = i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(SOUND_TAG, "ES8311 reg 0x00 write failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(es8311_handle);
        return ret;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    // 0x01 CLOCK_MANAGER/ MCLK=BCLK
    write_buf[0] = 0x01;
    write_buf[1] = 0xB5;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x02 CLOCK_MANAGER/ MULT_PRE=3
    write_buf[0] = 0x02;
    write_buf[1] = 0x18;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x0D SYSTEM/ Power up analog circuitry
    write_buf[0] = 0x0D;
    write_buf[1] = 0x01;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x12 SYSTEM/ power-up DAC - NOT default
    write_buf[0] = 0x12;
    write_buf[1] = 0x00;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x13 SYSTEM/ Enable output to HP drive - NOT default
    write_buf[0] = 0x13;
    write_buf[1] = 0x10;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x32 DAC/ DAC volume (0xBF == ±0 dB)
    write_buf[0] = 0x32;
    write_buf[1] = 0xBF;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    // 0x37 DAC/ Bypass DAC equalizer - NOT default
    write_buf[0] = 0x37;
    write_buf[1] = 0x08;
    i2c_master_transmit(es8311_handle, write_buf, 2, 1000 / portTICK_PERIOD_MS);
    
    ESP_LOGI(SOUND_TAG, "ES8311 codec initialized successfully");
    
    return ESP_OK;
}

void I_InitSound(void)
{
#if 0
  //debug minimal - Updated for Cardputer Advanced v1.2
  mixbuffer = malloc(MIXBUFFERSIZE*sizeof(unsigned char));
  static const i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_TX/*I2S_MODE_PDM, I2S_MODE_DAC_BUILT_IN*/,
    .sample_rate = SAMPLERATE,
    .bits_per_sample = SAMPLESIZE*8, /* the DAC module will only take the 8bits from MSB */
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0, // default interrupt priority
    .dma_buf_count = 4,
    .dma_buf_len = 64,
    .use_apll = false
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);   //install and start i2s driver
  i2s_pin_config_t pin_cfg;
  pin_cfg.bck_io_num = 1;          // ES8311 BCLK
  pin_cfg.data_in_num = -1;
  pin_cfg.data_out_num = 3;        // ES8311 DIN
  pin_cfg.mck_io_num = -1;
  pin_cfg.ws_io_num = 2;           // ES8311 LRCK  

  i2s_set_pin(I2S_NUM_0, &pin_cfg); //for internal DAC, this will enable both of the internal channels
  i2s_set_sample_rates(I2S_NUM_0, SAMPLERATE); //set sample rates
  
  for ( int i = 0; i< MIXBUFFERSIZE; i++ )
    mixbuffer[i] = i;
  
  xTaskCreatePinnedToCore(&updateTask, "updateTask", 4000, NULL, 1, NULL, 1);  // Priority 1, Core 1
#endif

  printf("[SOUND] I_InitSound() ENTRY\n");
  fflush(stdout);

  if((snd_card == 0) && (mus_card == 0)) {
    printf("[SOUND] Audio cards disabled, skipping init\n");
    fflush(stdout);
    return;
  }

  printf("[SOUND] Allocating mixbuffer (%d bytes)...\n", MIXBUFFERSIZE);
  fflush(stdout);
  mixbuffer = malloc(MIXBUFFERSIZE*sizeof(unsigned char));
  printf("[SOUND] mixbuffer allocated at %p\n", (void*)mixbuffer);
  fflush(stdout);
  
  printf("[SOUND] Installing I2S driver...\n");
  fflush(stdout);
  static const i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_TX,
    .sample_rate = SAMPLERATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // Stereo for ES8311
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0, // default interrupt priority
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false
  };

  // Use I2S_NUM_1 for Cardputer Advanced (per M5Unified)
  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);   //install and start i2s driver
  printf("[SOUND] I2S driver installed\n");
  fflush(stdout);

  // Configure I2S pins for Cardputer Advanced v1.2 with ES8311
  // Official M5Unified pinout: BCLK=GPIO41, WS=GPIO43, DOUT=GPIO42
  i2s_pin_config_t pin_cfg;
  pin_cfg.bck_io_num = 41;         // Bit Clock (BCLK)
  pin_cfg.data_in_num = -1;        // Not used (no input)
  pin_cfg.data_out_num = 42;       // Data Out (SDATA to ES8311)
  pin_cfg.mck_io_num = -1;         // Master clock not used
  pin_cfg.ws_io_num = 43;          // Word Select / LRCK

  i2s_set_pin(I2S_NUM_1, &pin_cfg);
  printf("[SOUND] I2S pins configured\n");
  fflush(stdout);
  
  // Initialize ES8311 codec via I2C
  printf("[SOUND] Initializing ES8311 codec...\n");
  fflush(stdout);
  esp_err_t codec_ret = ES8311_Init();
  if (codec_ret != ESP_OK) {
    ESP_LOGE(SOUND_TAG, "ES8311 initialization failed! Audio will not work.");
  }
  printf("[SOUND] ES8311 codec init complete (ret=%d)\n", codec_ret);
  fflush(stdout);

  //i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);  
/*    
      Values:

      I2S_DAC_CHANNEL_DISABLE = 0
      Disable I2S built-in DAC signals

      I2S_DAC_CHANNEL_RIGHT_EN = 1
      Enable I2S built-in DAC right channel, maps to DAC channel 1 on GPIO25

      I2S_DAC_CHANNEL_LEFT_EN = 2
      Enable I2S built-in DAC left channel, maps to DAC channel 2 on GPIO26

      I2S_DAC_CHANNEL_BOTH_EN = 0x3
      Enable both of the I2S built-in DAC channels.

      I2S_DAC_CHANNEL_MAX = 0x4
      I2S built-in DAC mode max index    
*/

  i2s_set_sample_rates(I2S_NUM_1, SAMPLERATE); //set sample rates
  audioStarted = true;

  // Initialize external data (all sounds) at start, keep static.
  lprintf( LO_INFO, "I_InitSound: ");
  
  for (int i=1 ; i<NUMSFX ; i++)
  { 
    // Alias? Example is the chaingun sound linked to pistol.
    if (!S_sfx[i].link)
    {
      // Load data from WAD file.
      S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
    }	
    else
    {
      // Previously loaded already?
      S_sfx[i].data = S_sfx[i].link->data;
      lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
    }
  }

  lprintf( LO_INFO, " pre-cached all sound data\n");
  
  // Now initialize mixbuffer with zero.
  for ( int i = 0; i< MIXBUFFERSIZE; i++ )
    mixbuffer[i] = i/8;
  
  // Finished initialization.
  lprintf(LO_INFO, "I_InitSound: sound module ready\n");

    audioStarted = true;

  if(mus_card) {    

    if(music_player->init(snd_samplerate)) {
      printf("music_player->init() success\n");
      mus_card = 1;
    } else {
      mus_card = 0;
      printf("music_player->init() failed\n");
    }
   
    printf("music_player: %p\n", music_player);
  }

  if(mus_card) {
    music_player->setvolume(15/*_g->snd_MusicVolume*/);
  }  

  printf("I_InitSound: Creating audio task on core 1\n");
  xTaskCreatePinnedToCore(&updateTask, "updateTask", 4000, NULL, 1, NULL, 1);  // Priority 1, Core 1

}




void I_ShutdownMusic(void)
{
      music_player->shutdown();
}

void I_InitMusic(void)
{
}

void I_PlaySong(int handle, int looping)
{
    printf("I_PlaySong called: handle=%d, looping=%d\n", handle, looping);
    music_player->play((void *)handle, looping);
    musicPlaying = true;
    printf("I_PlaySong: musicPlaying set to true\n");
}

extern int mus_pause_opt; // From m_misc.c

void I_PauseSong (int handle)
{
    music_player->pause();
    musicPlaying = false;
}

void I_ResumeSong (int handle)
{
    music_player->resume();
    musicPlaying = true;
}

void I_StopSong(int handle)
{
    music_player->stop();
    musicPlaying = false;
}

void I_UnRegisterSong(int handle)
{
    music_player->unregistersong((void *)handle);
}

int I_RegisterSong(const void *data, size_t len)
{
    uint8_t *mid = NULL;
    size_t midlen;
    int handle = 0;

    printf("I_RegisterSong: data=%p, len=%zu\n", data, len);

    // WAD already contains pre-converted MIDI (converted from MUS during WAD build)
    // This matches romalik's approach where GbaWadUtil converts MUS->MIDI at build time
/*
    if (mus2mid(data, len, &mid, &midlen, 64) == 0)
        handle = (int)music_player->registersong(mid, midlen);
    else
*/
    handle = (int)music_player->registersong(data, len);
    printf("I_RegisterSong: registersong returned handle=%d\n", handle);

    return handle;
}

int I_RegisterMusic( const char* filename, musicinfo_t *song )
{
    return 1;
}

void I_SetMusicVolume(int volume)
{
      music_player->setvolume(volume);
}


