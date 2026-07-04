# SETool
A work in progress EDL tool for exynos chips (S21 and later)
For older models use https://github.com/halal-beef/hubble

To use this tool you will need some files. These files can be found in samsung's BL.tar firmwares.
Some of the files need to be extracted from sboot.bin.
Use this tool to find the offsets: https://github.com/halal-beef/sboot-split-detector
Then use linux's dd to extract files from these offsets.

Example:
if the tool says:

```bash
BL1:    0x0      - 0x3000
EPBL:   0x3000   - 0x16000
BL2:    0x16000  - 0xEC000
LK:     0xEC000  - 0x36C000
EL3_MON:0x36C000 - 0x3AC000
```

you do:

```bash
dd if=sboot.bin of=../sources/bl1.img  bs=1 count=$((0x3000))
dd if=sboot.bin of=../sources/pbl.img  bs=1 skip=$((0x3000))  count=$((0x13000))
dd if=sboot.bin of=../sources/bl2.img  bs=1 skip=$((0x16000)) count=$((0xD6000))
dd if=sboot.bin of=../sources/abl.img  bs=1 skip=$((0xEC000)) count=$((0x280000))
dd if=sboot.bin of=../sources/bl31.img bs=1 skip=$((0x36C000)) count=$((0x40000))
```
