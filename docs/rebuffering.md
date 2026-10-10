# Graceful rebuffering

When the network cannot keep up with the stream, playback now pauses cleanly
*before* the buffer runs dry, shows a buffering wheel over the last picture,
and resumes by itself -- instead of playing into an empty buffer and stuttering.
Video only; music is unchanged. The compressed read-ahead, `FrameQueue` sizes,
`MAX_AUDIO_AHEAD_SECONDS`, prebuffer and the decoder are untouched.

## States (`src/media/playback_state.h`)
```
STARTUP_BUFFERING --prebuffer done--> PLAYING
PLAYING   --playable <= 2 s (and more media may arrive)--> BUFFERING
BUFFERING --playable >= 12 s, or nothing more can arrive--> PLAYING
```
Entry and exit differ on purpose (hysteresis). BUFFERING is held for at least
0.5 s so the wheel never just flashes. It is never entered while the user has
paused, before the first picture is shown, or once the demuxer has hit the real
end of the stream.

## "Playable seconds"
Timeline seconds ahead of the playback clock, the smaller of video and audio:
* video = (newest decoded frame's timestamp + frame time - clock) + compressed video read-ahead
* audio = what SDL still holds + compressed audio read-ahead

Never a packet or frame count. The compressed part comes from `Decoder`, which
now reports `max(sum of packet durations, span between oldest and newest queued
packet timestamps)`. The second term matters: this FFmpeg's `mov` demuxer leaves
`AVPacket::duration` at 0 on a live fragmented stream (Ufin skips
`avformat_find_stream_info`), so a duration sum alone reads 0.

## What "hold" does
The same mechanism as the user's pause: `AudioOutput::setPaused()` freezes the
audio clock exactly where it is and stops the device; no new frame is shown, so
the last picture stays up; the position stops. Resuming extrapolates from the
frozen clock -- no reset, no timestamp jump, so A/V stays in sync. The decode
thread keeps running and, with its outputs full, keeps reading packets: that is
what refills the buffer. The stream is never re-requested and nothing seeks.

## Seek / stop / EOF
Seek and stop are handled by the same poll loop in BUFFERING as in PLAYING; a
seek ends `play()` and the caller starts a new one (fresh decoder and queues,
startup prebuffer again). Genuine EOF (`Decoder::demuxFinished()`) leaves
BUFFERING at once so the remaining media plays out.

## Logging
State changes only, never per frame:
```
Ufin: playback state PLAYING -> BUFFERING
Ufin: BUFFERING entered: video=2.6s audio=2.0s (pos ..., low-water 2.0s, resume at 12.0s)
Ufin: playback state BUFFERING -> PLAYING
Ufin: BUFFERING recovery: video=.. audio=.. after 13.3s
```
The once-a-second buffer line gained `state=` and `playable=`.

## Tuning
`PlayOptions::rebufferLowWaterSeconds / rebufferRecoverySeconds / prebufferSeconds`
(0 = automatic: 2 s / 12 s / existing prebuffer). Not exposed in config.json.
