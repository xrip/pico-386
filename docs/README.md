# 386 reference documents and test data

The Intel manual, Intel386 SX data sheet, and XMS 3.0 text used during the CPU
migration are tracked reference files. The source links below identify where
they came from.

| Local path | Reference | Source |
|---|---|---|
| `docs/intel/i386-prm-1986.txt` | Intel 80386 Programmer's Reference Manual, 1986, order 230985 | `386INTEL.TXT` in [386INTEL.ZIP](http://cd.textfiles.com/freedom/CODE/TEXT/386INTEL.ZIP) |
| `docs/intel/240187-008_386SX_datasheet.pdf` | Intel386 SX data sheet, 1995, order 240187-008 | [PDF](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/Intel386%20SX.pdf) |
| `docs/intel/240187-008_386SX_datasheet.txt` | Text extracted from the data sheet PDF | Made from the PDF with `pypdf` |
| `docs/specs/xms30.txt` | eXtended Memory Specification 3.0, January 1991 | [Text copy](https://www.phatcode.net/res/219/files/xms30.txt) |

## Where to look in the Intel manual

The line numbers below were checked against the tracked text file.

| Line | Part | Topic |
|---:|---|---|
| 4723 | Chapter 5, Memory Management | Descriptors, segment loads, paging, page tables |
| 5434 | Chapter 6, Protection | Privilege levels and call gates |
| 6653 | Chapter 7, Multitasking | TSS layout and task switch |
| 7707 | Chapter 9, Exceptions and Interrupts | IDT gates and error codes |
| 8839 | Chapter 10, Initialization | Reset state and protected-mode transitions |
| 10569 | Chapter 14, Real-Address Mode | Real-mode rules on a 386 |
| 11066 | Chapter 15, Virtual 8086 Mode | V86 mode and IOPL-sensitive instructions |
| 11759 | Chapter 16, Mixing 16-Bit and 32-Bit Code | Operand and address size |
| 12202 | Chapter 17, Instruction Set | Instruction definitions |
| 20322 | Appendix A, Opcode Map | One- and two-byte opcode tables |

The migration notes in [386-migration.md](../386-migration.md) record which
manual sections informed the implementation. They include old source line
references and test results; check the current source before relying on them.

## Test data

Test data is not tracked. The runners take file paths as arguments.

- [SingleStepTests 80286](https://github.com/SingleStepTests/80286) and
  [80386](https://github.com/SingleStepTests/80386) provide real-mode MOO files.
  The files in `v1_real_mode` are gzip-compressed. For example,
  `gh api repos/SingleStepTests/80286/contents/v1_real_mode --jq '.[].download_url'`
  lists download URLs. Download the files and unpack them with `gzip -d *.MOO.gz`.
- [test386.asm](https://github.com/barotto/test386.asm) is an external ROM
  source. Build a local copy with NASM, then pass the binary to a runner built
  from the tracked `tests/test386_test.c` source.

For the ROM steps that match the current emulator limits, the test source copy
needs these edits before assembly:

1. With NASM 3, change `mov [x], word y` to `mov word [x], y` (and the same
   form for byte and dword operands).
2. Disable test 12's segment-write and LOCK checks, plus the ENTER page-fault
   check. These cases are listed as unsupported in `386-migration.md`.
3. Assemble with `nasm -isrc/ -f bin src/test386.asm -l test386.lst -o test386.bin`.
   Set `ROM128 equ 1` in `src/configuration.asm` for the 128 KB ROM, which adds
   the task-switch tests. Keep the source and build outputs outside this repo.
