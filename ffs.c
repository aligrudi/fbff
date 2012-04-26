#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "ffs.h"

/* ffmpeg stream */
struct ffs {
	AVCodecContext *cc;
	AVFormatContext *fc;
	AVPacket pkt;
	int si;			/* stream index */
	long ts;		/* frame timestamp (ms) */
	long seq;		/* current position in this stream */
	long seq_all;		/* seen packets after ffs_seek() */
	long seq_cur;		/* decoded packet after ffs_seek() */

	/* decoding video frames */
	struct SwsContext *swsc;
	AVFrame *dst;
	AVFrame *tmp;
};

struct ffs *ffs_alloc(char *path, int flags)
{
	struct ffs *ffs;
	int type = flags & FFS_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
	int idx = (flags & FFS_STRIDX) - 1;
	ffs = malloc(sizeof(*ffs));
	memset(ffs, 0, sizeof(*ffs));
	ffs->si = -1;
	if (avformat_open_input(&ffs->fc, path, NULL, NULL))
		goto failed;
	if (av_find_stream_info(ffs->fc) < 0)
		goto failed;
	ffs->si = av_find_best_stream(ffs->fc, type, idx, -1, NULL, 0);
	if (ffs->si < 0)
		goto failed;
	ffs->cc = ffs->fc->streams[ffs->si]->codec;
	avcodec_open(ffs->cc, avcodec_find_decoder(ffs->cc->codec_id));
	return ffs;
failed:
	ffs_free(ffs);
	return NULL;
}

void ffs_free(struct ffs *ffs)
{
	if (ffs->swsc)
		sws_freeContext(ffs->swsc);
	if (ffs->dst)
		av_free(ffs->dst);
	if (ffs->tmp)
		av_free(ffs->tmp);
	if (ffs->cc)
		avcodec_close(ffs->cc);
	if (ffs->fc)
		av_close_input_file(ffs->fc);
	free(ffs);
}

static AVPacket *ffs_pkt(struct ffs *ffs)
{
	AVPacket *pkt = &ffs->pkt;
	while (av_read_frame(ffs->fc, pkt) >= 0) {
		ffs->seq_all++;
		if (pkt->stream_index == ffs->si) {
			ffs->seq_cur++;
			ffs->seq++;
			return pkt;
		}
		av_free_packet(pkt);
	}
	return NULL;
}

static long ts_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int wait(long ts, int vdelay)
{
	long nts = ts_ms();
	if (nts > ts && ts + vdelay > nts) {
		usleep((ts + vdelay - nts) * 1000);
		return 0;
	}
	return 1;
}

#define MAX(a, b)	((a) < (b) ? (b) : (a))

void ffs_wait(struct ffs *ffs)
{
	AVRational *r = &ffs->fc->streams[ffs->si]->r_frame_rate;
	int vdelay = 1000 * r->den / r->num;
	if (!wait(ffs->ts, MAX(vdelay, 20)))
		ffs->ts += MAX(vdelay, 20);
	else
		ffs->ts = ts_ms();		/* out of sync */
}

/* audio/video frame offset difference */
int ffs_avdiff(struct ffs *ffs, struct ffs *affs)
{
	return affs->seq_all - ffs->seq_all;
}

long ffs_pos(struct ffs *ffs, int diff)
{
	return (ffs->si << 28) | (ffs->seq + diff);
}

void ffs_seek(struct ffs *ffs, long pos, int perframe)
{
	long idx = pos >> 28;
	long seq = pos & 0x00ffffff;
	av_seek_frame(ffs->fc, idx, seq * perframe,
			perframe == 1 ? AVSEEK_FLAG_FRAME : 0);
	ffs->seq = seq;
	ffs->seq_all = 0;
	ffs->seq_cur = 0;
	ffs->ts = 0;
}

void ffs_vinfo(struct ffs *ffs, int *w, int *h)
{
	*h = ffs->cc->height;
	*w = ffs->cc->width;
}

void ffs_ainfo(struct ffs *ffs, int *rate, int *bps, int *ch)
{
	*rate = ffs->cc->sample_rate;
	*ch = ffs->cc->channels;
	*bps = 16;
}

int ffs_vdec(struct ffs *ffs, void **buf)
{
	AVCodecContext *vcc = ffs->cc;
	AVPacket *pkt = ffs_pkt(ffs);
	int fine = 0;
	if (!pkt)
		return -1;
	avcodec_decode_video2(vcc, ffs->tmp, &fine, pkt);
	av_free_packet(pkt);
	if (fine && buf) {
		sws_scale(ffs->swsc, (void *) ffs->tmp->data, ffs->tmp->linesize,
			  0, vcc->height, ffs->dst->data, ffs->dst->linesize);
		*buf = (void *) ffs->dst->data[0];
		return ffs->dst->linesize[0];
	}
	return 0;
}

int ffs_adec(struct ffs *ffs, void *buf, int blen)
{
	int rdec = 0;
	AVPacket tmppkt = {0};
	AVPacket *pkt = ffs_pkt(ffs);
	if (!pkt)
		return -1;
	tmppkt.size = pkt->size;
	tmppkt.data = pkt->data;
	while (tmppkt.size > 0) {
		int size = blen - rdec;
		int len = avcodec_decode_audio3(ffs->cc, (int16_t *) (buf + rdec),
						&size, &tmppkt);
		if (len < 0)
			break;
		tmppkt.size -= len;
		tmppkt.data += len;
		if (size > 0)
			rdec += size;
	}
	av_free_packet(pkt);
	return rdec;
}

static int fbm2pixfmt(int fbm)
{
	switch (fbm & 0x0fff) {
	case 0x888:
		return PIX_FMT_RGB32;
	case 0x565:
		return PIX_FMT_RGB565;
	case 0x233:
		return PIX_FMT_RGB8;
	default:
		fprintf(stderr, "ffs: unknown fb_mode()\n");
		return PIX_FMT_RGB32;
	}
}

void ffs_vsetup(struct ffs *ffs, float zoom, int fbm)
{
	int h = ffs->cc->height;
	int w = ffs->cc->width;
	int fmt = ffs->cc->pix_fmt;
	int pixfmt = fbm2pixfmt(fbm);
	uint8_t *buf = NULL;
	int n;
	ffs->swsc = sws_getContext(w, h, fmt, w * zoom, h * zoom,
			pixfmt, SWS_FAST_BILINEAR | SWS_CPU_CAPS_MMX2,
			NULL, NULL, NULL);
	ffs->dst = avcodec_alloc_frame();
	ffs->tmp = avcodec_alloc_frame();
	n = avpicture_get_size(pixfmt, w * zoom, h * zoom);
	buf = av_malloc(n * sizeof(uint8_t));
	avpicture_fill((AVPicture *) ffs->dst, buf, pixfmt, w * zoom, h * zoom);
}

void ffs_globinit(void)
{
	av_register_all();
	avformat_network_init();
}
