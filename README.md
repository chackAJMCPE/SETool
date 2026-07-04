# SETool (short for Samsung EDL Tool)
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

Then you need to boot into EUB/EDL

To do that, there are 2 ways:

Corrupt the bootloader

Or

Short 2 test points (that's how i did it)


While shorting the test points you plug in the phone to your computer WITH the battery plugged in.

Dont know where to short? These guys have a lot of images for you: https://chimeratool.com/en/test-points

# Why only s21 and later??
The EUB before s21 is documented and the tools do exist already.
When Samsung removed their Moongoose cores, they also reworked the bootrom, especially EUB.
A very similiar EUB to the one used in the s21 happens to be in google's first tensor SOC.
(If you didn't know, the first tensor SOC was based on the exynos 2100/9840 which was used in the s21)

There are plans to later add that old EUB to this tool, but the focus right now is the new EUB.
