# DOS file redirector

This is a local filesystem redirector called through DOS INT 2Fh, AX=11xx.
It does not use an IP network. The host and RP2350 builds have separate code:
`src/emulator/network-redirector.c.inl` and
`src/emulator/network-redirector-rp2350.c.inl`.

## Filesystem roots

| Build | Path base used by the handler |
|---|---|
| Windows host | `C:\FASM` |
| Linux host | `/tmp/` |
| RP2350 | Root of the mounted FatFs volume, written as `\\` in the source |

The handler builds paths from these bases. This describes the current path
construction; it is not a verified filesystem-confinement or security boundary.
On RP2350, a guest path such as `H:\XT\file` is combined with the volume-root
base by `get_full_path()`.

## Map the DOS drive

The tracked [MAPDRIVE.COM](tools/mapdrive.com) utility maps the redirector to
DOS drive `H:`. Its source is [tools/mapdrive.asm](tools/mapdrive.asm). The
drive letter is fixed in the program. Copy `MAPDRIVE.COM` to a DOS-accessible
directory, then run `MAPDRIVE` at the DOS prompt. The utility checks that DOS
has `LASTDRIVE` set to `H` or a later letter. Add `LASTDRIVE=H` (or a later
letter) to `CONFIG.SYS` and reboot DOS before running it.

## Implemented subfunctions

Both backends handle these AX values:

| AX | Operation |
|---|---|
| `1100h` | Installation check |
| `1101h`, `1103h`, `1105h` | Remove directory, create directory, change directory |
| `1106h`, `1107h` | Close or commit a file handle |
| `1108h`, `1109h` | Read or write a file |
| `110Ah` | Lock or unlock a file region; stub that reports success |
| `110Ch` | Disk information; placeholder values, not live free-space data |
| `110Eh` | Set attributes; stub that reports success |
| `110Fh` | Get file attributes and size |
| `1111h`, `1113h` | Rename or delete a file |
| `1116h`, `1117h` | Open, or create/truncate, a file |
| `111Bh`, `111Ch` | Find first or next file |
| `1120h`, `1121h` | Flush open files, or seek from end of file |

Each backend has up to 32 open file handles. Error conversion differs by
backend: the host uses C library errors, while RP2350 uses FatFs results.
Unsupported subfunctions are not handled by this redirector.

## Limits

- Locking and setting attributes do not change the underlying files.
- Disk information is a stub and must not be used as the real volume size or
  free-space count.
- The handler's SFT and search-data layouts are implementation details. Check
  the backend source before relying on a field offset or an unlisted DOS call.

See the two source files above for exact register inputs, results, path rules,
and per-platform behavior.
