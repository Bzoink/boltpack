#!/usr/bin/env python3
"""
Bitstream analyzer for fluff compression format.

Parses a fluff-compressed block and displays:
- Header information
- Huffman tree structure (with RLE-encoded code lengths)
- Token stream statistics
- Bitstream composition (headers vs. data)
"""

import struct
import sys
from collections import defaultdict

# Fluff format constants
FLUFF_CODELEN_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
FLUFF_NUM_LITLEN = 286
FLUFF_NUM_DIST = 36
FLUFF_NUM_CODELEN = 19

# Length symbol to base length mapping (same as DEFLATE)
LEN_BASE = [3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31, 35,43,51,59, 67,83,99,115, 131,163,195,227, 258]
LEN_EXTRA = [0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0]

# Distance symbol to base distance mapping (DEFLATE + fluff extensions)
DIST_BASE = [
    1,2,3,4, 5,7,9,13,
    17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073,
    4097,6145,8193,12289, 16385,24577,
    32769, 49153, 65537, 98305, 131073, 196609
]
DIST_EXTRA = [
    0,0,0,0, 1,1,2,2,
    3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10,
    11,11,12,12, 13,13,
    14, 15, 16, 16, 17, 17
]

class BitReader:
    """Reads bits from a byte stream."""
    def __init__(self, data):
        self.data = data
        self.pos = 0  # bit position
        self.byte_pos = 0
        self.bits = 0
        self.nbits = 0
    
    def refill(self):
        """Fill bit buffer from data."""
        while self.nbits < 16 and self.byte_pos < len(self.data):
            self.bits |= self.data[self.byte_pos] << self.nbits
            self.nbits += 8
            self.byte_pos += 1
    
    def get_bit(self):
        """Read 1 bit."""
        return self.get_bits(1)
    
    def get_bits(self, n):
        """Read n bits."""
        self.refill()
        if self.nbits < n:
            raise EOFError(f"Not enough bits: need {n}, have {self.nbits}")
        val = self.bits & ((1 << n) - 1)
        self.bits >>= n
        self.nbits -= n
        self.pos += n
        return val
    
    def skip_to_byte(self):
        """Skip to next byte boundary."""
        if self.nbits > 0:
            self.bits = 0
            self.nbits = 0
    
    def tell(self):
        """Return current bit position."""
        return self.byte_pos * 8 - self.nbits

class Analyzer:
    def __init__(self, data):
        self.data = data
        self.br = BitReader(data)
        self.stats = {
            'literals': 0,
            'matches': 0,
            'header_bits': 0,
            'token_bits': 0,
            'total_bits': len(data) * 8,
        }
    
    def analyze(self):
        """Parse and analyze the bitstream."""
        print("=" * 70)
        print("FLUFF BITSTREAM ANALYSIS")
        print("=" * 70)
        
        # Parse header
        print("\n[HEADER]")
        if len(self.data) < 4:
            print("ERROR: File too small for header")
            return False
        
        magic = self.data[0:2]
        version = self.data[2]
        window_bits = self.data[3]
        
        print(f"  Magic:       {magic.hex().upper()} (should be 464C)")
        print(f"  Version:     0x{version:02X}")
        if version == 0x02:
            print(f"              ✓ Version 0x02 (RLE code-length support)")
        else:
            print(f"              ✗ Unexpected version (expected 0x02)")
        print(f"  Window bits: {window_bits} (2^{window_bits} = {1 << window_bits} bytes)")
        
        # Parse tree structure
        self.br.byte_pos = 4
        self.br.bits = 0
        self.br.nbits = 0
        self.br.pos = 32
        
        print("\n[HUFFMAN TREES]")
        tree_start = self.br.tell()
        
        try:
            self.analyze_trees()
            tree_bits = self.br.tell() - tree_start
            self.stats['header_bits'] = self.br.tell()
            
            print(f"\n  Tree structure size: {tree_bits} bits ({tree_bits // 8} bytes)")
            
            # Analyze tokens
            print("\n[TOKEN STREAM]")
            token_start = self.br.tell()
            self.analyze_tokens()
            token_bits = self.br.tell() - token_start
            self.stats['token_bits'] = token_bits
            
            print(f"\n  Token stream size: {token_bits} bits ({token_bits // 8} bytes)")
            
        except EOFError as e:
            print(f"ERROR: {e}")
            return False
        
        # Summary
        print("\n[SUMMARY]")
        print(f"  Total size:      {len(self.data)} bytes ({self.stats['total_bits']} bits)")
        print(f"  Header + trees:  {self.stats['header_bits']} bits ({100*self.stats['header_bits']/self.stats['total_bits']:.1f}%)")
        print(f"  Token stream:    {self.stats['token_bits']} bits ({100*self.stats['token_bits']/self.stats['total_bits']:.1f}%)")
        print(f"  Literals:        {self.stats['literals']} symbols")
        print(f"  Matches:         {self.stats['matches']} symbols")
        if self.stats['literals'] + self.stats['matches'] > 0:
            total = self.stats['literals'] + self.stats['matches']
            print(f"                   {100*self.stats['literals']/total:.1f}% literals, {100*self.stats['matches']/total:.1f}% matches")
        
        return True
    
    def analyze_trees(self):
        """Parse and display Huffman trees."""
        # Read tree header
        num_ll = self.br.get_bits(5) + 257
        num_d = self.br.get_bits(6) + 1
        hclen = self.br.get_bits(4) + 4
        
        print(f"  Literal/Length symbols: {num_ll}")
        print(f"  Distance symbols:       {num_d}")
        print(f"  Code-length symbols:    {hclen} (of {FLUFF_NUM_CODELEN})")
        
        # Read code-length tree
        cl_lens = [0] * FLUFF_NUM_CODELEN
        for i in range(hclen):
            idx = FLUFF_CODELEN_ORDER[i]
            cl_lens[idx] = self.br.get_bits(3)
        
        print(f"\n  Code-length code lengths:")
        for i in range(FLUFF_NUM_CODELEN):
            if cl_lens[i] > 0:
                print(f"    Symbol {i:2d}: {cl_lens[i]} bits")
        
        # Decode code lengths (with RLE support)
        print(f"\n  Decoding code lengths (with RLE)...")
        combined = []
        prev_len = 0
        cl_freq = defaultdict(int)
        rle_symbols = 0
        
        while len(combined) < num_ll + num_d:
            # Decode a symbol from code-length tree
            sym = self._decode_symbol(cl_lens)
            rle_symbols += 1
            
            if sym <= 15:
                combined.append(sym)
                prev_len = sym
                cl_freq[sym] += 1
            elif sym == 16:
                # Repeat previous length
                count = self.br.get_bits(2) + 3
                for _ in range(count):
                    combined.append(prev_len)
                    cl_freq[prev_len] += 1
                # print(f"    RLE: Repeat {prev_len} x{count}")
            elif sym == 17:
                # Emit 3-10 zeros
                count = self.br.get_bits(3) + 3
                for _ in range(count):
                    combined.append(0)
                # print(f"    RLE: Zero x{count}")
                prev_len = 0
            elif sym == 18:
                # Emit 11-138 zeros
                count = self.br.get_bits(7) + 11
                for _ in range(count):
                    combined.append(0)
                # print(f"    RLE: Zero x{count}")
                prev_len = 0
        
        print(f"  RLE symbols emitted: {rle_symbols} (to encode {num_ll + num_d} code lengths)")
        
        # Split back into litlen and dist
        ll_lens = combined[:num_ll]
        d_lens = combined[num_ll:]
        
        # Analyze code length distribution
        print(f"\n  Literal/Length code length distribution:")
        length_freq = defaultdict(int)
        for length in ll_lens:
            length_freq[length] += 1
        for length in sorted(length_freq.keys()):
            count = length_freq[length]
            pct = 100.0 * count / num_ll if num_ll > 0 else 0
            if length == 0:
                print(f"    {length:2d}-bit: {count:4d} codes ({pct:5.1f}%) - unused")
            else:
                print(f"    {length:2d}-bit: {count:4d} codes ({pct:5.1f}%)")
        
        self.ll_lens = ll_lens
        self.d_lens = d_lens
    
    def _decode_symbol(self, lens):
        """Decode a symbol from a code-length tree using proper Huffman decoding."""
        # Build canonical codes from code lengths
        # Count frequency of each code length
        bl_count = [0] * 16
        for length in lens:
            if 0 < length <= 15:
                bl_count[length] += 1
        
        # Build code table
        next_code = [0] * 16
        code = 0
        bl_count[0] = 0
        for bits in range(1, 16):
            code = (code + bl_count[bits - 1]) << 1
            next_code[bits] = code
        
        code_table = {}
        for sym in range(len(lens)):
            length = lens[sym]
            if length != 0:
                code_table[next_code[length]] = (sym, length)
                next_code[length] += 1
        
        # Decode by reading bits one at a time
        code_val = 0
        for bit_len in range(1, 16):
            bit = self.br.get_bit()
            code_val = (code_val << 1) | bit
            
            if code_val in code_table:
                sym, expected_len = code_table[code_val]
                if expected_len == bit_len:
                    return sym
        
        raise ValueError(f"No matching symbol in code-length tree after 15 bits")
    
    def _get_code(self, sym, lens):
        """Get canonical Huffman code for a symbol (now unused, kept for compatibility)."""
        # Build canonical code from code lengths
        code = 0
        for prev_sym in range(sym):
            if lens[prev_sym] == lens[sym]:
                code += 1
            elif lens[prev_sym] < lens[sym]:
                code = (code + 1) << (lens[sym] - lens[prev_sym])
        return code
    
    def analyze_tokens(self):
        """Parse and display token stream."""
        lit_freq = defaultdict(int)
        match_lengths = []
        distances = []
        
        token_count = 0
        while True:
            # Try to decode a symbol
            sym = self._decode_litlen_symbol()
            token_count += 1
            
            if sym == 256:  # End of block
                print(f"  End-of-block symbol (token #{token_count})")
                break
            elif sym < 256:  # Literal
                self.stats['literals'] += 1
                lit_freq[sym] += 1
            elif 257 <= sym <= 285:  # Match length
                self.stats['matches'] += 1
                match_len = self._decode_length(sym)
                match_lengths.append(match_len)
                # Now decode distance
                try:
                    dist_sym = self._decode_distance_symbol()
                    dist = self._decode_distance(dist_sym)
                    distances.append(dist)
                except:
                    print(f"    Error decoding distance after match")
                    break
        
        print(f"\n  Token count: {token_count}")
        print(f"  Literals: {self.stats['literals']}")
        print(f"  Matches: {self.stats['matches']}")
        
        self.stats['distances'] = distances
        self.stats['match_lengths'] = match_lengths

        if match_lengths:
            print(f"\n  Match lengths: min={min(match_lengths)}, max={max(match_lengths)}, avg={sum(match_lengths)/len(match_lengths):.1f}")
        if distances:
            print(f"  Distances: min={min(distances)}, max={max(distances)}, avg={sum(distances)/len(distances):.1f}")
        
        # Byte distribution
        if lit_freq:
            print(f"\n  Literal distribution:")
            most_common = sorted(lit_freq.items(), key=lambda x: x[1], reverse=True)[:5]
            for byte, count in most_common:
                pct = 100.0 * count / self.stats['literals']
                print(f"    0x{byte:02X}: {count:6d} ({pct:5.1f}%)")
    
    def _build_decode_table(self, lens):
        """Build a fast decode table from code lengths."""
        # Count frequency of each code length
        bl_count = [0] * 16
        for length in lens:
            if 0 < length <= 15:
                bl_count[length] += 1
        
        # Build code table
        next_code = [0] * 16
        code = 0
        bl_count[0] = 0
        for bits in range(1, 16):
            code = (code + bl_count[bits - 1]) << 1
            next_code[bits] = code
        
        code_table = {}
        for sym in range(len(lens)):
            length = lens[sym]
            if length != 0:
                code_table[next_code[length]] = (sym, length)
                next_code[length] += 1
        
        return code_table
    
    def _decode_from_table(self, code_table):
        """Decode a symbol using tablelookup with bit-by-bit reading."""
        code_val = 0
        for bit_len in range(1, 16):
            bit = self.br.get_bit()
            code_val = (code_val << 1) | bit
            
            if code_val in code_table:
                sym, expected_len = code_table[code_val]
                if expected_len == bit_len:
                    return sym
        
        raise EOFError(f"No symbol found in code table after 15 bits")
    
    def _decode_litlen_symbol(self):
        """Decode a literal/length symbol."""
        if not hasattr(self, '_ll_code_table'):
            self._ll_code_table = self._build_decode_table(self.ll_lens)
        return self._decode_from_table(self._ll_code_table)
    
    def _decode_distance_symbol(self):
        """Decode a distance symbol."""
        if not hasattr(self, '_d_code_table'):
            self._d_code_table = self._build_decode_table(self.d_lens)
        return self._decode_from_table(self._d_code_table)    
    def _decode_length(self, sym):
        """Convert length symbol to actual length."""
        if sym < 257 or sym > 285:
            return 0
        idx = sym - 257
        if idx >= len(LEN_BASE):
            return 258
        base = LEN_BASE[idx]
        extra_bits = LEN_EXTRA[idx]
        if extra_bits > 0:
            extra = self.br.get_bits(extra_bits)
            return base + extra
        return base
    
    def _decode_distance(self, sym):
        """Convert distance symbol to actual distance."""
        if sym >= len(DIST_BASE):
            return 1
        base = DIST_BASE[sym]
        extra_bits = DIST_EXTRA[sym]
        if extra_bits > 0:
            extra = self.br.get_bits(extra_bits)
            return base + extra
        return base


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('--help', '-h', 'help'):
        print("Usage: analyze_bitstream.py <fluff_block_file>")
        print("\nAnalyze fluff bitstream structure and display breakdown:")
        print("  - Header (magic, version, window size)")
        print("  - Huffman tree structure (code lengths, RLE symbols)")
        print("  - Token stream (literals, matches, distances)")
        print("  - Bit allocation summary")
        print("\nExample:")
        print("  python3 extract_fluff_block.py file.bolt raw/block.raw")
        print("  python3 analyze_bitstream.py raw/block.raw")
        sys.exit(0 if '--help' in sys.argv else 1)
    
    filepath = sys.argv[1]
    with open(filepath, 'rb') as f:
        data = f.read()
    
    analyzer = Analyzer(data)
    success = analyzer.analyze()
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
