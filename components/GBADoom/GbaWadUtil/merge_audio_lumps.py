#!/usr/bin/env python3
"""
Merge audio lumps (GENMIDI and DS* sounds) from original DOOM WAD
into GbaWadUtil-converted WAD, then output as C source file.

This script also converts MUS music format to MIDI format during
the merge process, matching romalik's GbaWadUtil approach.
"""

import struct
import sys
import os

# MUS to MIDI conversion (ported from mus2mid.c)
# MUS event types
RELEASE_NOTE = 0
PLAY_NOTE = 1
BEND_NOTE = 2
SYS_EVENT = 3
CNTL_CHANGE = 4
UNKNOWN_EVENT1 = 5
SCORE_END = 6
UNKNOWN_EVENT2 = 7

# MUS -> MIDI control mapping
MUS2MIDcontrol = [
    0,      # Program change - not a MIDI control change
    0x00,   # Bank select
    0x01,   # Modulation pot
    0x07,   # Volume
    0x0A,   # Pan pot
    0x0B,   # Expression pot
    0x5B,   # Reverb depth
    0x5D,   # Chorus depth
    0x40,   # Sustain pedal
    0x43,   # Soft pedal
    0x78,   # All sounds off
    0x7B,   # All notes off
    0x7E,   # Mono
    0x7F,   # Poly
    0x79,   # Reset all controllers
]

def mus2mid(mus_data, division=64):
    """Convert MUS format to MIDI format.
    
    Args:
        mus_data: bytes containing MUS data
        division: MIDI time division (default 64, same as romalik)
    
    Returns:
        bytes containing MIDI data, or None if conversion failed
    """
    if len(mus_data) < 16:
        return None
    
    # Check MUS header
    if mus_data[:4] != b'MUS\x1a':
        return None
    
    # Parse MUS header
    score_length = struct.unpack('<H', mus_data[4:6])[0]
    score_start = struct.unpack('<H', mus_data[6:8])[0]
    channels = struct.unpack('<H', mus_data[8:10])[0]
    sec_channels = struct.unpack('<H', mus_data[10:12])[0]
    instr_cnt = struct.unpack('<H', mus_data[12:14])[0]
    
    if channels > 15:
        return None
    
    muslen = score_start + score_length
    if muslen > len(mus_data):
        muslen = len(mus_data)
    
    if score_length == 0:
        return None
    
    # Track data structures
    MIDI_TRACKS = 16
    tracks = []
    for i in range(MIDI_TRACKS):
        tracks.append({
            'velocity': 64,
            'deltaT': 0,
            'data': bytearray()
        })
    
    MUS2MIDchannel = [-1] * MIDI_TRACKS
    MIDIchan2track = [0] * MIDI_TRACKS
    
    # First track is tempo/key track
    track0_data = bytes([
        0x00, 0xff, 0x59, 0x02, 0x00, 0x00,        # Key (C major)
        0x00, 0xff, 0x51, 0x03, 0x09, 0xa3, 0x1a,  # Tempo
    ])
    tracks[0]['data'].extend(track0_data)
    
    musptr = score_start
    numtracks = 1
    
    # Process MUS events
    while musptr < muslen:
        MUSevent = mus_data[musptr]
        musptr += 1
        
        MUSeventType = (MUSevent & 0x7F) >> 4
        MUSchannel = MUSevent & 0x0F
        
        if MUSeventType == SCORE_END:
            break
        
        # Assign MIDI channel if not already assigned
        if MUS2MIDchannel[MUSchannel] == -1:
            if MUSchannel == 15:
                # MUS channel 15 -> MIDI channel 9 (percussion)
                MUS2MIDchannel[MUSchannel] = 9
            else:
                # Find the maximum assigned MIDI channel (excluding -1s and 9)
                assigned = [c for c in MUS2MIDchannel if c != -1 and c != 9]
                if not assigned:
                    max_chan = -1
                else:
                    max_chan = max(assigned)
                # Skip MIDI channel 9 (percussion)
                if max_chan == 8:
                    MUS2MIDchannel[MUSchannel] = 10
                else:
                    new_chan = max_chan + 1
                    # Ensure we don't exceed MIDI channel 15
                    if new_chan >= 16:
                        new_chan = 15
                    MUS2MIDchannel[MUSchannel] = new_chan
            midi_chan = MUS2MIDchannel[MUSchannel]
            if midi_chan < 16:
                MIDIchan2track[midi_chan] = numtracks
            numtracks += 1
        
        MIDIchannel = MUS2MIDchannel[MUSchannel]
        if MIDIchannel >= 16:
            continue  # Skip invalid channels
        MIDItrack = MIDIchan2track[MIDIchannel]
        
        # Write delta time as variable-length quantity
        value = tracks[MIDItrack]['deltaT']
        buffer_bytes = []
        buffer_bytes.append(value & 0x7f)
        while value >> 7:
            value >>= 7
            buffer_bytes.append(0x80 | (value & 0x7f))
        buffer_bytes.reverse()
        tracks[MIDItrack]['data'].extend(buffer_bytes)
        tracks[MIDItrack]['deltaT'] = 0
        
        # Process event
        if MUSeventType == RELEASE_NOTE:
            tracks[MIDItrack]['data'].append(0x90 | MIDIchannel)
            data = mus_data[musptr]
            musptr += 1
            tracks[MIDItrack]['data'].append(data & 0x7F)
            tracks[MIDItrack]['data'].append(0)
            
        elif MUSeventType == PLAY_NOTE:
            tracks[MIDItrack]['data'].append(0x90 | MIDIchannel)
            data = mus_data[musptr]
            musptr += 1
            tracks[MIDItrack]['data'].append(data & 0x7F)
            if data & 0x80:
                tracks[MIDItrack]['velocity'] = mus_data[musptr] & 0x7f
                musptr += 1
            tracks[MIDItrack]['data'].append(tracks[MIDItrack]['velocity'])
            
        elif MUSeventType == BEND_NOTE:
            tracks[MIDItrack]['data'].append(0xE0 | MIDIchannel)
            data = mus_data[musptr]
            musptr += 1
            tracks[MIDItrack]['data'].append((data & 1) << 6)
            tracks[MIDItrack]['data'].append(data >> 1)
            
        elif MUSeventType == SYS_EVENT:
            tracks[MIDItrack]['data'].append(0xB0 | MIDIchannel)
            data = mus_data[musptr]
            musptr += 1
            if data < 10 or data > 14:
                return None  # Bad event
            tracks[MIDItrack]['data'].append(MUS2MIDcontrol[data])
            if data == 12:
                tracks[MIDItrack]['data'].append(channels + 1)
            else:
                tracks[MIDItrack]['data'].append(0)
                
        elif MUSeventType == CNTL_CHANGE:
            data = mus_data[musptr]
            musptr += 1
            if data > 9:
                return None  # Bad event
            if data:
                tracks[MIDItrack]['data'].append(0xB0 | MIDIchannel)
                tracks[MIDItrack]['data'].append(MUS2MIDcontrol[data])
            else:
                tracks[MIDItrack]['data'].append(0xC0 | MIDIchannel)
            data = mus_data[musptr]
            musptr += 1
            tracks[MIDItrack]['data'].append(data & 0x7F)
            
        else:
            return None  # Unknown event
        
        # Check for delay
        if MUSevent & 0x80:
            DeltaTime = 0
            while True:
                byte = mus_data[musptr]
                musptr += 1
                DeltaTime = (DeltaTime << 7) + (byte & 0x7F)
                if not (byte & 0x80):
                    break
            for i in range(MIDI_TRACKS):
                tracks[i]['deltaT'] += DeltaTime
        
        if musptr >= muslen:
            break
    
    # Build MIDI file
    # Count tracks with data
    active_tracks = []
    for i in range(MIDI_TRACKS):
        if len(tracks[i]['data']) > 0:
            active_tracks.append(i)
    
    # MIDI header
    midi_data = bytearray()
    midi_data.extend(b'MThd')
    midi_data.extend(struct.pack('>I', 6))  # Header length
    midi_data.extend(struct.pack('>H', 1))  # Format 1
    midi_data.extend(struct.pack('>H', len(active_tracks)))  # Number of tracks
    midi_data.extend(struct.pack('>H', division & 0x7fff))  # Division
    
    # Write each track
    for i in active_tracks:
        track_data = tracks[i]['data']
        # Add end of track marker
        track_data.extend([0x00, 0xFF, 0x2F, 0x00])
        
        midi_data.extend(b'MTrk')
        midi_data.extend(struct.pack('>I', len(track_data)))
        midi_data.extend(track_data)
    
    return bytes(midi_data)

def read_wad(filename):
    """Read WAD file and return header info and lump directory."""
    with open(filename, 'rb') as f:
        data = f.read()
    
    sig, numlumps, dirofs = struct.unpack('<4sII', data[:12])
    sig = sig.decode('ascii')
    
    lumps = []
    for i in range(numlumps):
        entry_ofs = dirofs + i * 16
        offset, size, name = struct.unpack('<II8s', data[entry_ofs:entry_ofs+16])
        name = name.rstrip(b'\x00').decode('ascii', errors='ignore')
        lump_data = data[offset:offset+size] if size > 0 else b''
        lumps.append({
            'name': name,
            'size': size,
            'data': lump_data
        })
    
    return sig, lumps, data

def write_wad(filename, sig, lumps):
    """Write lumps to a new WAD file."""
    # Calculate positions
    data_start = 12  # After header
    current_offset = data_start
    
    # Build lump data section
    lump_data = b''
    directory = []
    
    for lump in lumps:
        if lump['size'] > 0:
            offset = current_offset
            lump_data += lump['data']
            current_offset += lump['size']
        else:
            offset = 0
        
        directory.append({
            'offset': offset,
            'size': lump['size'],
            'name': lump['name']
        })
    
    dir_offset = current_offset
    
    # Build directory
    dir_data = b''
    for entry in directory:
        name_bytes = entry['name'].encode('ascii')[:8].ljust(8, b'\x00')
        dir_data += struct.pack('<II', entry['offset'], entry['size']) + name_bytes
    
    # Build header
    header = struct.pack('<4sII', sig.encode('ascii'), len(lumps), dir_offset)
    
    # Write file
    with open(filename, 'wb') as f:
        f.write(header)
        f.write(lump_data)
        f.write(dir_data)
    
    return len(header) + len(lump_data) + len(dir_data)

def wad_to_c_source(wad_filename, c_filename, array_name='doom_iwad_builtin'):
    """Convert WAD file to C source code."""
    with open(wad_filename, 'rb') as f:
        data = f.read()
    
    size = len(data)
    
    with open(c_filename, 'w') as f:
        f.write(f'unsigned char * doom_iwad;\n')
        f.write(f'const unsigned char {array_name}[{size}UL] = {{\n')
        
        # Write data in rows of 40 bytes
        bytes_per_line = 40
        for i in range(0, size, bytes_per_line):
            chunk = data[i:i+bytes_per_line]
            hex_str = ','.join(f'0x{b:02x}' for b in chunk)
            if i + bytes_per_line < size:
                f.write(hex_str + ',\n')
            else:
                f.write(hex_str + '\n')
        
        f.write('};\n')
    
    return size

def main():
    # File paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    original_wad = os.path.join(script_dir, 'doom1.wad')
    converted_wad = os.path.join(script_dir, 'gdoom1.wad')
    output_wad = os.path.join(script_dir, 'gdoom1_with_audio.wad')
    output_c = os.path.join(script_dir, '..', 'source', 'iwad', 'doom1.c')
    
    print("=== DOOM WAD Audio Lump Merger ===\n")
    
    # Read original WAD
    print(f"Reading original WAD: {original_wad}")
    orig_sig, orig_lumps, _ = read_wad(original_wad)
    print(f"  Signature: {orig_sig}, {len(orig_lumps)} lumps")
    
    # Read converted WAD
    print(f"Reading converted WAD: {converted_wad}")
    conv_sig, conv_lumps, _ = read_wad(converted_wad)
    print(f"  Signature: {conv_sig}, {len(conv_lumps)} lumps")
    
    # Find audio lumps in original
    audio_lumps = []
    music_converted = 0
    for lump in orig_lumps:
        name = lump['name']
        # GENMIDI for OPL instrument data
        # DS* for sound effects
        # D_* for music lumps (D_E1M1, D_INTRO, etc.)
        if name == 'GENMIDI' or name.startswith('DS') or (name.startswith('D_') and len(name) <= 8):
            # Convert music lumps from MUS to MIDI format (like romalik's GbaWadUtil)
            if name.startswith('D_'):
                midi_data = mus2mid(lump['data'], division=64)
                if midi_data:
                    print(f"  Converted music: {name} MUS({lump['size']} bytes) -> MIDI({len(midi_data)} bytes)")
                    lump = {
                        'name': name,
                        'size': len(midi_data),
                        'data': midi_data
                    }
                    music_converted += 1
                else:
                    print(f"  WARNING: Failed to convert {name}, keeping original MUS format")
            else:
                print(f"  Found audio lump: {name} ({lump['size']} bytes)")
            audio_lumps.append(lump)
    
    print(f"\nTotal audio lumps to merge: {len(audio_lumps)}")
    print(f"Music lumps converted MUS->MIDI: {music_converted}")
    
    # Check which lumps are already in converted WAD
    conv_names = {l['name'] for l in conv_lumps}
    new_lumps = [l for l in audio_lumps if l['name'] not in conv_names]
    print(f"New lumps to add: {len(new_lumps)}")
    
    # Merge: add audio lumps to converted WAD
    merged_lumps = conv_lumps + new_lumps
    print(f"\nMerged WAD will have {len(merged_lumps)} lumps")
    
    # Write merged WAD
    print(f"\nWriting merged WAD: {output_wad}")
    wad_size = write_wad(output_wad, 'IWAD', merged_lumps)
    print(f"  WAD size: {wad_size} bytes")
    
    # Convert to C source
    print(f"\nGenerating C source: {output_c}")
    c_size = wad_to_c_source(output_wad, output_c)
    print(f"  Array size: {c_size} bytes")
    
    print("\n=== Done! ===")
    print(f"Update main.cpp to use array size: {c_size}UL")
    
    return c_size

if __name__ == '__main__':
    size = main()
    # Print size for script capture
    print(f"\nNEW_SIZE={size}")
