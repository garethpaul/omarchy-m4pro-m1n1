# J614s / T6040 preflight candidate

Read-only macOS inventory identified a 14-inch M4 Pro as `Mac16,8`, board ID
4, chip ID `0x6040`, with 12 CPU cores and 24 GiB RAM. The installed macOS
version was 26.6.2, build 25G83. Its memory-controller device reports
`mcc,t6041`, `amcc-reg-idx = 12`, `amcc-plane-enable-mask = 0x0f` and
`dcs-channel-enable-mask = 0xffffffff`.

The firmware-cache initializer now permits board 4 alongside board 6. Board
4 must still pass every existing J616s check: four MCC instances, four
channels and planes, the expected stride, exact register bases and aperture
sizes, and all sixteen firmware cache-status reads. The path continues to
leave cache-control and TZ programming disabled. Other board IDs and M4 Max
remain rejected. The J616s CPU-frequency admission is unchanged.

The host harness runs the successful layout, each bad status, and rejected
register, aperture, count, channel, plane and stride cases for both boards.
It checks that rejected layouts cause no status reads and that successful
initialization does not enable cache-control or TZ programming:

```sh
python3 -m pytest tests/python/test_t6040_mcc.py -q
```

## J614s hardware checkpoint

The candidate at source commit `8e0cf28`, built with `CHAINLOADING=1` and
`T6040_STAGE1_CDC=1`, was installed as a custom boot object and booted on this
J614s on 2026-10-19. The transfer binary is identified by the checksum file
in the `j614s-preflight-8e0cf28` release; later documentation commits do not
change the identity of the installed image.

The real machine enumerated the candidate's high-speed USB CDC device.
Both the transport NOP and proxy NOP passed. Two full reads of the 606208-byte
Apple device tree were identical, and the proxy reported no new exceptions.
Raw boot arguments and device-tree contents remain private.

Cached startup metadata identified boot CPU 4 with architectural affinity
`0x10100`. Its AIC3 reports 2720 implemented interrupts, 4096 maximum
interrupts, and the register layout expected by the existing AIC3 driver.
The bootloader inherited the internal 3024-by-1964, 30-bit framebuffer;
external HDMI did not provide video during this proxy session.

This establishes a working m1n1 debug proxy on the tested J614s. It does not
qualify a Linux boot, persistent installation, graphics, storage, input,
sustained USB operation, CPU-frequency changes or any other J614s hardware.
No Linux kernel or Omarchy desktop has run on this machine at this checkpoint.
The [J616s bring-up record](j616s-bringup.md) describes separate experiments
on the 16-inch machine; those results do not qualify J614s.
