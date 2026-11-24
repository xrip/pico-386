# Emulated I/O ports

This is a map of the port cases in `src/emulator/ports.c`, not a full hardware
specification. Many devices are partial emulations. An unhandled read returns
`0xFF`; an unhandled write has no effect.

## Main port handler

| Port(s) | Write behavior | Read behavior |
|---|---|---|
| `0000h–000Fh` | 8237 DMA controller | 8237 DMA controller |
| `0020h–0021h` | 8259 PIC | 8259 PIC |
| `0040h–0043h` | 8253 PIT | `0040h–0042h` only |
| `0060h` | No handler | Keyboard data; reading releases the queued key |
| `0061h` | PC speaker control byte | Last written byte |
| `0064h` | Keyboard command on RP2350 | Keyboard status byte |
| `0081h`, `0082h`, `0083h`, `0087h` | DMA page registers | DMA page registers |
| `0092h` | Set A20 from bit 0 | A20 state |
| `00C0h–00C7h`, `01E0h`, `02C0h` | Tandy SN76489 sound writes | Not handled |
| `0201h` | Reset joystick timing | Joystick buttons and axes |
| `0220h–0223h` | CMS write, then also reaches the Sound Blaster write handler | Sound Blaster handler |
| `0224h–0227h`, `022Ah–022Fh` | Sound Blaster handler | Sound Blaster handler |
| `0228h–0229h`, `0388h–0389h` | AdLib/OPL2 register write | AdLib/OPL2 status |
| `0240h–0257h` | Not handled | BCD date/time fields from the host clock; year is fixed to 24 |
| `0278h` | Covox sample | Not handled |
| `027Ah` | Not handled | Returns zero |
| `0330h–0331h` | MPU-401 MIDI | MPU-401 MIDI |
| `0378h`, `037Ah` | Disney Sound Source | `0378h–0379h` read status/data; `037Ah` returns zero |
| `03B0h–03B7h`, `03D0h–03D7h` | MC6845 index/data registers | Only `03D4h` (index) and `03D5h` (data) |
| `03B8h`, `03BFh`, `03D8h`, `03D9h` | Hercules or CGA control | Not handled here |
| `03C0h–03C9h`, `03CEh–03CFh` | VGA register handler for selected ports | VGA register handler for selected ports |
| `03DAh` | TGA register index | CGA status; also resets the VGA attribute flip-flop |
| `03DEh–03DFh` | TGA data and page registers | Not handled |
| `03F8h–03FFh` | Serial mouse | Serial mouse |
| `0260h–0263h` | Not handled | Not handled; reads return `0xFF` |

Writes to `0220h–0223h` currently call `cms_out()` and then fall through to
`blaster_write()`. Preserve this behavior only if it is intended; the source
does not mark the fall-through as an explicit design choice.

## Video port handlers

- `src/emulator/video/cga.c` handles Hercules mode control at `03B8h` and
  enable at `03BFh`, plus CGA mode/color writes at `03D8h` and `03D9h`.
- `src/emulator/video/tga.c` handles writes at `03DAh`, `03DEh`, and `03DFh`.
- `src/emulator/video/vga.c` handles selected attribute, sequencer, DAC, and
  graphics-controller ports in the `03C0h–03CFh` range. The main handler decides
  which ports reach it.

For exact device behavior, read the handlers named above. `portin16()` and
`portout16()` perform two adjacent byte-port operations.
