/*
 * fbff - a small ffmpeg-based framebuffer/oss media player
 *
 * Copyright (C) 2009-2011 Ali Gholami Rudi
 *
 * This program is released under GNU GPL version 2.
 */
#include <fcntl.h>
#include <pty.h>
#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/soundcard.h>
#include <pthread.h>
#include "config.h"
#include "ffs.h"
#include "draw.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

static int arg;
static struct termios termios;
static int paused;
static int exited;

static float zoom = 1;
static int magnify = 1;
static int jump = 0;
static int fullscreen = 0;
static int audio = 1;
static int video = 1;
static int just = 0;
static int frame_jmp = 1;	/* the changes to pos_cur for each frame */

static struct ffs *affs;	/* audio ffmpeg stream */
static struct ffs *vffs;	/* video ffmpeg stream */
static int afd;			/* oss fd */
static int vnum;		/* decoded video frame count */

static void stroll(void)
{
	usleep(10000);
}

static void draw_frame(void *img, int linelen)
{
	int w, h;
	fbval_t buf[1 << 14];
	int nr, nc, cb;
	int i, r, c;
	ffs_vinfo(vffs, &w, &h);
	nr = MIN(h * zoom, fb_rows() / magnify);
	nc = MIN(w * zoom, fb_cols() / magnify);
	cb = just ? fb_cols() - nc * magnify : 0;
	for (r = 0; r < nr; r++) {
		fbval_t *row = img + r * linelen;
		if (magnify == 1) {
			fb_set(r, cb, row, nc);
			continue;
		}
		for (c = 0; c < nc; c++)
			for (i = 0; i < magnify; i++)
				buf[c * magnify + i] = row[c];
		for (i = 0; i < magnify; i++)
			fb_set(r * magnify + i, cb, buf, nc * magnify);
	}
}

#define ABUFSZ		(1 << 18)

static int a_cons;
static int a_prod;
static char a_buf[AUDIOBUFS][ABUFSZ];
static int a_len[AUDIOBUFS];
static int a_reset;

static int a_conswait(void)
{
	return a_cons == a_prod;
}

static int a_prodwait(void)
{
	return ((a_prod + 1) & (AUDIOBUFS - 1)) == a_cons;
}

static void a_doreset(int pause)
{
	a_reset = 1 + pause;
	while (audio && a_reset)
		stroll();
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
	struct ffs *ffs = video ? vffs : affs;
	long pos = ffs_pos(ffs, n);
	a_doreset(0);
	if (audio)
		ffs_seek(affs, pos, frame_jmp);
	if (video)
		ffs_seek(vffs, pos, frame_jmp);
}

static void printinfo(void)
{
	struct ffs *ffs = video ? vffs : affs;
	printf("fbff:   %8lx\r", ffs_pos(ffs, 0));
	fflush(stdout);
}

#define JMP1		(1 << 5)
#define JMP2		(JMP1 << 3)
#define JMP3		(JMP2 << 5)

static void execkey(void)
{
	int c;
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':
			exited = 1;
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
			paused = !paused;
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

static void mainloop(void)
{
	int eof = 0;
	while (eof < audio + video) {
		execkey();
		if (exited)
			break;
		if (paused) {
			a_doreset(1);
			waitkey();
			continue;
		}
		while (audio && !eof && !a_prodwait()) {
			int ret = ffs_adec(affs, a_buf[a_prod], ABUFSZ);
			if (ret < 0)
				eof++;
			if (ret > 0) {
				a_len[a_prod] = ret;
				a_prod = (a_prod + 1) & (AUDIOBUFS - 1);
			}
		}
		if (video && (!audio || eof || ffs_vsync(vffs, affs, AUDIOBUFS))) {
			int ignore = jump && (vnum++ % (jump + 1));
			void *buf;
			int ret = ffs_vdec(vffs, ignore ? NULL : &buf);
			if (ret < 0)
				eof++;
			if (ret > 0)
				draw_frame((void *) buf, ret);
			ffs_wait(vffs);
		} else {
			stroll();
		}
	}
	exited = 1;
}

static void oss_init(void)
{
	int rate, ch, bps;
	afd = open("/dev/dsp", O_RDWR);
	if (afd < 0) {
		fprintf(stderr, "cannot open /dev/dsp\n");
		exit(1);
	}
	ffs_ainfo(affs, &rate, &bps, &ch);
	ioctl(afd, SOUND_PCM_WRITE_RATE, &rate);
	ioctl(afd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(afd, SOUND_PCM_WRITE_BITS, &bps);
}

static void oss_close(void)
{
	close(afd);
}

static void *process_audio(void *dat)
{
	oss_init();
	while (1) {
		while (!a_reset && (a_conswait() || paused) && !exited)
			stroll();
		if (exited)
			goto ret;
		if (a_reset) {
			if (a_reset == 1)
				a_cons = a_prod;
			a_reset = 0;
			continue;
		}
		write(afd, a_buf[a_cons], a_len[a_cons]);
		a_cons = (a_cons + 1) & (AUDIOBUFS - 1);
	}
ret:
	oss_close();
	return NULL;
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
	"  -m x     magnify the screen by repeating pixels\n"
	"  -z x     zoom the screen using ffmpeg\n"
	"  -j x     jump every x video frames; for slow machines\n"
	"  -f       start full screen\n"
	"  -v       video only playback\n"
	"  -a       audio only playback\n"
	"  -t       use time based seeking; only if the default does't work\n"
	"  -R       adjust the video to the right of the screen\n\n";

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
		if (!strcmp(argv[i], "-f"))
			fullscreen = 1;
		if (!strcmp(argv[i], "-a"))
			video = 0;
		if (!strcmp(argv[i], "-v"))
			audio = 0;
		if (!strcmp(argv[i], "-t"))
			frame_jmp = 1024 / 32;
		if (!strcmp(argv[i], "-h"))
			printf(usage);
		if (!strcmp(argv[i], "-R"))
			just = 1;
		i++;
	}
}

int main(int argc, char *argv[])
{
	pthread_t a_thread;
	char *path = argv[argc - 1];
	if (argc < 2) {
		printf("usage: %s [options] filename\n", argv[0]);
		return 1;
	}
	read_args(argc, argv);
	ffs_globinit();
	if (video && !(vffs = ffs_alloc(path, 1)))
		video = 0;
	if (audio && !(affs = ffs_alloc(path, 0)))
		audio = 0;
	if (!video && !audio)
		return 1;
	if (audio)
		pthread_create(&a_thread, NULL, process_audio, NULL);
	if (video) {
		int w, h;
		if (fb_init())
			return 1;
		ffs_vinfo(vffs, &w, &h);
		if (magnify != 1 && sizeof(fbval_t) != FBM_BPP(fb_mode()))
			fprintf(stderr, "fbff: magnify != 1 and fbval_t doesn't match\n");
		if (fullscreen)
			zoom = (float) fb_cols() / w / magnify;
		ffs_vsetup(vffs, zoom, fb_mode());
	}
	term_setup();
	signal(SIGCONT, sigcont);
	mainloop();
	term_cleanup();

	if (video) {
		fb_free();
		ffs_free(vffs);
	}
	if (audio) {
		pthread_join(a_thread, NULL);
		ffs_free(affs);
	}
	return 0;
}
