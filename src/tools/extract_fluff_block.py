#!/usr/bin/env python3
"""
Extract raw fluff bitstream blocks from .bolt archives.
Reads the first file entry and saves compressed blocks for analysis.
"""

import struct
import sys
import os

def read_u8(f):
    b = f.read(1)
    if not b:
        raise EOFError("Unexpected EOF")
    return struct.unpack('B', b)[0]

def read_u16_le(f):
    b = f.read(2)
    if len(b) < 2:
        raise EOFError("Unexpected EOF")
    return struct.unpack('<H', b)[0]

def read_u32_le(f):
    b = f.read(4)
    if len(b) < 4:
        raise EOFError("Unexpected EOF")
    return struct.unpack('<I', b)[0]

def read_u64_le(f):
    b = f.read(8)
    if len(b) < 8:
        raise EOFError("Unexpected EOF")
    return struct.unpack('<Q', b)[0]

def read_bytes(f, n):
    b = f.read(n)
    if len(b) < n:
        raise EOFError(f"Unexpected EOF: wanted {n}, got {len(b)}")
    return b

def extract_first_block(bolt_path, output_path):
    """Extract the first compressed block from a .bolt archive."""
    
    if not os.path.exists(bolt_path):
        print(f"Error: {bolt_path} not found")
        return False
    
    with open(bolt_path, 'rb') as f:
        # Read archive header (32 bytes)
        magic = read_bytes(f, 4)
        if magic != b'BOLT':
            print("Error: Not a BOLT archive (invalid magic)")
            return False
        
        version = read_u16_le(f)
        flags = read_u16_le(f)
        file_count = read_u32_le(f)
        creator_os = read_u8(f)
        creator_cpu = read_u8(f)
        creator_ver = read_u16_le(f)
        archive_size = read_u64_le(f)
        reserved = read_u64_le(f)
        
        print(f"Archive version: {version:04x}")
        print(f"File count: {file_count}")
        print(f"Archive size: {archive_size} bytes")
        print()
        
        # Read first file entry header
        header_size = read_u16_le(f)
        name_len = read_u16_le(f)
        name = read_bytes(f, name_len).decode('utf-8', errors='replace')
        original_size = read_u64_le(f)
        compressed_size = read_u64_le(f)
        checksum = read_u32_le(f)
        method = read_u8(f)
        checksum_type = read_u8(f)
        block_size = read_u32_le(f)
        entry_type = read_u8(f)
        platform = read_u8(f)
        mtime = read_u64_le(f)
        flags = read_u16_le(f)
        
        print(f"File entry: {name}")
        print(f"Original size: {original_size} bytes")
        print(f"Compressed size: {compressed_size} bytes")
        print(f"Compression method: {method}")
        print(f"Block size: {block_size}")
        print(f"Entry type: {entry_type}")
        print()
        
        # Now read compressed data
        compressed_data = read_bytes(f, compressed_size)
        
        # If block_size == 0, the entire data is one block
        if block_size == 0:
            print(f"Single block archive (no block headers)")
            block_data = compressed_data
            block_index = 0
        else:
            print(f"Block-based archive ({block_size} byte blocks)")
            # Extract first block
            offset = 0
            if offset + 8 > len(compressed_data):
                print("Error: compressed data too small for block header")
                return False
            
            block_comp_size = struct.unpack('<I', compressed_data[offset:offset+4])[0]
            block_orig_size = struct.unpack('<I', compressed_data[offset+4:offset+8])[0]
            
            print(f"First block: {block_comp_size} bytes compressed, {block_orig_size} bytes original")
            
            offset += 8
            if offset + block_comp_size > len(compressed_data):
                print("Error: block data extends beyond compressed data")
                return False
            
            block_data = compressed_data[offset:offset+block_comp_size]
            block_index = 0
        
        # Write block to output
        with open(output_path, 'wb') as out_f:
            out_f.write(block_data)
        
        print(f"\nExtracted block {block_index} to: {output_path}")
        print(f"Block size: {len(block_data)} bytes")
        
        return True

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] in ('--help', '-h', 'help'):
        print(f"Usage: {sys.argv[0]} <archive.bolt> [output.raw]")
        print(f"\nExtract raw fluff bitstream block from a .bolt archive.")
        print(f"\nIf output path not specified, writes to same directory as input with")
        print(f"basename_extracted.raw suffix, or to ./raw/ if current directory has it.")
        sys.exit(0 if '--help' in sys.argv else 1)
    
    bolt_path = sys.argv[1]
    
    # Determine output path
    if len(sys.argv) > 2:
        output_path = sys.argv[2]
    else:
        # Default strategy: use same directory as input, suffix with _extracted.raw
        # Unless ./raw/ directory exists (common when running from test/)
        if os.path.isdir('raw'):
            os.makedirs('raw', exist_ok=True)
            output_path = os.path.join('raw', 'extracted_block.raw')
        else:
            base = os.path.splitext(os.path.basename(bolt_path))[0]
            input_dir = os.path.dirname(bolt_path) or '.'
            output_path = os.path.join(input_dir, f'{base}_extracted.raw')
    
    if extract_first_block(bolt_path, output_path):
        print("\nSuccess!")
        sys.exit(0)
    else:
        print("\nFailed!")
        sys.exit(1)
