/*
 * fbff - A small ffmpeg-based framebuffer/alsa media player
 *
 * Copyright (C) 2009-2010 Ali Gholami Rudi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, as published by the
 * Free Software Foundation.
 */
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "draw.h"

static AVFormatContext *fc;
static AVFrame *frame;
static struct SwsContext *swsc;

static int vsi = -1;		/* video stream index */
static AVCodecContext *vcc;	/* video codec context */
static AVCodec *vc;		/* video codec */

static int asi = -1;		/* audio stream index */
static AVCodecContext *acc;	/* audio codec context */
static AVCodec *ac;		/* audio codec */

static int seek_idx;		/* stream index used for seeking */
static int pos_cur;		/* current frame number in seek_idx stream */
static int pos_max;		/* maximum frame number seen so far */
static int frame_jmp = 1;	/* the changes to pos_cur for each frame */

static snd_pcm_t *alsa;
static int bps; 		/* bytes per sample */
static int arg;
static struct termios termios;
static unsigned long num;	/* decoded video frame number */
static unsigned int vnum;	/* number of successive video frames */
static int cmd;

static float zoom = 1;
static int magnify = 0;
static int drop = 0;
static int jump = 0;
static int fullscreen = 0;
static int audio = 1;
static int video = 1;
static int rate = 0;

static void init_streams(void)
{
	int i;
	for (i = 0; i < fc->nb_streams; i++) {
		if (fc->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO)
			vsi = i;
		if (fc->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO)
			asi = i;
	}
	if (video && vsi != -1) {
		vcc = fc->streams[vsi]->codec;
		vc = avcodec_find_decoder(vcc->codec_id);
		avcodec_open(vcc, vc);
	}
	if (audio && asi != -1) {
		acc = fc->streams[asi]->codec;
		ac = avcodec_find_decoder(acc->codec_id);
		avcodec_open(acc, ac);
	}
	seek_idx = vcc ? vsi : asi;
}

static void draw_frame(void)
{
	fbval_t buf[1 << 14];
	int r, c;
	int nr = MIN(vcc->height * zoom, fb_rows() / magnify);
	int nc = MIN(vcc->width * zoom, fb_cols() / magnify);
	int i;
	for (r = 0; r < nr; r++) {
		unsigned char *row = frame->data[0] + r * frame->linesize[0];
		for (c = 0; c < nc; c++) {
			fbval_t v = fb_color(row[c * 3],
					row[c * 3 + 1],
					row[c * 3 + 2]);
			for (i = 0; i < magnify; i++)
				buf[c * magnify + i] = v;
		}
		for (i = 0; i < magnify; i++)
			fb_set(r * magnify + i, 0, buf, nc * magnify);
	}
}

static void decode_video_frame(AVFrame *main_frame, AVPacket *packet)
{
	int fine = 0;
	avcodec_decode_video2(vcc, main_frame, &fine, packet);
	if (fine && (!jump || !(num % jump)) && (!drop || !vnum)) {
		sws_scale(swsc, main_frame->data, main_frame->linesize,
			  0, vcc->height, frame->data, frame->linesize);
		draw_frame();
	}
}

#define AUDIOBUFSIZE		(1 << 20)

static void decode_audio_frame(AVPacket *pkt)
{
	char buf[AUDIOBUFSIZE];
	AVPacket tmppkt;
	tmppkt.size = pkt->size;
	tmppkt.data = pkt->data;
	while (tmppkt.size > 0) {
		int size = sizeof(buf);
		int len = avcodec_decode_audio3(acc, (int16_t *) buf,
						&size, &tmppkt);
		if (len < 0)
			break;
		if (size <= 0)
			continue;
		if (snd_pcm_writei(alsa, buf, size / bps) < 0)
			snd_pcm_prepare(alsa);
		tmppkt.size -= len;
		tmppkt.data += len;
	}
}

static int readkey(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) <= 0)
		return -1;
	return b;
}

static void waitkey(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static int ffarg(void)
{
	int n = arg;
	arg = 0;
	return n ? n : 1;
}

static void ffjmp(int n, int rel)
{
	pos_cur = rel ? pos_cur + n * frame_jmp : pos_cur * n / 100;
	if (pos_cur < 0)
		pos_cur = 0;
	if (pos_cur > pos_max)
		pos_max = pos_cur;
	av_seek_frame(fc, seek_idx, pos_cur,
			frame_jmp == 1 ? AVSEEK_FLAG_FRAME : 0);
}

static void printinfo(void)
{
	printf("fbff:   %d\t%d    \r", pos_cur, pos_cur * 100 / pos_max);
	fflush(stdout);
}

#define JMP1		(1 << 5)
#define JMP2		(JMP1 << 3)
#define JMP3		(JMP2 << 5)

#define FF_PLAY			0
#define FF_PAUSE		1
#define FF_EXIT			2

static void execkey(void)
{
	int c;
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':
			cmd = FF_EXIT;
			break;
		case 'l':
			ffjmp(ffarg() * JMP1, 1);
			break;
		case 'h':
			ffjmp(-ffarg() * JMP1, 1);
			break;
		case 'j':
			ffjmp(ffarg() * JMP2, 1);
			break;
		case 'k':
			ffjmp(-ffarg() * JMP2, 1);
			break;
		case 'J':
			ffjmp(ffarg() * JMP3, 1);
			break;
		case 'K':
			ffjmp(-ffarg() * JMP3, 1);
			break;
		case '%':
			if (arg)
				ffjmp(100, 0);
			break;
		case 'i':
			printinfo();
			break;
		case ' ':
		case 'p':
			cmd = cmd ? FF_PLAY : FF_PAUSE;
			break;
		case 27:
			arg = 0;
			break;
		default:
			if (isdigit(c))
				arg = arg * 10 + c - '0';
		}
	}
}

static void read_frames(void)
{
	AVFrame *main_frame = avcodec_alloc_frame();
	AVPacket pkt;
	uint8_t *buf;
	int n = AUDIOBUFSIZE;
	if (vcc)
		n = avpicture_get_size(PIX_FMT_RGB24, vcc->width * zoom,
					   vcc->height * zoom);
	buf = av_malloc(n * sizeof(uint8_t));
	if (vcc)
		avpicture_fill((AVPicture *) frame, buf, PIX_FMT_RGB24,
				vcc->width * zoom, vcc->height * zoom);
	while (cmd != FF_EXIT && av_read_frame(fc, &pkt) >= 0) {
		execkey();
		if (cmd == FF_PAUSE) {
			waitkey();
			continue;
		}
		if (vcc && pkt.stream_index == vsi) {
			if (rate)
				usleep(1000000 / rate);
			decode_video_frame(main_frame, &pkt);
			vnum++;
			num++;
		}
		if (acc && pkt.stream_index == asi) {
			decode_audio_frame(&pkt);
			vnum = 0;
		}
		if (pkt.stream_index == seek_idx) {
			pos_cur += frame_jmp;
			if (pos_cur > pos_max)
				pos_max = pos_cur;
		}
		av_free_packet(&pkt);
	}
	av_free(buf);
	av_free(main_frame);
}

#define ALSADEV			"default"

static void alsa_init(void)
{
	int format = SND_PCM_FORMAT_S16_LE;
	if (snd_pcm_open(&alsa, ALSADEV, SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return;
	snd_pcm_set_params(alsa, format, SND_PCM_ACCESS_RW_INTERLEAVED,
			acc->channels, acc->sample_rate, 1, 500000);
	bps = acc->channels * snd_pcm_format_physical_width(format) / 8;
	snd_pcm_prepare(alsa);
}

static void alsa_close(void)
{
	snd_pcm_close(alsa);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(STDIN_FILENO, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtermios);
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

static void term_cleanup(void)
{
	tcsetattr(STDIN_FILENO, 0, &termios);
}

static void sigcont(int sig)
{
	term_setup();
}

static char *usage = "usage: fbff [options] file\n"
	"\noptions:\n"
	"    -m x	magnify the screen by repeating pixels\n"
	"    -z x	zoom the screen using ffmpeg\n"
	"    -j x	jump every x video frames; for slow machines\n"
	"    -d		don't draw following video frames\n"
	"    -f		start full screen\n"
	"    -r	x	set the fps; for video only playback\n"
	"    -v		video only playback\n"
	"    -a		audio only playback\n"
	"    -t		use time based seeking; only if the default does't work\n";

static void read_args(int argc, char *argv[])
{
	int i = 1;
	while (i < argc) {
		if (!strcmp(argv[i], "-m"))
			magnify = atoi(argv[++i]);
		if (!strcmp(argv[i], "-z"))
			zoom = atof(argv[++i]);
		if (!strcmp(argv[i], "-j"))
			jump = atoi(argv[++i]);
		if (!strcmp(argv[i], "-d"))
			drop = 1;
		if (!strcmp(argv[i], "-f"))
			fullscreen = 1;
		if (!strcmp(argv[i], "-r"))
			rate = atoi(argv[++i]);
		if (!strcmp(argv[i], "-a"))
			video = 0;
		if (!strcmp(argv[i], "-v"))
			audio = 0;
		if (!strcmp(argv[i], "-t"))
			frame_jmp = 1024 / 32;
		if (!strcmp(argv[i], "-h"))
			printf(usage);
		i++;
	}
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: %s [options] filename\n", argv[0]);
		return 1;
	}
	read_args(argc, argv);
	av_register_all();
	if (av_open_input_file(&fc, argv[argc - 1], NULL, 0, NULL))
		return 1;
	if (av_find_stream_info(fc) < 0)
		return 1;
	init_streams();
	frame = avcodec_alloc_frame();
	if (acc)
		alsa_init();
	if (vcc) {
		fb_init();
		if (!magnify)
			magnify = fb_cols() / vcc->width / zoom;
		if (fullscreen)
			zoom = (float) fb_cols() / vcc->width / magnify;
		swsc = sws_getContext(vcc->width, vcc->height, vcc->pix_fmt,
			vcc->width * zoom, vcc->height * zoom,
			PIX_FMT_RGB24, SWS_FAST_BILINEAR | SWS_CPU_CAPS_MMX2,
			NULL, NULL, NULL);
	}

	term_setup();
	signal(SIGCONT, sigcont);
	read_frames();
	term_cleanup();

	if (vcc) {
		fb_free();
		sws_freeContext(swsc);
		avcodec_close(vcc);
	}
	if (acc) {
		alsa_close();
		avcodec_close(acc);
	}
	av_free(frame);
	av_close_input_file(fc);
	return 0;
}
