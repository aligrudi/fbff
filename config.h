/* framebuffer device */
#define FBDEV_PATH	"/dev/fb0"

/* framebuffer depth; FFMPEG_PIXFMT should match too */
typedef unsigned int fbval_t;

/*
 * ffmpeg pixel format; the most common values based on fb depth are:
 * + 32bit: PIX_FMT_RGB32
 * + 16bit: PIX_FMT_RGB565
 * + 8bit:  PIX_FMT_RGB8
 */
#define FFMPEG_PIXFMT		PIX_FMT_RGB32

/* audio packets to buffer (power of two); increase if sound is choppy */
#define AUDIOBUFS		(1 << 3)
