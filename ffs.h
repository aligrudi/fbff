void ffs_globinit(void);

/* ffmpeg stream */
struct ffs *ffs_alloc(char *path, int video);
void ffs_free(struct ffs *ffs);

long ffs_pos(struct ffs *ffs, int diff);
void ffs_seek(struct ffs *ffs, long pos, int perframe);
void ffs_wait(struct ffs *ffs);
int ffs_vsync(struct ffs *ffs, struct ffs *affs, int abufs);

/* audio */
void ffs_ainfo(struct ffs *ffs, int *rate, int *bps, int *ch);
int ffs_adec(struct ffs *ffs, void *buf, int blen);

/* video */
void ffs_vsetup(struct ffs *ffs, float zoom, int fbm);
void ffs_vinfo(struct ffs *ffs, int *w, int *h);
int ffs_vdec(struct ffs *ffs, void **buf);
