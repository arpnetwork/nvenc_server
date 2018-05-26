#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define GOP         3
#define MAX_THREADS 2
#define ADDR        "ipc:///tmp/nvenc.sock"

typedef struct {
  int fd;
  int width;
  int height;
} TSContext;

static void *nvenc_thread(void *opaque);
static int parse_frame(uint8_t *data, int width, int height, AVFrame **frame);
static int copy_frame(AVFrame *frame, const uint8_t *src);

static AVCodecContext *open_h264_nv_encoder(int width, int height, int quality);

int main(int argc, const char *argv[])
{
  int i = 0;
  TSContext ctx[1];
  pthread_t tids[MAX_THREADS];
  int rc = 0;

  if (argc < 3)
  {
    fprintf(stderr, "Usage: nvenc_srv <WIDTH> <HEIGHT>\n");
    return -1;
  }

  ctx->width = atoi(argv[1]);
  ctx->height = atoi(argv[2]);
  if (ctx->width <= 0 || ctx->height <= 0)
  {
    fprintf(stderr, "Invalid video size (%dx%d).\n", ctx->width, ctx->height);
    return -1;
  }

  ctx->fd = nn_socket(AF_SP_RAW, NN_REP); assert(ctx->fd >= 0);
  if (nn_bind(ctx->fd, ADDR) < 0)
  {
    fprintf(stderr, "nn_bind: %s\n", nn_strerror(nn_errno()));
    nn_close(ctx->fd);
    return -1;
  }

  fprintf(stderr, "Start server on %s. size=%dx%d\n", ADDR, ctx->width, ctx->height);

  av_register_all();

  memset(tids, 0, sizeof (tids));

  /*  Start up the threads. */
  for (i = 0; i < MAX_THREADS; i++)
  {
    rc = pthread_create(&tids[i], NULL, nvenc_thread, ctx); assert(rc >= 0);
  }

  /*  Now wait on them to finish. */
  for (i = 0; i < MAX_THREADS; i++)
  {
    pthread_join(tids[i], NULL);
  }

  return 0;
}

/* A thread handler that encoding H.264 frame with NVEnc codec. */
void *nvenc_thread(void *opaque)
{
  TSContext *ctx = (TSContext *) opaque;

  int rc = -1;
  int i = 0;
  int off = 0;
  int size = 0;
  uint8_t *data = NULL;
  void *control = NULL;
  struct nn_iovec iov[GOP * 3];
  struct nn_msghdr hdr;
  AVFrame *frame = NULL;
  AVPacket pkt[3];

  AVCodecContext *codec = open_h264_nv_encoder(ctx->width, ctx->height, 0);

  for (;;)
  {
    data = NULL;
    control = NULL;
    memset(&iov, 0, sizeof (iov));
    memset(&hdr, 0, sizeof (hdr));
    iov[0].iov_base = &data;
    iov[0].iov_len = NN_MSG;
    hdr.msg_iov = iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &control;
    hdr.msg_controllen = NN_MSG;

    rc = nn_recvmsg(ctx->fd, &hdr, 0);
    if (rc < 0)
    {
      if (nn_errno() == EBADF)
      {
        return NULL;  /* Socket closed by another thread. */
      }

      /*  Any error here is unexpected. */
      fprintf(stderr, "nn_recv: %s\n", nn_strerror(nn_errno()));
      break;
    }

    size = rc;
    for (i = 0, off = 0; off < size; i++)
    {
      av_init_packet(&pkt[i]);

      off += parse_frame(data + off, ctx->width, ctx->height, &frame);
      if (i == 0)
      {
        frame->pict_type = AV_PICTURE_TYPE_I;
      }
      rc = avcodec_send_frame(codec, frame); assert(rc >= 0);
      rc = avcodec_receive_packet(codec, &pkt[i]); assert(rc >= 0);
      iov[i * 3].iov_base = &pkt[i].flags;
      iov[i * 3].iov_len = sizeof (pkt[i].flags);
      iov[i * 3 + 1].iov_base = &pkt[i].size;
      iov[i * 3 + 1].iov_len = sizeof (pkt[i].size);
      iov[i * 3 + 2].iov_base = pkt[i].data;
      iov[i * 3 + 2].iov_len = pkt[i].size;

      av_frame_free(&frame);
    }
    nn_freemsg(data); data = NULL;

    hdr.msg_iovlen = i * 3;

    rc = nn_sendmsg(ctx->fd, &hdr, 0);
    if (rc < 0)
    {
      fprintf(stderr, "nn_send: %s\n", nn_strerror(nn_errno()));
      nn_freemsg(control);
    }
    control = NULL;

    for (; i > 0; i--)
    {
      av_packet_unref(&pkt[i - 1]);
    }
  }

  nn_close(ctx->fd);

  return NULL;
}

/* Copy frame data from buffer to `AVFrame` struct with given `width` and `height`. */
int parse_frame(uint8_t *data, int width, int height, AVFrame **frame)
{
  int size = 0;

  *frame = av_frame_alloc();

  (*frame)->width = width;
  (*frame)->height = height;
  (*frame)->format = AV_PIX_FMT_YUV420P;
  return copy_frame(*frame, data);
}

/* Copy frame data from buffer to `AVFrame` struct. */
int copy_frame(AVFrame *frame, const uint8_t *src)
{
  av_frame_get_buffer(frame, 4);

  uint8_t *src_data[4];
  int src_linesize[4];
  av_image_fill_arrays(src_data, src_linesize, src,
                       frame->format, frame->width, frame->height, 4);
  av_image_copy(frame->data, frame->linesize,
                (const uint8_t **) src_data, src_linesize,
                frame->format, frame->width, frame->height);

  return av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
}

/* Open H.264 NVEnc encoder with given parameters */
AVCodecContext *open_h264_nv_encoder(int width, int height, int quality)
{
  AVCodec *codec = NULL;
  AVCodecContext *ctx = NULL;
  int ret = 0;

  codec = avcodec_find_encoder_by_name("h264_nvenc"); assert(codec != NULL);
  ctx = avcodec_alloc_context3(codec); assert(ctx != NULL);

  ctx->width = width;
  ctx->height = height;
  ctx->gop_size = GOP;
  ctx->qmin = 20;
  ctx->qmax = 35;
  ctx->max_b_frames = 0;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->framerate = av_make_q(1, 16);
  ctx->time_base = av_mul_q(ctx->framerate, av_make_q(1, 1000));
  ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  av_opt_set(ctx->priv_data, "profile", "high", 0);
  av_opt_set(ctx->priv_data, "preset", "slow", 0);
  av_opt_set(ctx->priv_data, "delay", "0", 0);

  // TODO: use quality
  (void) quality;

  ret = avcodec_open2(ctx, codec, NULL); assert(ret >= 0);

  return ctx;
}
