/* MUS to MIDI converter header */

#ifndef MUS2MID_H
#define MUS2MID_H

#include <stdint.h>
#include <stddef.h>

/**
 * Convert MUS format music to MIDI format
 * 
 * @param mus      Pointer to MUS data
 * @param muslen   Length of MUS data
 * @param mid      Output: Pointer to allocated MIDI data (caller must free)
 * @param midlen   Output: Length of MIDI data
 * @param division MIDI time division (typically 70)
 * @return 0 on success, -1 on error
 */
int mus2mid(const void *mus, size_t muslen, uint8_t **mid, size_t *midlen, uint16_t division);

#endif /* MUS2MID_H */
