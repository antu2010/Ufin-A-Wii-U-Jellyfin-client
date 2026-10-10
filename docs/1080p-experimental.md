# Experimental 1080p30 mode

**Status: experimental, opt-in, untested on real Wii U hardware.** The stable
default is and stays 1280x720. Host tests prove the request, config and
arithmetic; they say nothing about whether the console's hardware decoder
survives 1920x1080. Do not treat it as stable after one good launch -- earlier
work saw a low-probability (roughly 1 in 25) crash on non-aligned geometry.

## Turning it on
* Settings -> **Video quality** -> cycle past "High" to
  **1080p30 Experimental** (1920x1080, 6 Mbps), or
* `config.json`: `"video_height": 1080` (bitrate defaults to 6000000 unless
  `video_bitrate` is also set). Any other `video_height` is ignored (720).

## What it requests from Jellyfin
`Width=1920&Height=1080&VideoCodec=h264&Profile=baseline&Level=41&MaxFramerate=30`
`&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false`, AAC audio, fragmented
MP4 (`stream.mp4`). Baseline only: no B-frames, decode order == display order.
The 720p request is byte-for-byte unchanged.

## What changes in the client
* `VideoStreamOptions` / `UfinConfig` carry the target size (720 or 1080 only).
* `FrameQueue` capacity is derived from the decoded frame size: 24 slots at
  720p (unchanged), 16 at 1080p (~47.5 MiB instead of ~71 MiB).
* Buffering profile for pictures larger than 720p (720p keeps its constants):
  prebuffer 45 s (max wait 90 s) instead of 15 s / 30 s, compressed read-ahead
  cap 160 MiB instead of 96 MiB. Compressed video is the cheap place to spend
  RAM: 1 s is ~0.75 MB at 6 Mbps but ~89 MB decoded.
* Optional `config.json` overrides, 0/absent = automatic, for tuning on the
  console without a rebuild: `video_buffer_mb` (16-512, compressed cap) and
  `video_queue_frames` (4-48, decoded frames).
* The once-a-second log gains a second line with the read-ahead in seconds and
  MiB, average/max hardware-decode+copy time per frame, total/max time blocked
  on the network, and average/max render (texture upload) time.
* `src/media/h264_geometry.h` mirrors the patched allocator's arithmetic for
  diagnostics and tests. Each playback logs, once (and again only if it
  changes): the visible size, the AVFrame linesizes, and the layout the patched
  allocator *expects* the hardware to have written.

## What is deliberately NOT changed
* `patches/ffmpeg-wiiu-fixes.patch` (already sizes the buffer for any
  geometry: for 1920x1080 pitch 2048, rows 1088, Y 2,228,224 + UV 1,114,112 =
  3,342,336 bytes, allocated with +64 KiB slack).
* Compressed-packet buffering, A/V sync, GX2 output, aspect handling.
* No Main/High, no reorder queue, no DR1, no fixed 2800x1408 buffer.

## Decoder-API items that could NOT be verified here
The Wii U `h264/decode.h` (wut) is not in this repository or in the build
sandbox, so these were **not** implemented rather than guessed:
* Alignment of the decoder context (`malloc` today) and of the bitstream given
  to `H264DECSetBitstream`.
* Logging the hardware-reported pitch / width / height / crop.
* `H264DECCheckMemSegmentation`.

To check on your machine (adjust the path to your wut install):
```
grep -n "nextLine\|crop\|CheckMemSegmentation\|H264DecodeResult\|H264DecodeOutput" \
  $DEVKITPRO/wut/include/h264/decode.h
```
If `nextLine` and the crop fields are present, the hardware-reported layout can
be logged from `frame_callback` in `h264_wiiu.c` and compared with the
"expected hw layout" line the client already prints.
