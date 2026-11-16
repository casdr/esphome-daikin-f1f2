# Bus logs

Put your own F1F2 bus logs here (`*.tsv`). They are git-ignored. `make -C tests decoder`
runs the C++ decoder and the Python reference (`tools/f1f2_decode.py`) over every log here
and fails on any difference.

One frame per line, tab-separated, as written by `tools/f1f2_log.py`:

```
<unix time>	<gap before the frame, s>	<frame as hex>	[# note]
1794783600.237	0.453	E900FF1C23E06D67C018...
```

The frame is the hex after `R T <gap>:` in the bridge's raw output (`P1P2/R/...` on MQTT),
including the checksum byte.
