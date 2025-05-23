#!/usr/bin/env python3

#
# Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
# Copyright (C) 2024 Mark Thompson
# Copyleft (ɔ) 2024 zxkmm with the GPL license
#
# This file is part of PortaPack.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

import sys
import os
from external_app_info import maximum_application_size
from external_app_info import external_apps_address_start
from external_app_info import external_apps_address_end
import subprocess

import re
from pathlib import Path

usage_message = """
PortaPack SPI flash image generator

Usage: <command> <application_path> <baseband_path> <output_path>
       Where paths refer to the .bin files for each component project.
"""


def read_image(path):
    f = open(path, 'rb')
    data = f.read()
    f.close()
    return data


def write_image(data, path):
    f = open(path, 'wb')
    f.write(data)
    f.close()

########external app linker script address check########

def parse_memory_regions(ld_file_path):
    regions = []
    
    with open(ld_file_path, 'r') as f:
        content = f.read()
        
    memory_section = re.search(r'MEMORY\s*\{(.*?)\}', content, re.DOTALL)
    if not memory_section:
        print("ERROR: Could not find MEMORY section in the linker script.")
        return []
    
    memory_content = memory_section.group(1)
    
    pattern = r'ram_external_app_(\w+)\s*\(rwx\)\s*:\s*org\s*=\s*(0x[A-Fa-f0-9]+),\s*len\s*=\s*(\d+)k'
    matches = re.finditer(pattern, memory_content)
    
    for match in matches:
        app_name = match.group(1)
        address = int(match.group(2), 16)  # string with hex -> int
        length = int(match.group(3)) * 1024  # kb -> bytes
        
        regions.append({
            'app_name': app_name,
            'address': address,
            'length': length
        })
        
    return sorted(regions, key=lambda x: x['address'])

def validate_memory_regions(regions):
    if not regions:
        return False
    
    expected_step = 0x10000  # 64k as step (not sure why)
    expected_base = 0xADB10000 # the start (not sure why this one)
    expected_size = 32 * 1024  # 32k
    issues_found = False
    
    print("\n")
    print(f"checking {len(regions)} external apps address memory regions")
    
    if regions[0]['address'] != expected_base:
        print(f"WARNING: external app first region should start at {hex(expected_base)}, but starts at {hex(regions[0]['address'])}")
        issues_found = True
    
    for i, region in enumerate(regions):
        expected_address = expected_base + (i * expected_step)
        
        # address count
        if region['address'] != expected_address:
            print(f"WARNING: external app region '{region['app_name']}' has incorrect address")
            print(f"want: {hex(expected_address)}, Found: {hex(region['address'])}")
            issues_found = True
        
        # size
        if region['length'] != expected_size:
            print(f"WARNING: external app region '{region['app_name']}' has incorrect size")
            print(f"want: {expected_size//1024}KB, Found: {region['length']//1024}KB")
            issues_found = True
        
        # overlap
        if i < len(regions) - 1:
            next_region = regions[i + 1]
            if region['address'] + region['length'] > next_region['address']:
                print(f"WARNING: external app region '{region['app_name']}' overlapped with '{next_region['app_name']}'")
                issues_found = True
    
    return not issues_found

#^^^^^^^^external app linker script address check^^^^^^^^

########gcc version check from elf file########

def get_gcc_version_from_elf(elf_file):
    succeed = False

    output = subprocess.check_output(['readelf', '-p', '.comment', elf_file])
    output = output.decode('utf-8')
    lines = output.split('\n')

    for line in lines:
        if 'GCC:' in line:
            version_info = line.split('GCC:')[1].strip()
            succeed = True
            return version_info

    if not succeed:  # didn't use try except here cuz don't need to break compile if this is bad result anyway
        return None


def get_gcc_version_from_elf_files_in_giving_path_or_filename_s_path(path):
    elf_files = []
    if os.path.isdir(path):
        elf_files = [os.path.join(path, f) for f in os.listdir(path) if f.endswith(".elf")]
    elif os.path.isfile(path):
        elf_files = [os.path.join(os.path.dirname(path), f) for f in os.listdir(os.path.dirname(path)) if
                     f.endswith(".elf")]
    else:
        print(
            "gave path or filename is not valid")  # didn't use except nor exit here cuz don't need to break compile if this is bad result anyway

    gcc_versions = []
    for elf_file in elf_files:
        version_info = get_gcc_version_from_elf(elf_file)
        if version_info is not None:
            extract_elf_file_name = os.path.basename(elf_file)
            gcc_versions.append("gcc version of " + extract_elf_file_name + " is " + version_info)
    return gcc_versions

#^^^^^^^^gcc version check from elf file^^^^^^^^

if len(sys.argv) != 4:
    print(usage_message)
    sys.exit(-1)

application_image = read_image(sys.argv[1])
baseband_image = read_image(sys.argv[2])
output_path = sys.argv[3]

print("\ncheck gcc versions from all elf target\n")
application_gcc_versions = get_gcc_version_from_elf_files_in_giving_path_or_filename_s_path(sys.argv[1])
baseband_gcc_versions = get_gcc_version_from_elf_files_in_giving_path_or_filename_s_path(sys.argv[2])

for itap in application_gcc_versions:
    print(itap)

for itbb in baseband_gcc_versions:
    print(itbb)

print("\n")

########external app linker script address check worker########

ld_file_path = Path("..") / ".." / "firmware" / "application" / "external" / "external.ld"

try:
    regions = parse_memory_regions(ld_file_path)
    
    if not regions:
        print("some issue causing that we can't see external app's linker script's address list, pass")
    
    if validate_memory_regions(regions):
        print("external app addr seems correct, pass")
    else:
        print("\nWARNING: It seems you are having incorrect external app addresses.")
    
except Exception as e:
    print(f"err: {e}")

#^^^^^^^^external app linker script address check worker^^^^^^^^


spi_size = 1048576

images = (
    {
        'name': 'application',
        'data': application_image,
        'size': len(application_image),
    },
    {
        'name': 'baseband',
        'data': baseband_image,
        'size': len(baseband_image),
    },
)

spi_image = bytearray()
spi_image_default_byte = bytearray((255,))


#########check if the fw size ok and check external addr leak#########
for image in images:
    if len(image['data']) > image['size']:
        raise RuntimeError(
            'data for image "%(name)s" is longer than 0x%(size)x bytes 0x%(sz)x' % {'name': image['name'],
                                                                                    'size': image['size'],
                                                                                    'sz': len(image['data'])})
    pad_size = image['size'] - len(image['data'])
    padded_data = image['data'] + (spi_image_default_byte * pad_size)
    spi_image += padded_data

if len(spi_image) > spi_size - 4:
    raise RuntimeError('SPI flash image size of %d exceeds device size of %d bytes' % (len(spi_image) + 4, spi_size))

pad_size = spi_size - 4 - len(spi_image)
for i in range(pad_size):
    spi_image += spi_image_default_byte

# quick "add up the words" checksum, and check for possible references to code in external apps
checksum = 0
for i in range(0, len(spi_image), 4):
    snippet = spi_image[i:i + 4]
    val = int.from_bytes(snippet, byteorder='little')
    checksum += val
    if (val >= external_apps_address_start) and (val < external_apps_address_end) and (
            (val & 0xFFFF) < maximum_application_size):
        print("WARNING: Possible external code address", hex(val), "at offset", hex(i), "in", sys.argv[3])

final_checksum = 0
checksum = (final_checksum - checksum) & 0xFFFFFFFF

spi_image += checksum.to_bytes(4, 'little')

write_image(spi_image, output_path)

percent_remaining = round(1000 * pad_size / spi_size) / 10;
print("Space remaining in flash ROM:", pad_size, "bytes (", percent_remaining, "%)")


#^^^^^^^^check if the fw size ok and check external addr leak^^^^^^^^

# copy the fast flash script
build_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), 'build')
flash_py_path = os.path.join(build_dir, 'flash.py')

if not os.path.exists(flash_py_path):

    current_dir = os.path.dirname(__file__)
    source_file = os.path.join(current_dir, 'fast_flash_pp_and_copy_apps.py')
    
    if not os.path.exists(build_dir):
        os.makedirs(build_dir)
        
    # cp
    import shutil
    print(f"\ncopy {source_file} to {flash_py_path}\n")
    shutil.copy2(source_file, flash_py_path)


