# GLaBIOS on Pico-386

This optional system ROM uses the upstream 8088 emulator preset. POST shows
386, 387 and VGA for the emulated machine. The system ROM keeps its CGA switch
setting for POST. The native Pico-386 video BIOS provides INT 10h and
identifies the guest adapter as VGA. POST uses 640 KB of base RAM, sets up the Disney and Covox ports, and
boots the disk through INT 13h without a physical floppy controller.

## Build the trial ROM

In a checkout of [GLaBIOS v0.4.2](https://github.com/640-KB/GLaBIOS/tree/v0.4.2)
at commit `b98b0fe396466fa41466a9521f40987c989ae077`, run:

```sh
git apply /path/to/pico-386/tools/glabios/pico386.patch
jwasm -q -Zm -Zg -bin -DARCH_TYPE=ARCH_EMU -Fo=profile.bin src/GLABIOS.ASM
python src/gla2rom/gla2rom.py profile.bin profile.rom
```

This was tested with JWasm 2.20. The tagged source has the internal version
string 0.4.1. To try it, use a separate Pico-386 checkout, place `profile.rom`
at its root, and run the following in Python before the normal host or M2P2
build. The default Pico-386 checkout uses PCXTBIOS.

```python
from pathlib import Path

rom = Path("profile.rom").read_bytes()
assert len(rom) == 8192 and sum(rom) % 256 == 0
header = "const unsigned char BIOS[] = {" + ",".join(map(str, rom)) + "};\n"
Path("src/emulator/includes/bios.h").write_text(header + "unsigned int pcxtbios_bin_len = 8192;\n")
```

GLaBIOS is copyright 640KB and contributors and uses the [GNU GPLv3](https://github.com/640-KB/GLaBIOS/blob/v0.4.2/LICENSE).
