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

struct ffs *ffs_alloc(char *path, int video)
{
	struct ffs *ffs;
	int codec_type = video ? CODEC_TYPE_VIDEO : CODEC_TYPE_AUDIO;
	int i;
	ffs = malloc(sizeof(*ffs));
	memset(ffs, 0, sizeof(*ffs));
	ffs->si = -1;
	if (av_open_input_file(&ffs->fc, path, NULL, 0, NULL))
		goto failed;
	if (av_find_stream_info(ffs->fc) < 0)
		goto failed;
	for (i = 0; i < ffs->fc->nb_streams; i++)
		if (ffs->fc->streams[i]->codec->codec_type == codec_type)
			ffs->si = i;
	if (ffs->si == -1)
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

static void wait(long ts, int vdelay)
{
	int nts = ts_ms();
	if (ts + vdelay > nts)
		usleep((ts + vdelay - nts) * 1000);
}

void ffs_wait(struct ffs *ffs)
{
	long ts = ts_ms();
	if (ts && ffs->ts) {
		AVRational *r = &ffs->fc->streams[ffs->si]->time_base;
		int vdelay = 1000 * r->num / r->den;
		wait(ffs->ts, vdelay < 20 ? 20 : vdelay);
	}
	ffs->ts = ts_ms();
}

long ffs_pos(struct ffs *ffs, int diff)
{
	return (ffs->si << 28) | (ffs->seq + diff);
}

void ffs_seek(struct ffs *ffs, long pos, int perframe)
{
	long idx = pos >> 28;
	long seq = pos & 0x0fffffff;
	av_seek_frame(ffs->fc, idx, seq * perframe,
			perframe == 1 ? AVSEEK_FLAG_FRAME : 0);
	ffs->seq = seq;
	ffs->seq_all = 0;
	ffs->seq_cur = 0;
	ffs->ts = 0;
}

long ffs_seq(struct ffs *ffs, int all)
{
	return all ? ffs->seq_all : ffs->seq_cur;
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

int ffs_vdec(struct ffs *ffs, char **buf)
{
	AVCodecContext *vcc = ffs->cc;
	AVPacket *pkt = ffs_pkt(ffs);
	int fine = 0;
	if (!pkt)
		return -1;
	avcodec_decode_video2(vcc, ffs->tmp, &fine, pkt);
	av_free_packet(pkt);
	if (fine && buf) {
		sws_scale(ffs->swsc, ffs->tmp->data, ffs->tmp->linesize,
			  0, vcc->height, ffs->dst->data, ffs->dst->linesize);
		*buf = (void *) ffs->dst->data[0];
		return ffs->dst->linesize[0];
	}
	return 0;
}

int ffs_adec(struct ffs *ffs, char *buf, int blen)
{
	int rdec = 0;
	AVPacket tmppkt;
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

void ffs_vsetup(struct ffs *ffs, float zoom, int pixfmt)
{
	int h = ffs->cc->height;
	int w = ffs->cc->width;
	int fmt = ffs->cc->pix_fmt;
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
}
