#define FFS_AUDIO	0x1000
#define FFS_VIDEO	0x2000
#define FFS_SUBTS	0x4000
#define FFS_STRIDX	0x0fff

void ffs_globinit(void);

/* ffmpeg stream */
struct ffs *ffs_alloc(char *path, int flags);
void ffs_free(struct ffs *ffs);

long ffs_pos(struct ffs *ffs);
long ffs_duration(struct ffs *ffs);
void ffs_seek(struct ffs *ffs, struct ffs *vffs, long pos);
void ffs_wait(struct ffs *ffs);
int ffs_avdiff(struct ffs *ffs, struct ffs *affs);

/* audio */
void ffs_aconf(struct ffs *ffs);
void ffs_ainfo(struct ffs *ffs, int *rate, int *bps, int *ch);
int ffs_adec(struct ffs *ffs, void *buf, int blen);

/* video */
void ffs_vconf(struct ffs *ffs, float zoom, int fbm);
void ffs_vinfo(struct ffs *ffs, int *w, int *h);
int ffs_vdec(struct ffs *ffs, void **buf);

/* subtitles */
int ffs_sdec(struct ffs *ffs, char *buf, int blen, long *beg, long *end);
