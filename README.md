# tsfix

A C program that reconstructs H.264 / HEVC PTS/DTS from the POC (display
order) in the bitstream.
(Generalised from `mkvtsfix` and renamed, since it is no longer MKV specific.)

It covers two uses with the same mechanism.

1. **Repairing broken timestamps** — because of an ffmpeg bug
   ([RHBZ #2483137](https://bugzilla.redhat.com/show_bug.cgi?id=2483137)), a
   build with the H.264 / HEVC decoders disabled (`ffmpeg-free` and friends)
   cannot set `has_b_frames` correctly in `libavformat/demux.c` and therefore
   fails to reorder B frames.  Running `-c copy` in that state overwrites the
   output PTS with the value of the preceding frame, so the timestamps end up
   duplicated and the display order is lost.
2. **Adding timestamps to a raw elementary stream** — a bare Annex B stream
   carries no timestamps at all, so they have to be computed when putting it
   into a container.

Both come down to the same thing: **once the display order is known, the
timestamps can be computed**.  H.264 / HEVC bitstreams carry the POC (Picture
Order Count), which *is* the display order, so it is enough to read the POC
with a parser - no decoding required.

## Scope

| | |
| --- | --- |
| Input container | anything libavformat can open (mkv, mp4, ts, flv, raw ES, ...) |
| Output container | taken from the output file name (`--format` to be explicit) |
| Codecs | H.264 / HEVC |
| Requirements | the libavcodec **parser** (no decoder needed, so the very ffmpeg that broke the file can repair it) |

Length prefixed input (avcC / hvcC) gets `h264_mp4toannexb` /
`hevc_mp4toannexb` inserted automatically.  Input that is already Annex B,
such as a raw ES, is passed through unchanged.

## Building and running

```bash
gcc -std=c17 -O2 -Wall -Wextra -o tsfix tsfix.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil)
```

```bash
tsfix -a broken.mkv                    # analyse only (exit code 1 if anything is broken)
tsfix broken.mkv fixed.mkv             # repair a broken MKV
tsfix broken.mp4 fixed.mp4             # the same for MP4
tsfix -f 24000/1001 raw.h264 out.mkv   # add timestamps to a raw H.264 stream
tsfix -f 30000/1001 raw.hevc out.mp4   # raw HEVC into MP4
```

`--fps` is **mandatory for a raw stream**.  For a raw ES libavformat picks
`r_frame_rate` up from the VUI in the bitstream, which can come out as the
field rate (48000/1001, say), so it cannot be trusted; without the option the
program stops with an error.  For container input the value from the stream is
used when the option is omitted.

Other options: `--format NAME`, `--avoid-negative-ts MODE`, `-v`, `-h`.

`-a` exits 1 when anything was found broken and 0 otherwise, so it can be used
as a check in a script.

```
$ tsfix -a xx.mkv
== Analysis ==
Input container      : matroska,webm
Codec                : h264
Video frames         : 35510
Frame rate           : 24000/1001 (23.976 fps) [from stream]
Computation timebase : 1/1000
POC increment        : 2
Coded sequences      : 327
Reorder delay        : 2 frames
Anchor PTS (disp 0)  : 0
PTS missing          : 0 frames
PTS broken           : 11388 frames (32.1%)
Last PTS after fix   : 1481021 (1481.021 s, display 35509)
First DTS            : -83 (-0.083 s)

$ tsfix xx.mkv fixed.mkv
== Analysis ==
...
== Writing ==
Output container     : matroska
Output timebase      : 1/1000 (video)
Wrote                : fixed.mkv
  video: PTS/DTS reconstructed for 35510 frames, 69427 other packets untouched
```

## How it works

- **Pass 1 (analysis)** — run every video packet through the parser and
  collect the POC, the picture type and the IDR positions.  An IDR resets the
  POC to 0, so that is where a sequence boundary is, and within a sequence the
  rank in POC order becomes the display order number.  The POC increment (2
  for frame coded H.264, 1 for HEVC) is derived from the greatest common
  divisor rather than hardcoded.
- **Reorder delay** — `R = max(decode order position - display order)`.
  Running the DTS ahead by `R` keeps it monotonic and always satisfies
  `dts <= pts`.
- **Pass 2 (writing)** — remux, replacing the video PTS/DTS.  Neither the
  video payload nor the audio is altered by a single byte.

Timestamps are **computed directly in the output time base**, so rounding
happens exactly once.  Matroska is always 1/1000 (milliseconds), which
reproduces the original values; MP4 accepts a request for 1/24000, where one
frame is exactly 1001 ticks and no rounding error remains at all.

## Results

### The broken MKV (`xx.mkv`, 35,510 frames, 24000/1001, H.264)

| | |
| --- | --- |
| **PTS broken** | **11,388 frames (32.1%)** |
| PTS missing | 0 frames |
| Coded video sequences | 327 (POC resets at IDR) |
| Reorder delay | 2 frames |

How it was broken, at the start of decode order:

```
recorded PTS:      0 250 125 125 125 167 209 375 334 334 542 459 459 501 ...
reconstructed PTS: 0 250 125  42  83 167 209 375 334 292 542 459 417 501 ...
                             ~~~ ~~~                 ~~~         ~~~
```

| Check | Result |
| --- | --- |
| Do the 24,122 PTS that were **not** broken match the reconstruction? | **All of them** - this is the evidence that the reconstruction is right |
| Is the display order contiguous, 0..N-1, in all 327 sequences? | **Yes in every one** (no dropped frames) |
| PTS count / distinct values after the repair | **35,510 / 35,510** (no duplicates; it was 24,122 before) |
| PTS spacing in display order | **41 ms x 10,357 and 42 ms x 25,152 only** (the two roundings of 41.708 ms, no gaps) |
| Largest PTS | **1,481,021 ms**, matching `round(35509 x 1001/24)` |
| md5 of the video and audio payload | **Unchanged** |
| The 69,427 audio PTS | **Untouched**, so A/V sync is preserved |
| Frame order when decoded | before `0, 2, 3, 4, ...` (**a frame is lost**), after `0, 1, 2, 3, ...` |

That last row is the clearest symptom: before the repair a frame was being
dropped at playback because two frames claimed the same timestamp.

### Container combinations

A raw H.264 stream (496 frames, extracted from `xx.mkv` with the timestamps
discarded) and the broken MKV (35,510 frames), written to various containers.

| Input | Output | Output time base | Distinct PTS / total | PTS spacing in display order |
| --- | --- | --- | --- | --- |
| raw h264 | mkv | 1/1000 | 496 / 496 | 41 x 144, 42 x 351 |
| raw h264 | mp4 | 1/24000 | 496 / 496 | **1001 x 495** (no rounding) |
| raw h264 | ts | 1/90000 | 496 / 496 | 3753 x 124, 3754 x 371 |
| raw h264 | flv | 1/1000 | 496 / 496 | 41 x 144, 42 x 351 |
| broken mkv | mkv | 1/1000 | 35,510 / 35,510 | 41 x 10357, 42 x 25152 |
| broken mkv | mp4 | 1/24000 | 35,510 / 35,510 | **1001 x 35509** (no rounding) |

- No duplicates, spacing only within the rounding of one frame, no gaps.
  Every path is sound.
- **The PTS of raw h264 -> mkv match the first 496 frames of the container
  path (broken mkv -> mkv) exactly.**  Two independent paths arrive at the
  same answer, which cross-checks the result.
- Re-analysing the MP4 output reports 0% broken.

### HEVC

Verified with material produced by `hevc_qsv` (640x360, 24000/1001, 10
seconds = 240 frames, `-bf 3 -g 48`).

| | |
| --- | --- |
| POC increment | **1** (2 for H.264; derived automatically from the gcd) |
| Coded video sequences | **5** (`-g 48` x 5 = 240 frames, as expected) |
| Reorder delay | 2 frames (B frames present) |
| First PTS in decode order | `0, 167, 83, 42, 125, 334, 250, 209, 292, 501` (a B pyramid) |

| Output | Output time base | Distinct PTS / total | Largest PTS | PTS spacing in display order |
| --- | --- | --- | --- | --- |
| mkv | 1/1000 | 240 / 240 | 9,968 | 41 x 70, 42 x 169 |
| mp4 | 1/24000 | 240 / 240 | **239,239** = 239 x 1001 | **1001 x 239** (no rounding) |
| ts | 1/90000 | 240 / 240 | 904,654 | 3753 x 60, 3754 x 179 |

- Re-analysing the mkv / mp4 output reports **0% broken**.
- The decoded frame order is a clean **`0, 1, 2, ...`**.
- Decoding the raw HEVC, the mkv and the mp4 gives **identical hashes for all
  240 frames**.  (The md5 of the elementary stream differs between the raw
  file and the containers because VPS/SPS/PPS move from in-band into hvcC; the
  pictures themselves are the same.)

For reference: feeding the same material to this build's ffmpeg as
`-r 24000/1001 -c copy` into MKV fails with every option tried, including
`-fflags +genpts`, `-fps_mode passthrough` and `-copyts`
(`Timestamps are unset in a packet` -> `Can't write packet with unknown
timestamp`).  libavformat cannot derive the DTS without an HEVC decoder, which
is the gap tsfix fills.

## Bugs found in this program during generalisation

Both were caught by the verification and have been fixed.

1. **Double rounding** — timestamps were computed in milliseconds (1/1000) and
   then converted to the MP4 time base, so rounding happened twice and
   re-analysing the MP4 output misreported 95.8% as broken.  Changed to
   compute directly in the output time base.
2. **The default for `--avoid-negative-ts`** — the first DTS is negative by
   the reorder delay.  With the default of `disabled` (write negatives as they
   are) a 32 bit millisecond field overflowed in FLV and the PTS developed a
   2147483440 jump.  The default is now `auto`, leaving the decision to
   libavformat per format.  Matroska and MP4 can write negatives as they are,
   so the repair result under `auto` was measured to be PTS-identical to
   `disabled`.

## Assumptions and limitations

- **A constant frame rate is assumed.**  Variable frame rate material cannot
  be reconstructed from the POC alone.  The analysis does check the
  reconstruction against the PTS that were not broken, however, so a violated
  assumption shows up as a broken rate near 100% (and a warning is printed).
- **One packet is assumed to be one frame.**  The program stops if that does
  not hold.  A sequence whose display order is not contiguous also produces a
  warning, which catches dropped frames and field coding.
- Field coding (PAFF/MBAFF) is untested; the display order will not come out
  contiguous, so the warning fires.
- Audio timestamps are out of scope (this bug does not damage them).
- The output is a freshly built container.  Chapters, attachments and
  subtitles are copied, but cues and indexes are regenerated by the muxer.

## A separate issue seen on this machine

**On this Cygwin ffmpeg, opening a file that contains HEVC used to segfault in
ffprobe / ffmpeg.**  With no native HEVC decoder, `hevc_mf` (MediaFoundation)
was picked as the default decoder and crashed after failing to create the MFT.
The H.264 path only warned that the decoder could not be opened, which is what
should have happened here as well.

```
[hevc_mf] could not find any MFT for the given media type
[hevc_mf] could not create MFT
Segmentation fault
```

The workaround was to name a decoder explicitly:

```bash
ffprobe -c:v hevc_qsv ...     # does not crash
```

tsfix is unaffected either way, because it never opens a decoder - only the
parser.

This has since been fixed in the package: `mf_close()` no longer dereferences
a NULL transform, and `libde265` is now the default HEVC decoder.
