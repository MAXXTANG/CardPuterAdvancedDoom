/* MUS to MIDI converter
 * Based on the mus2mid implementation from Chocolate Doom
 * Converts DOOM MUS format to Standard MIDI format
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mus2mid.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define NUM_CHANNELS 16
#define MIDI_PERCUSSION_CHAN 9
#define MUS_PERCUSSION_CHAN 15

// MUS event types
typedef enum {
    mus_releasekey = 0x00,
    mus_presskey = 0x10,
    mus_pitchwheel = 0x20,
    mus_systemevent = 0x30,
    mus_changecontroller = 0x40,
    mus_measureend = 0x50,
    mus_scoreend = 0x60,
    mus_unused = 0x70
} musevent;

// MIDI event types
typedef enum {
    midi_noteoff = 0x80,
    midi_noteon = 0x90,
    midi_pitchbend = 0xE0,
    midi_meta = 0xFF
} midievent;

// MUS file header
typedef struct {
    uint8_t id[4];           // "MUS\x1A"
    uint16_t scorelen;
    uint16_t scorestart;
    uint16_t channels;
    uint16_t sec_channels;
    uint16_t instrCnt;
    uint16_t dummy;
} musheader_t;

// Channel mapping from MUS to MIDI
static int channel_map[NUM_CHANNELS];

// Volume for each channel
static uint8_t channel_volumes[NUM_CHANNELS];

// Output MIDI data
static uint8_t *midi_data;
static size_t midi_size;
static size_t midi_capacity;

// Track data (separate from header)
static uint8_t *track_data;
static size_t track_size;
static size_t track_capacity;

// Cleanup function for error handling
static void midi_cleanup(void)
{
    if (track_data) {
        free(track_data);
        track_data = NULL;
    }
    if (midi_data) {
        free(midi_data);
        midi_data = NULL;
    }
    track_size = 0;
    track_capacity = 0;
    midi_size = 0;
    midi_capacity = 0;
}

static int midi_init(void)
{
#ifdef ESP_PLATFORM
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    printf("mus2mid: Free heap before alloc: %u bytes\n", (unsigned)free_heap);
#endif

    // Start with small buffers - MUS files are typically 1-3KB
    // Use 2KB initial, grow by 1KB as needed
    midi_capacity = 2048;
    midi_data = malloc(midi_capacity);
    if (!midi_data) {
        printf("mus2mid: Failed to allocate midi_data (%u bytes)\n", (unsigned)midi_capacity);
        return -1;
    }
    midi_size = 0;
    
    track_capacity = 2048;
    track_data = malloc(track_capacity);
    if (!track_data) {
        printf("mus2mid: Failed to allocate track_data (%u bytes)\n", (unsigned)track_capacity);
        free(midi_data);
        midi_data = NULL;
        return -1;
    }
    track_size = 0;
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channel_map[i] = -1;
        channel_volumes[i] = 127;
    }
    
    return 0;
}

static int midi_write_byte(uint8_t b)
{
    if (midi_size >= midi_capacity) {
        size_t new_cap = midi_capacity + 1024;  // Grow by 1KB
        uint8_t *new_data = realloc(midi_data, new_cap);
        if (!new_data) {
            printf("mus2mid: Failed to grow midi_data to %zu\n", new_cap);
            return -1;
        }
        midi_data = new_data;
        midi_capacity = new_cap;
    }
    midi_data[midi_size++] = b;
    return 0;
}

static int track_write_byte(uint8_t b)
{
    if (track_size >= track_capacity) {
        size_t new_cap = track_capacity + 1024;  // Grow by 1KB
        uint8_t *new_data = realloc(track_data, new_cap);
        if (!new_data) {
            printf("mus2mid: Failed to grow track_data to %zu\n", new_cap);
            return -1;
        }
        track_data = new_data;
        track_capacity = new_cap;
    }
    track_data[track_size++] = b;
    return 0;
}

static void write_time(unsigned int time)
{
    unsigned int buffer = time & 0x7F;
    
    while ((time >>= 7) != 0) {
        buffer <<= 8;
        buffer |= ((time & 0x7F) | 0x80);
    }
    
    for (;;) {
        track_write_byte(buffer & 0xFF);
        if (buffer & 0x80)
            buffer >>= 8;
        else
            break;
    }
}

static int get_midi_channel(int mus_channel)
{
    // MUS channel 15 is percussion (maps to MIDI 9)
    if (mus_channel == MUS_PERCUSSION_CHAN)
        return MIDI_PERCUSSION_CHAN;
    
    // Check if already mapped
    if (channel_map[mus_channel] >= 0)
        return channel_map[mus_channel];
    
    // Find free MIDI channel (skip percussion channel)
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i == MIDI_PERCUSSION_CHAN)
            continue;
        
        int used = 0;
        for (int j = 0; j < NUM_CHANNELS; j++) {
            if (channel_map[j] == i) {
                used = 1;
                break;
            }
        }
        
        if (!used) {
            channel_map[mus_channel] = i;
            return i;
        }
    }
    
    // No free channel, use mus channel directly
    return mus_channel;
}

// MUS controller to MIDI controller mapping
static uint8_t controller_map[] = {
    0x00,  // 0 = program change
    0x00,  // 1 = bank select
    0x01,  // 2 = modulation
    0x07,  // 3 = volume
    0x0A,  // 4 = pan
    0x0B,  // 5 = expression
    0x5B,  // 6 = reverb
    0x5D,  // 7 = chorus
    0x40,  // 8 = sustain pedal
    0x43,  // 9 = soft pedal
    0x78,  // 10 = all sounds off
    0x7B,  // 11 = all notes off
    0x7E,  // 12 = mono
    0x7F,  // 13 = poly
    0x79,  // 14 = reset all controllers
};

int mus2mid(const void *mus, size_t muslen, uint8_t **mid, size_t *midlen, uint16_t division)
{
    const uint8_t *musdata = (const uint8_t *)mus;
    const musheader_t *header;
    const uint8_t *music;
    int done = 0;
    unsigned int queuedtime = 0;
    
    *mid = NULL;
    *midlen = 0;
    
    // Check minimum size
    if (muslen < sizeof(musheader_t)) {
        printf("mus2mid: MUS data too short\n");
        return -1;
    }
    
    // Validate MUS header
    header = (const musheader_t *)musdata;
    if (memcmp(header->id, "MUS\x1A", 4) != 0) {
        printf("mus2mid: Not a MUS file (header: %02X %02X %02X %02X)\n",
               header->id[0], header->id[1], header->id[2], header->id[3]);
        return -1;
    }
    
    if (midi_init() != 0) {
        return -1;
    }
    
    // Point to start of MUS data
    music = musdata + header->scorestart;
    
    // Process MUS events
    while (!done && music < musdata + muslen) {
        uint8_t event_desc = *music++;
        int mus_channel = event_desc & 0x0F;
        int event_type = event_desc & 0x70;
        int last = event_desc & 0x80;
        
        int midi_channel = get_midi_channel(mus_channel);
        
        switch (event_type) {
            case mus_releasekey: {
                uint8_t key = *music++;
                write_time(queuedtime);
                queuedtime = 0;
                track_write_byte(midi_noteoff | midi_channel);
                track_write_byte(key & 0x7F);
                track_write_byte(0);  // velocity
                break;
            }
            
            case mus_presskey: {
                uint8_t key = *music++;
                uint8_t vol;
                if (key & 0x80) {
                    vol = *music++;
                    channel_volumes[midi_channel] = vol;
                } else {
                    vol = channel_volumes[midi_channel];
                }
                write_time(queuedtime);
                queuedtime = 0;
                track_write_byte(midi_noteon | midi_channel);
                track_write_byte(key & 0x7F);
                track_write_byte(vol & 0x7F);
                break;
            }
            
            case mus_pitchwheel: {
                uint8_t wheel = *music++;
                int pitchbend = wheel * 64;  // Scale to MIDI range
                write_time(queuedtime);
                queuedtime = 0;
                track_write_byte(midi_pitchbend | midi_channel);
                track_write_byte(pitchbend & 0x7F);
                track_write_byte((pitchbend >> 7) & 0x7F);
                break;
            }
            
            case mus_systemevent: {
                uint8_t controller = *music++;
                if (controller < 10 || controller > 14) {
                    // Skip unknown controllers
                    break;
                }
                write_time(queuedtime);
                queuedtime = 0;
                track_write_byte(0xB0 | midi_channel);  // Control change
                track_write_byte(controller_map[controller]);
                track_write_byte(0);
                break;
            }
            
            case mus_changecontroller: {
                uint8_t controller = *music++;
                uint8_t value = *music++;
                
                if (controller == 0) {
                    // Program change
                    write_time(queuedtime);
                    queuedtime = 0;
                    track_write_byte(0xC0 | midi_channel);
                    track_write_byte(value & 0x7F);
                } else if (controller < 15) {
                    write_time(queuedtime);
                    queuedtime = 0;
                    track_write_byte(0xB0 | midi_channel);
                    track_write_byte(controller_map[controller]);
                    track_write_byte(value & 0x7F);
                }
                break;
            }
            
            case mus_scoreend:
                done = 1;
                break;
            
            default:
                break;
        }
        
        // Handle timing
        if (last) {
            unsigned int delta = 0;
            do {
                uint8_t b = *music++;
                delta = (delta * 128) + (b & 0x7F);
                if (!(b & 0x80))
                    break;
            } while (music < musdata + muslen);
            queuedtime += delta;
        }
    }
    
    // Write end of track
    write_time(0);
    track_write_byte(0xFF);
    track_write_byte(0x2F);
    track_write_byte(0x00);
    
    // Build MIDI file
    // MIDI header
    midi_write_byte('M');
    midi_write_byte('T');
    midi_write_byte('h');
    midi_write_byte('d');
    midi_write_byte(0);  // Header length (big endian)
    midi_write_byte(0);
    midi_write_byte(0);
    midi_write_byte(6);
    midi_write_byte(0);  // Format 0
    midi_write_byte(0);
    midi_write_byte(0);  // 1 track
    midi_write_byte(1);
    midi_write_byte((division >> 8) & 0xFF);  // Division
    midi_write_byte(division & 0xFF);
    
    // Track header
    midi_write_byte('M');
    midi_write_byte('T');
    midi_write_byte('r');
    midi_write_byte('k');
    midi_write_byte((track_size >> 24) & 0xFF);
    midi_write_byte((track_size >> 16) & 0xFF);
    midi_write_byte((track_size >> 8) & 0xFF);
    midi_write_byte(track_size & 0xFF);
    
    // Copy track data
    for (size_t i = 0; i < track_size; i++) {
        midi_write_byte(track_data[i]);
    }
    
    // Return result - caller is responsible for freeing *mid
    *mid = midi_data;
    *midlen = midi_size;
    
    // Clear static pointers (midi_data now owned by caller)
    midi_data = NULL;
    midi_size = 0;
    midi_capacity = 0;
    
    // Free track data
    free(track_data);
    track_data = NULL;
    track_size = 0;
    track_capacity = 0;
    
    printf("mus2mid: Converted %u bytes MUS to %u bytes MIDI\n", (unsigned)muslen, (unsigned)*midlen);
    
#ifdef ESP_PLATFORM
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    printf("mus2mid: Free heap after conversion: %u bytes\n", (unsigned)free_heap);
#endif
    
    return 0;
}
