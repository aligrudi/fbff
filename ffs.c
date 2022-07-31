#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "ffs.h"

#define FFS_SAMPLEFMT		AV_SAMPLE_FMT_S16
#define FFS_CHLAYOUT		AV_CH_LAYOUT_STEREO
#define FFS_CHCNT		2

#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define MIN(a, b)		((a) < (b) ? (a) : (b))

/* ffmpeg stream */
struct ffs {
	AVCodecContext *cc;
	AVFormatContext *fc;
	AVStream *st;
	AVPacket pkt;		/* used in ffs_pkt() */
	int si;			/* stream index */
	long ts;		/* frame timestamp (ms) */
	long pts;		/* last decoded packet pts in milliseconds */
	long dur;		/* last decoded packet duration */
	struct SwsContext *swsc;
	struct SwrContext *swrc;
	AVFrame *dst;		/* used in ffs_vdec() */
	AVFrame *tmp;		/* used in ffs_recv() */
};

static int ffs_stype(int flags)
{
	if (flags & FFS_VIDEO)
		return AVMEDIA_TYPE_VIDEO;
	if (flags & FFS_AUDIO)
		return AVMEDIA_TYPE_AUDIO;
	if (flags & FFS_SUBTS)
		return AVMEDIA_TYPE_SUBTITLE;
	return 0;
}

struct ffs *ffs_alloc(char *path, int flags)
{
	struct ffs *ffs;
	int idx = (flags & FFS_STRIDX) - 1;
	AVDictionary *opt = NULL;
	const AVCodec *dec = NULL;
	ffs = malloc(sizeof(*ffs));
	memset(ffs, 0, sizeof(*ffs));
	ffs->si = -1;
	if (avformat_open_input(&ffs->fc, path, NULL, NULL))
		goto failed;
	if (avformat_find_stream_info(ffs->fc, NULL) < 0)
		goto failed;
	ffs->si = av_find_best_stream(ffs->fc, ffs_stype(flags), idx, -1, NULL, 0);
	if (ffs->si < 0)
		goto failed;
	dec = avcodec_find_decoder(ffs->fc->streams[ffs->si]->codecpar->codec_id);
	if (dec == NULL)
		goto failed;
	ffs->cc = avcodec_alloc_context3(dec);
	if (ffs->cc == NULL)
		goto failed;
	avcodec_parameters_to_context(ffs->cc, ffs->fc->streams[ffs->si]->codecpar);
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
		av_packet_unref(pkt);
	}
	return NULL;
}

static AVFrame *ffs_recv(struct ffs *ffs)
{
	AVCodecContext *vcc = ffs->cc;
	AVPacket *pkt = NULL;
	while (1) {
		int ret = avcodec_receive_frame(vcc, ffs->tmp);
		if (ret == 0)
			return ffs->tmp;
		if (ret < 0 && ret != AVERROR(EAGAIN))
			return NULL;
		if ((pkt = ffs_pkt(ffs)) == NULL)
			return NULL;
		if (avcodec_send_packet(vcc, pkt) < 0) {
			av_packet_unref(pkt);
			return NULL;
		}
		av_packet_unref(pkt);
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
	*ch = FFS_CHCNT;
	*bps = 16;
}

int ffs_vdec(struct ffs *ffs, void **buf)
{
	AVFrame *tmp = ffs_recv(ffs);
	AVFrame *dst = ffs->dst;
	if (tmp == NULL)
		return -1;
	if (buf) {
		sws_scale(ffs->swsc, (void *) tmp->data, tmp->linesize,
			  0, ffs->cc->height, dst->data, dst->linesize);
		*buf = (void *) dst->data[0];
		return dst->linesize[0];
	}
	return 0;
}

int ffs_sdec(struct ffs *ffs, char *buf, int blen, long *beg, long *end)
{
	AVPacket *pkt = ffs_pkt(ffs);
	AVSubtitle sub = {0};
	AVSubtitleRect *rect;
	int fine = 0;
	int i;
	if (!pkt)
		return -1;
	avcodec_decode_subtitle2(ffs->cc, &sub, &fine, pkt);
	av_packet_unref(pkt);
	buf[0] = '\0';
	if (!fine)
		return 1;
	rect = sub.num_rects ? sub.rects[0] : NULL;
	if (rect && rect->text)
		snprintf(buf, blen, "%s", sub.rects[0]->text);
	if (rect && !rect->text && rect->ass) {
		char *s = rect->ass;
		for (i = 0; s && i < 9; i++)
			s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL;
		if (s)
			snprintf(buf, blen, "%s", s);
	}
	if (strchr(buf, '\n'))
		*strchr(buf, '\n') = '\0';
	*beg = ffs->pts + sub.start_display_time * av_q2d(ffs->st->time_base) * 1000;
	*end = ffs->pts + sub.end_display_time * av_q2d(ffs->st->time_base) * 1000;
	avsubtitle_free(&sub);
	return 0;
}

static int ffs_bytespersample(struct ffs *ffs)
{
	return av_get_bytes_per_sample(FFS_SAMPLEFMT) * FFS_CHCNT;
}

int ffs_adec(struct ffs *ffs, void *buf, int blen)
{
	AVFrame *tmp = ffs_recv(ffs);
	uint8_t *out[] = {buf};
	int len;
	if (tmp == NULL)
		return -1;
	len = swr_convert(ffs->swrc, out, blen / ffs_bytespersample(ffs),
		(void *) tmp->extended_data, tmp->nb_samples);
	return len > 0 ? len * ffs_bytespersample(ffs) : -1;
}

static int fbm2pixfmt(int fbm)
{
	switch (fbm & 0x0fff) {
	case 0x888:
		return AV_PIX_FMT_RGB32;
	case 0x565:
		return AV_PIX_FMT_RGB565;
	case 0x233:
		return AV_PIX_FMT_RGB8;
	default:
		fprintf(stderr, "ffs: unknown fb_mode()\n");
		return AV_PIX_FMT_RGB32;
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
			pixfmt, SWS_FAST_BILINEAR,
			NULL, NULL, NULL);
	n = av_image_get_buffer_size(pixfmt, w * zoom, h * zoom, 8);
	buf = av_malloc(n * sizeof(uint8_t));
	av_image_fill_arrays(ffs->dst->data, ffs->dst->linesize, buf,
		pixfmt, w * zoom, h * zoom, 8);
}

void ffs_aconf(struct ffs *ffs)
{
	int rate, bps, ch;
	ffs_ainfo(ffs, &rate, &bps, &ch);
	ffs->swrc = swr_alloc();
	av_opt_set_int(ffs->swrc, "in_channel_layout", ffs->cc->channel_layout, 0);
	/* av_opt_set_int(ffs->swrc, "in_channel_layout", ffs->cc->ch_layout, 0); */
	av_opt_set_int(ffs->swrc, "in_sample_rate", ffs->cc->sample_rate, 0);
	av_opt_set_sample_fmt(ffs->swrc, "in_sample_fmt", ffs->cc->sample_fmt, 0);
	av_opt_set_int(ffs->swrc, "out_channel_layout", FFS_CHLAYOUT, 0);
	av_opt_set_int(ffs->swrc, "out_sample_rate", rate, 0);
	av_opt_set_sample_fmt(ffs->swrc, "out_sample_fmt", FFS_SAMPLEFMT, 0);
	swr_init(ffs->swrc);
}

void ffs_globinit(void)
{
}

long ffs_duration(struct ffs *ffs)
{
	if (ffs->st->duration != AV_NOPTS_VALUE)
		return ffs->st->duration * av_q2d(ffs->st->time_base) * 1000;
	if (ffs->fc->duration > 0)
		return ffs->fc->duration / (AV_TIME_BASE / 1000);
	return 0;
}
