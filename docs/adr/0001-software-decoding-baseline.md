# Software decoding first, on a pinned media baseline

The Switch 2 port ships software decoding on FFmpeg 7.1 and mpv 0.36.0 with `hwdec=no` as its known-good release baseline. Native libounce VideoDecoder work is a separate G3 research track on its own branch, merged only on measured benefit and acceptable maintenance cost. We chose this because a successful compile or a working GPU renderer does not establish hardware decode support on a particular console and firmware, and a speculative hardware path would delay the first usable beta.
