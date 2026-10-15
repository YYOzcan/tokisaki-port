#!/usr/bin/env python3
import os
import sys
import struct

def extract_spu_elfs(eboot_path, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    with open(eboot_path, "rb") as f:
        data = f.read()

    idx = 0
    pos = 0
    extracted = []

    while True:
        pos = data.find(b"\x7fELF", pos)
        if pos == -1:
            break

        if pos + 52 > len(data):
            break

        # SPU is 32-bit big-endian ELF (EI_CLASS=1, EI_DATA=2)
        ei_class = data[pos + 4]
        ei_data = data[pos + 5]

        endian = ">" if ei_data == 2 else "<"

        if ei_class == 1: # 32-bit (SPU)
            e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(
                f"{endian}HHIIIIIHHHHHH", data, pos + 16
            )
            if e_machine == 0x0017: # SPU
                # Calculate total ELF size
                total_size = e_ehsize
                if e_phnum > 0 and e_phoff > 0:
                    total_size = max(total_size, e_phoff + (e_phnum * e_phentsize))
                if e_shnum > 0 and e_shoff > 0:
                    total_size = max(total_size, e_shoff + (e_shnum * e_shentsize))

                # Also inspect sections/segments to ensure full payload is captured
                for p in range(e_phnum):
                    ph_offset = pos + e_phoff + (p * e_phentsize)
                    p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from(f"{endian}IIII", data, ph_offset + 4)
                    total_size = max(total_size, p_offset + p_filesz)

                spu_data = data[pos:pos+total_size]
                out_name = f"spu_job_{idx:02d}_0x{pos:08X}.elf"
                out_path = os.path.join(output_dir, out_name)
                with open(out_path, "wb") as out_f:
                    out_f.write(spu_data)

                print(f"[+] Extracted SPU ELF #{idx:02d} at offset 0x{pos:08X} (size: {total_size} bytes, entry: 0x{e_entry:08X}) -> {out_name}")
                extracted.append(out_path)
                idx += 1
                pos += total_size
                continue

        pos += 4

    print(f"\n[+] Total {len(extracted)} SPU ELFs extracted into: {output_dir}")
    return extracted

if __name__ == "__main__":
    eboot = sys.argv[1] if len(sys.argv) > 1 else "/home/yigit/Kurumi Tokisaki/game_dump/PS3_GAME/USRDIR/EBOOT.ELF"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "/home/yigit/Kurumi Tokisaki/game_dump/spu_modules"
    extract_spu_elfs(eboot, out_dir)
