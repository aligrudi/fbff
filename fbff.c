/*
 * fbff - a small ffmpeg-based framebuffer/oss media player
 *
 * Copyright (C) 2009-2015 Ali Gholami Rudi
 *
 * This program is released under the Modified BSD license.
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

static int paused;
static int exited;
static int domark;
static int dojump;
static int arg;
static char filename[32];
static struct termios termios;

static float zoom = 1;
static int magnify = 1;
static int jump = 0;
static int fullscreen = 0;
static int video = 1;		/* video stream; 0=none, 1=auto, >2=idx */
static int audio = 1;		/* audio stream; 0=none, 1=auto, >2=idx */
static int rjust = 0;		/* justify video to the right */
static int bjust = 0;		/* justify video to the bottom */

static struct ffs *affs;	/* audio ffmpeg stream */
static struct ffs *vffs;	/* video ffmpeg stream */
static int afd;			/* oss fd */
static int vnum;		/* decoded video frame count */
static long mark[256];		/* marks */

static int sync_diff;		/* audio/video frame position diff */
static int sync_period;		/* sync after every this many number of frames */
static int sync_since;		/* frames since th last sync */
static int sync_cnt = 32;	/* synchronization steps */
static int sync_cur;		/* synchronization steps left */
static int sync_first;		/* first frame to record sync_diff */

static void stroll(void)
{
	usleep(10000);
}

static void draw_frame(void *img, int linelen)
{
	int w, h;
	fbval_t buf[1 << 14];
	int nr, nc, cb, rb;
	int i, r, c;
	ffs_vinfo(vffs, &w, &h);
	nr = MIN(h * zoom, fb_rows() / magnify);
	nc = MIN(w * zoom, fb_cols() / magnify);
	cb = rjust ? fb_cols() - nc * magnify : 0;
	rb = bjust ? fb_rows() - nr * magnify : 0;
	for (r = 0; r < nr; r++) {
		fbval_t *row = img + r * linelen;
		if (magnify == 1) {
			fb_set(rb + r, cb, row, nc);
			continue;
		}
		for (c = 0; c < nc; c++)
			for (i = 0; i < magnify; i++)
				buf[c * magnify + i] = row[c];
		for (i = 0; i < magnify; i++)
			fb_set((rb + r) * magnify + i, cb, buf, nc * magnify);
	}
}

static int oss_open(void)
{
	int rate, ch, bps;
	afd = open("/dev/dsp", O_WRONLY);
	if (afd < 0)
		return 1;
	ffs_ainfo(affs, &rate, &bps, &ch);
	ioctl(afd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(afd, SOUND_PCM_WRITE_BITS, &bps);
	ioctl(afd, SOUND_PCM_WRITE_RATE, &rate);
	return 0;
}

static void oss_close(void)
{
	if (afd > 0)
		close(afd);
	afd = 0;
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

static int cmdread(void)
{
	char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return b;
}

static void cmdwait(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = 0;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static void cmdjmp(int n, int rel)
{
	struct ffs *ffs = video ? vffs : affs;
	long pos = (rel ? ffs_pos(ffs) : 0) + n * 1000;
	a_doreset(0);
	sync_cur = sync_cnt;
	if (pos < 0)
		pos = 0;
	if (!rel)
		mark['\''] = ffs_pos(ffs);
	if (audio)
		ffs_seek(affs, ffs, pos);
	if (video)
		ffs_seek(vffs, ffs, pos);
}

static void cmdinfo(void)
{
	struct ffs *ffs = video ? vffs : affs;
	long pos = ffs_pos(ffs);
	long percent = ffs_duration(ffs) ? pos * 1000 / ffs_duration(ffs) : 0;
	printf("%c %3ld.%01ld%%  %3ld:%02ld.%01ld  (AV:%4d)     [%20s] \r",
		paused ? (afd < 0 ? '*' : ' ') : '>',
		percent / 10, percent % 10,
		pos / 60000, (pos % 60000) / 1000, (pos % 1000) / 100,
		video && audio ? ffs_avdiff(vffs, affs) : 0,
		filename);
	fflush(stdout);
}

static int cmdarg(int def)
{
	int n = arg;
	arg = 0;
	return n ? n : def;
}

static void cmdexec(void)
{
	int c;
	while ((c = cmdread()) >= 0) {
		if (domark) {
			domark = 0;
			mark[c] = ffs_pos(video ? vffs : affs);
			continue;
		}
		if (dojump) {
			dojump = 0;
			if (mark[c] > 0)
				cmdjmp(mark[c] / 1000, 0);
			continue;
		}
		switch (c) {
		case 'q':
			exited = 1;
			break;
		case 'l':
			cmdjmp(cmdarg(1) * 10, 1);
			break;
		case 'h':
			cmdjmp(-cmdarg(1) * 10, 1);
			break;
		case 'j':
			cmdjmp(cmdarg(1) * 60, 1);
			break;
		case 'k':
			cmdjmp(-cmdarg(1) * 60, 1);
			break;
		case 'J':
			cmdjmp(cmdarg(1) * 600, 1);
			break;
		case 'K':
			cmdjmp(-cmdarg(1) * 600, 1);
			break;
		case 'G':
			cmdjmp(cmdarg(1) * 60, 0);
			break;
		case '%':
			cmdjmp(cmdarg(0) * ffs_duration(vffs ? vffs : affs) / 100000, 0);
			break;
		case 'm':
			domark = 1;
			break;
		case '\'':
			dojump = 1;
			break;
		case 'i':
			cmdinfo();
			break;
		case ' ':
		case 'p':
			if (audio && paused)
				if (oss_open())
					break;
			if (audio && !paused)
				oss_close();
			paused = !paused;
			sync_cur = sync_cnt;
			break;
		case '-':
			sync_diff = -cmdarg(0);
			break;
		case '+':
			sync_diff = cmdarg(0);
			break;
		case 'a':
			sync_diff = ffs_avdiff(vffs, affs);
			break;
		case 'c':
			sync_cnt = cmdarg(0);
			break;
		case 's':
			sync_cur = cmdarg(sync_cnt);
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

/* return nonzero if one more video frame can be decoded */
static int vsync(void)
{
	if (sync_period && sync_since++ >= sync_period) {
		sync_cur = sync_cnt;
		sync_since = 0;
	}
	if (sync_first) {
		sync_cur = 0;
		if (sync_first < vnum) {
			sync_first = 0;
			sync_diff = ffs_avdiff(vffs, affs);
		}
	}
	if (sync_cur > 0) {
		sync_cur--;
		return ffs_avdiff(vffs, affs) >= sync_diff;
	}
	ffs_wait(vffs);
	return 1;
}

static void mainloop(void)
{
	int eof = 0;
	while (eof < audio + video) {
		cmdexec();
		if (exited)
			break;
		if (paused) {
			a_doreset(1);
			cmdwait();
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
		if (video && (!audio || eof || vsync())) {
			int ignore = jump && (vnum % (jump + 1));
			void *buf;
			int ret = ffs_vdec(vffs, ignore ? NULL : &buf);
			vnum++;
			if (ret < 0)
				eof++;
			if (ret > 0)
				draw_frame((void *) buf, ret);
		} else {
			stroll();
		}
	}
	exited = 1;
}

static void *process_audio(void *dat)
{
	while (1) {
		while (!a_reset && (a_conswait() || paused) && !exited)
			stroll();
		if (exited)
			return NULL;
		if (a_reset) {
			if (a_reset == 1)
				a_cons = a_prod;
			a_reset = 0;
			continue;
		}
		if (afd > 0) {
			write(afd, a_buf[a_cons], a_len[a_cons]);
			a_cons = (a_cons + 1) & (AUDIOBUFS - 1);
		}
	}
	return NULL;
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
}

static void term_cleanup(void)
{
	tcsetattr(0, 0, &termios);
}

static void sigcont(int sig)
{
	term_setup();
}

static char *usage = "usage: fbff [options] file\n"
	"\noptions:\n"
	"  -z x     zoom the video\n"
	"  -m x     magnify the video by duplicating pixels\n"
	"  -j x     jump every x video frames; for slow machines\n"
	"  -f       start full screen\n"
	"  -v x     select video stream; '-' disables video\n"
	"  -a x     select audio stream; '-' disables audio\n"
	"  -s       always synchronize (-sx for every x frames)\n"
	"  -u       record A/V delay after the first few frames\n"
	"  -r       adjust the video to the right of the screen\n"
	"  -b       adjust the video to the bottom of the screen\n\n";

static void read_args(int argc, char *argv[])
{
	int i = 1;
	while (i < argc) {
		char *c = argv[i];
		if (c[0] != '-')
			break;
		if (c[1] == 'm')
			magnify = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'z')
			zoom = c[2] ? atof(c + 2) : atof(argv[++i]);
		if (c[1] == 'j')
			jump = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'f')
			fullscreen = 1;
		if (c[1] == 's')
			sync_period = c[2] ? atoi(c + 2) : 1;
		if (c[1] == 'h')
			printf(usage);
		if (c[1] == 'r')
			rjust = 1;
		if (c[1] == 'b')
			bjust = 1;
		if (c[1] == 'u')
			sync_first = 32;
		if (c[1] == 'v') {
			char *arg = c[2] ? c + 2 : argv[++i];
			video = arg[0] == '-' ? 0 : atoi(arg) + 2;
		}
		if (c[1] == 'a') {
			char *arg = c[2] ? c + 2 : argv[++i];
			audio = arg[0] == '-' ? 0 : atoi(arg) + 2;
		}
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
	snprintf(filename, sizeof(filename), "%s", path);
	if (video && !(vffs = ffs_alloc(path, FFS_VIDEO | (video - 1))))
		video = 0;
	if (audio && !(affs = ffs_alloc(path, FFS_AUDIO | (audio - 1))))
		audio = 0;
	if (!video && !audio)
		return 1;
	if (audio) {
		ffs_aconf(affs);
		if (oss_open()) {
			fprintf(stderr, "fbff: /dev/dsp busy?\n");
			return 1;
		}
		pthread_create(&a_thread, NULL, process_audio, NULL);
	}
	if (video) {
		int w, h;
		if (fb_init())
			return 1;
		ffs_vinfo(vffs, &w, &h);
		if (magnify != 1 && sizeof(fbval_t) != FBM_BPP(fb_mode()))
			fprintf(stderr, "fbff: magnify != 1 and fbval_t doesn't match\n");
		if (fullscreen) {
			float hz = (float) fb_rows() / h / magnify;
			float wz = (float) fb_cols() / w / magnify;
			zoom = hz < wz ? hz : wz;
		}
		ffs_vconf(vffs, zoom, fb_mode());
	}
	term_setup();
	signal(SIGCONT, sigcont);
	mainloop();
	term_cleanup();
	printf("\n");

	if (video) {
		fb_free();
		ffs_free(vffs);
	}
	if (audio) {
		pthread_join(a_thread, NULL);
		oss_close();
		ffs_free(affs);
	}
	return 0;
}
