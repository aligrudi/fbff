#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "ffs.h"

#define FFS_SAMPLEFMT		AV_SAMPLE_FMT_S16
#define FFS_CHLAYOUT		AV_CH_LAYOUT_STEREO

#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define MIN(a, b)		((a) < (b) ? (a) : (b))

/* ffmpeg stream */
struct ffs {
	AVCodecContext *cc;
	AVFormatContext *fc;
	AVStream *st;
	AVPacket pkt;
	int si;			/* stream index */
	long ts;		/* frame timestamp (ms) */
	long pts;		/* last decoded packet pts in milliseconds */
	long dur;		/* last decoded packet duration */

	/* decoding video frames */
	struct SwsContext *swsc;
	struct SwrContext *swrc;
	AVFrame *dst;
	AVFrame *tmp;
};

struct ffs *ffs_alloc(char *path, int flags)
{
	struct ffs *ffs;
	int type = flags & FFS_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
	int idx = (flags & FFS_STRIDX) - 1;
	AVDictionary *opt = NULL;
	ffs = malloc(sizeof(*ffs));
	memset(ffs, 0, sizeof(*ffs));
	ffs->si = -1;
	if (avformat_open_input(&ffs->fc, path, NULL, NULL))
		goto failed;
	if (avformat_find_stream_info(ffs->fc, NULL) < 0)
		goto failed;
	ffs->si = av_find_best_stream(ffs->fc, type, idx, -1, NULL, 0);
	if (ffs->si < 0)
		goto failed;
	ffs->cc = ffs->fc->streams[ffs->si]->codec;
	if (avcodec_open2(ffs->cc, avcodec_find_decoder(ffs->cc->codec_id), &opt))
		goto failed;
	ffs->st = ffs->fc->streams[ffs->si];
	ffs->tmp = av_frame_alloc();
	ffs->dst = av_frame_alloc();
	return ffs;
failed:
	ffs_free(ffs);
	return NULL;
}

void ffs_free(struct ffs *ffs)
{
	if (ffs->swrc)
		swr_free(&ffs->swrc);
	if (ffs->swsc)
		sws_freeContext(ffs->swsc);
	if (ffs->dst)
		av_free(ffs->dst);
	if (ffs->tmp)
		av_free(ffs->tmp);
	if (ffs->cc)
		avcodec_close(ffs->cc);
	if (ffs->fc)
		avformat_close_input(&ffs->fc);
	free(ffs);
}

static AVPacket *ffs_pkt(struct ffs *ffs)
{
	AVPacket *pkt = &ffs->pkt;
	while (av_read_frame(ffs->fc, pkt) >= 0) {
		if (pkt->stream_index == ffs->si) {
			long pts = (pkt->dts == AV_NOPTS_VALUE ? 0 : pkt->dts) *
				av_q2d(ffs->st->time_base) * 1000;
			ffs->dur = MIN(MAX(0, pts - ffs->pts), 1000);
			if (pts > ffs->pts || pts + 200 < ffs->pts)
				ffs->pts = pts;
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

void ffs_wait(struct ffs *ffs)
{
	int vdelay = ffs->dur;
	if (!wait(ffs->ts, MAX(vdelay, 20)))
		ffs->ts += MAX(vdelay, 20);
	else
		ffs->ts = ts_ms();		/* out of sync */
}

/* audio/video frame offset difference */
int ffs_avdiff(struct ffs *ffs, struct ffs *affs)
{
	return affs->pts - ffs->pts;
}

long ffs_pos(struct ffs *ffs)
{
	return ffs->pts;
}

void ffs_seek(struct ffs *ffs, struct ffs *vffs, long pos)
{
	av_seek_frame(ffs->fc, vffs->si,
		pos / av_q2d(vffs->st->time_base) / 1000, 0);
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
	*ch = av_get_channel_layout_nb_channels(FFS_CHLAYOUT);
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

static int ffs_bytespersample(ffs)
{
	return av_get_bytes_per_sample(FFS_SAMPLEFMT) *
		av_get_channel_layout_nb_channels(FFS_CHLAYOUT);
}

int ffs_adec(struct ffs *ffs, void *buf, int blen)
{
	int rdec = 0;
	AVPacket tmppkt = {0};
	AVPacket *pkt = ffs_pkt(ffs);
	uint8_t *out[] = {NULL};
	if (!pkt)
		return -1;
	tmppkt.size = pkt->size;
	tmppkt.data = pkt->data;
	while (tmppkt.size > 0) {
		int len, size;
		len = avcodec_decode_audio4(ffs->cc, ffs->tmp, &size, &tmppkt);
		if (len < 0)
			break;
		tmppkt.size -= len;
		tmppkt.data += len;
		if (size <= 0)
			continue;
		out[0] = buf + rdec;
		len = swr_convert(ffs->swrc,
			out, (blen - rdec) / ffs_bytespersample(ffs),
			(void *) ffs->tmp->extended_data, ffs->tmp->nb_samples);
		if (len > 0)
			rdec += len * ffs_bytespersample(ffs);
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

void ffs_vconf(struct ffs *ffs, float zoom, int fbm)
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
	n = avpicture_get_size(pixfmt, w * zoom, h * zoom);
	buf = av_malloc(n * sizeof(uint8_t));
	avpicture_fill((AVPicture *) ffs->dst, buf, pixfmt, w * zoom, h * zoom);
}

void ffs_aconf(struct ffs *ffs)
{
	int rate, bps, ch;
	ffs_ainfo(ffs, &rate, &bps, &ch);
	ffs->swrc = swr_alloc_set_opts(NULL,
		FFS_CHLAYOUT, FFS_SAMPLEFMT, rate,
		ffs->cc->channel_layout, ffs->cc->sample_fmt, ffs->cc->sample_rate,
		0, NULL);
	swr_init(ffs->swrc);
}

void ffs_globinit(void)
{
	av_register_all();
	avformat_network_init();
}

long ffs_duration(struct ffs *ffs)
{
	if (ffs->st->duration == AV_NOPTS_VALUE)
		return 0;
	return ffs->st->duration * av_q2d(ffs->st->time_base) * 1000;
}
