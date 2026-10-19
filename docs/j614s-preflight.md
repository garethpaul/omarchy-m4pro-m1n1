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

This is an unproven bootloader candidate. No m1n1 boot, live MCC status read,
Linux boot, USB proxy session or persistent installation has been run on
J614s. macOS registry metadata does not establish that the J616s runtime
profile works on this board. The retained checks may reject it. The
[J616s bring-up record](j616s-bringup.md) describes separate experiments on
the 16-inch machine; those results do not qualify J614s.
