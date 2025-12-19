#!/usr/bin/env python3
"""
Generate embedded FIX dictionary as a byte array.

This script converts the FIX dictionary XML into a C++ byte array to avoid
MSVC's 16KB string literal limitation while remaining cross-platform compatible.
"""

import sys
import os

def read_dictionary_xml(xml_path):
    """Read the XML content from file."""
    with open(xml_path, 'r', encoding='utf-8') as f:
        xml_content = f.read()
    return xml_content

def generate_byte_array_cpp(xml_content, output_path):
    """Generate C++ source file with byte array."""
    
    # Convert string to bytes
    xml_bytes = xml_content.encode('utf-8')
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('// Auto-generated file - DO NOT EDIT\n')
        f.write('// Generated from FIX 4.4 dictionary XML\n')
        f.write('// This file is generated at build time by scripts/generate_embedded_dictionary.py\n\n')
        f.write('#include "dictionary/embedded_fix44_dictionary.hpp"\n')
        f.write('#include <string>\n\n')
        f.write('namespace duckdb {\n\n')
        f.write('// Embedded FIX 4.4 dictionary as byte array\n')
        f.write('static const unsigned char embedded_fix44_dict_data[] = {\n')
        
        # Write bytes in groups of 12 per line
        bytes_per_line = 12
        for i in range(0, len(xml_bytes), bytes_per_line):
            chunk = xml_bytes[i:i+bytes_per_line]
            hex_values = ', '.join(f'0x{b:02x}' for b in chunk)
            f.write(f'    {hex_values}')
            if i + bytes_per_line < len(xml_bytes):
                f.write(',\n')
            else:
                f.write('\n')
        
        f.write('};\n\n')
        f.write(f'static const size_t embedded_fix44_dict_size = {len(xml_bytes)};\n\n')
        
        # Generate accessor function
        f.write('std::string GetEmbeddedFix44Dictionary() {\n')
        f.write('    return std::string(\n')
        f.write('        reinterpret_cast<const char*>(embedded_fix44_dict_data),\n')
        f.write('        embedded_fix44_dict_size\n')
        f.write('    );\n')
        f.write('}\n\n')
        f.write('} // namespace duckdb\n')

def main():
    if len(sys.argv) != 3:
        print("Usage: generate_embedded_dictionary.py <input_header> <output_cpp>")
        sys.exit(1)
    
    input_header = sys.argv[1]
    output_cpp = sys.argv[2]
    
    if not os.path.exists(input_header):
        print(f"Error: Input file not found: {input_header}")
        sys.exit(1)
    
    print(f"Reading dictionary from: {input_header}")
    xml_content = read_dictionary_xml(input_header)
    print(f"Dictionary size: {len(xml_content)} characters")
    
    print(f"Generating byte array in: {output_cpp}")
    generate_byte_array_cpp(xml_content, output_cpp)
    print("Done!")

if __name__ == '__main__':
    main()
