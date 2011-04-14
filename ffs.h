void ffs_globinit(void);

/* ffmpeg stream */
struct ffs *ffs_alloc(char *path, int video);
void ffs_free(struct ffs *ffs);

long ffs_pos(struct ffs *ffs, int diff);
void ffs_seek(struct ffs *ffs, long pos, int perframe);
long ffs_seq(struct ffs *ffs, int all);
void ffs_wait(struct ffs *ffs);

/* audio */
void ffs_ainfo(struct ffs *ffs, int *rate, int *bps, int *ch);
int ffs_adec(struct ffs *ffs, char *buf, int blen);

/* video */
void ffs_vsetup(struct ffs *ffs, float zoom, int fbm);
void ffs_vinfo(struct ffs *ffs, int *w, int *h);
int ffs_vdec(struct ffs *ffs, char **buf);
