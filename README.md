# What is this?
This tool fixes the broken timestamps due to an issue in Fedora ffmpeg-free package as well as Cygwin ffmpeg package.

When you manupulate H.264 Video files, Fedora's ffmpeg package occasionally outputs:
``` log
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 66 PTS: 33, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 198 PTS: 167, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 330 PTS: 300, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 462 PTS: 433, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 594 PTS: 567, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 726 PTS: 700, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 858 PTS: 833, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 990 PTS: 967, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 1122 PTS: 1100, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 1254 PTS: 1233, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 1452 PTS: 1433, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Invalid DTS: 1584 PTS: 1567, replacing by guess
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 1733, current: 1700; changing to 1733. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 1867, current: 1833; changing to 1867. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2000, current: 1967; changing to 2000. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2133, current: 2100; changing to 2133. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2267, current: 2233; changing to 2267. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2400, current: 2367; changing to 2400. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2533, current: 2500; changing to 2533. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2667, current: 2633; changing to 2667. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2800, current: 2767; changing to 2800. This may result in incorrect timestamps in the output file.
[vost#0:0/copy @ 0xa00061e80] Non-monotonic DTS; previous: 2933, current: 2900; changing to 2933. This may result in incorrect timestamps in the output file.
```

If these warning messages are shown, the timestamps (PTS, DTS) of the output file is possiblly broken.

```
Usage: fix_ts broken.mkv output.mkv [m/n]
       m/n (optional): frame_rate (e.g. 30000/1001: 30fps in NTSC)
```

## Build requires
libavformat-devel, libavcodec-devel, and libavutil-devel

## How to build
Simply run `make`.
