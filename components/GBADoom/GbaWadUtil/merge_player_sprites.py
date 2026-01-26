#!/usr/bin/env python3
"""
Merge player sprites (PLAY*) from original DOOM WAD into GbaWadUtil-converted WAD.

GbaWadUtil strips player sprites since the GBA port was single-player only.
This script restores them for multiplayer support.

Usage:
    python merge_player_sprites.py

Required files in same directory:
    - doom1.wad (or doom2.wad) - Original DOOM WAD with player sprites
    - gdoom1_with_audio.wad (or gdoom1.wad) - GbaWadUtil converted WAD
    
Output:
    - gdoom1_complete.wad - WAD with player sprites added
    - Updates ../source/iwad/doom1.c with new WAD data
"""

import struct
import sys
import os


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


def find_sprite_section(lumps):
    """Find the S_START and S_END markers in the lump list."""
    s_start_idx = None
    s_end_idx = None
    
    for i, lump in enumerate(lumps):
        if lump['name'] == 'S_START':
            s_start_idx = i
        elif lump['name'] == 'S_END':
            s_end_idx = i
            break
    
    return s_start_idx, s_end_idx


def get_player_sprites(lumps):
    """Extract all PLAY* sprite lumps from original WAD."""
    player_sprites = []
    
    # Player sprite naming: PLAYxy where x=frame (A-W), y=rotation (0-8, 0=all angles)
    # Full list: PLAYA0, PLAYA1-A8, PLAYB0, etc. through death/gib frames
    
    for lump in lumps:
        name = lump['name']
        if name.startswith('PLAY') and len(name) >= 5 and len(name) <= 8:
            # Verify it looks like a sprite (has frame letter and rotation)
            if len(lump['data']) > 0:  # Has actual data
                player_sprites.append(lump)
    
    return player_sprites


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Try to find original WAD (doom1.wad or doom2.wad)
    original_wad = None
    for wad_name in ['doom1.wad', 'doom2.wad', 'doomu.wad', 'DOOM1.WAD', 'DOOM2.WAD']:
        path = os.path.join(script_dir, wad_name)
        if os.path.exists(path):
            original_wad = path
            break
    
    if not original_wad:
        print("ERROR: No original DOOM WAD found!")
        print("Please place doom1.wad or doom2.wad in:", script_dir)
        sys.exit(1)
    
    # Try to find converted WAD (prefer one with audio already merged)
    converted_wad = None
    for wad_name in ['gdoom1_with_audio.wad', 'gdoom1.wad', 'gdoom2.wad']:
        path = os.path.join(script_dir, wad_name)
        if os.path.exists(path):
            converted_wad = path
            break
    
    if not converted_wad:
        print("ERROR: No converted WAD found!")
        print("Please run GbaWadUtil first to create gdoom1.wad")
        sys.exit(1)
    
    output_wad = os.path.join(script_dir, 'gdoom1_complete.wad')
    output_c = os.path.join(script_dir, '..', 'source', 'iwad', 'doom1.c')
    
    print("=== DOOM WAD Player Sprite Merger ===\n")
    
    # Read original WAD
    print(f"Reading original WAD: {original_wad}")
    orig_sig, orig_lumps, _ = read_wad(original_wad)
    print(f"  Signature: {orig_sig}, {len(orig_lumps)} lumps")
    
    # Read converted WAD
    print(f"Reading converted WAD: {converted_wad}")
    conv_sig, conv_lumps, _ = read_wad(converted_wad)
    print(f"  Signature: {conv_sig}, {len(conv_lumps)} lumps")
    
    # Get player sprites from original
    print("\nSearching for player sprites in original WAD...")
    player_sprites = get_player_sprites(orig_lumps)
    
    if not player_sprites:
        print("ERROR: No player sprites found in original WAD!")
        sys.exit(1)
    
    print(f"Found {len(player_sprites)} player sprite lumps:")
    total_size = 0
    for sprite in player_sprites:
        print(f"  {sprite['name']}: {sprite['size']} bytes")
        total_size += sprite['size']
    print(f"Total player sprite data: {total_size} bytes ({total_size/1024:.1f} KB)")
    
    # Check which sprites are already in converted WAD
    conv_names = {l['name'] for l in conv_lumps}
    existing = [s for s in player_sprites if s['name'] in conv_names]
    new_sprites = [s for s in player_sprites if s['name'] not in conv_names]
    
    if existing:
        print(f"\nSprites already in converted WAD: {len(existing)}")
        for s in existing[:5]:
            print(f"  {s['name']}")
        if len(existing) > 5:
            print(f"  ... and {len(existing) - 5} more")
    
    print(f"\nNew sprites to add: {len(new_sprites)}")
    
    if new_sprites:
        # Find sprite section in converted WAD
        s_start, s_end = find_sprite_section(conv_lumps)
        
        if s_start is not None and s_end is not None:
            print(f"\nSprite section found at lumps {s_start} (S_START) to {s_end} (S_END)")
            
            # Insert new sprites just after S_START (before existing sprites)
            insert_pos = s_start + 1
            
            # Build new lump list
            merged_lumps = (
                conv_lumps[:insert_pos] + 
                new_sprites + 
                conv_lumps[insert_pos:]
            )
            
            print(f"Inserting {len(new_sprites)} sprites after S_START")
        else:
            print("\nWARNING: S_START/S_END markers not found, appending sprites at end")
            merged_lumps = conv_lumps + new_sprites
    
        print(f"\nMerged WAD will have {len(merged_lumps)} lumps")
    
        # Write merged WAD
        print(f"\nWriting merged WAD: {output_wad}")
        wad_size = write_wad(output_wad, 'IWAD', merged_lumps)
        print(f"  WAD size: {wad_size} bytes ({wad_size/1024/1024:.2f} MB)")
    else:
        print("All player sprites already present! No changes needed.")
        # Just use converted WAD as-is for C generation
        output_wad = converted_wad
    
    # Convert to C source
    print(f"\nGenerating C source: {output_c}")
    c_size = wad_to_c_source(output_wad, output_c)
    print(f"  Array size: {c_size} bytes")
    
    # Verify the sprites are now in the output
    print("\nVerifying output WAD...")
    final_wad = output_wad if new_sprites else converted_wad
    _, verify_lumps, _ = read_wad(final_wad)
    verify_play = [l for l in verify_lumps if l['name'].startswith('PLAY')]
    print(f"  PLAY* sprites in output: {len(verify_play)}")
    
    print("\n=== Done! ===")
    print(f"\nPlayer sprites added: {len(new_sprites)}")
    print(f"Total WAD size: {c_size} bytes ({c_size/1024/1024:.2f} MB)")
    print(f"\nC source updated: {output_c}")
    print("\nRebuild the firmware to include player sprites.")
    
    return c_size


if __name__ == '__main__':
    try:
        size = main()
        print(f"\nNEW_SIZE={size}")
    except FileNotFoundError as e:
        print(f"ERROR: File not found: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
