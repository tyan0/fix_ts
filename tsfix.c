/*
 * tsfix — reconstruct H.264 / HEVC PTS/DTS from the POC in the bitstream.
 *
 * Two uses, handled by the same mechanism:
 *
 *   1. Repairing broken timestamps.
 *      Because of an ffmpeg bug (Red Hat Bugzilla #2483137), a build with
 *      the H.264 / HEVC decoders disabled (ffmpeg-free and friends) cannot
 *      set has_b_frames correctly in libavformat/demux.c and therefore fails
 *      to reorder B frames.  Running `-c copy` in that state overwrites the
 *      output PTS with the value of the preceding frame, so timestamps end
 *      up duplicated and the display order is lost.
 *
 *   2. Adding timestamps to a raw elementary stream.
 *      A bare Annex B stream carries no timestamps at all, so correct
 *      PTS/DTS have to be computed when putting it into a container.  The
 *      frame rate is supplied with --fps.
 *
 * Both come down to the same thing: once the display order is known, the
 * timestamps can be computed.  H.264 / HEVC bitstreams carry the POC
 * (Picture Order Count), which *is* the display order, so it is enough to
 * read the POC with a parser - no decoding required.
 *
 *   First pass:  read the POC of every video packet and settle the display
 *                order and the reorder delay.  When the input has original
 *                timestamps, check that the intact ones match the computed
 *                values (a match is numeric evidence that the reconstruction
 *                is right).
 *   Second pass: remux, replacing the video PTS/DTS.  Neither the video
 *                payload nor the audio is altered by a single byte.
 *
 * The input may be any container libavformat can open (mkv, mp4, ts, flv,
 * raw ES, ...).  The output container is derived from the output file name
 * (or given explicitly with --format).
 *
 * Build:
 *   gcc -std=c17 -O2 -Wall -Wextra -o tsfix tsfix.c \
 *       $(pkg-config --cflags --libs libavformat libavcodec libavutil)
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

#define PROG "tsfix"

/* ------------------------------------------------------------- diagnostics */

static int verbose = 0;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, PROG ": ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

static void die_av(const char *what, int err) __attribute__((noreturn));
static void die_av(const char *what, int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(err, buf, sizeof buf);
  die("%s: %s", what, buf);
}

/* ------------------------------------------------------------- frame table */

typedef struct Frame {
  int64_t stored_pts;   /* PTS as found in the input (may be broken or absent) */
  int64_t pos;          /* position in the file, for diagnostics */
  int     poc;          /* Picture Order Count */
  int     seq;          /* index of the coded video sequence (IDR starts a new one) */
  int     display;      /* reconstructed display order number */
  char    pict_type;
} Frame;

typedef struct FrameList {
  Frame *v;
  size_t n, cap;
} FrameList;

static void fl_push(FrameList *fl, Frame f) {
  if (fl->n == fl->cap) {
    fl->cap = fl->cap ? fl->cap * 2 : 4096;
    Frame *nv = realloc(fl->v, fl->cap * sizeof *fl->v);
    if (!nv) die("out of memory");
    fl->v = nv;
  }
  fl->v[fl->n++] = f;
}

static int gcd_int(int a, int b) {
  while (b) { int t = a % b; a = b; b = t; }
  return a < 0 ? -a : a;
}

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

/* ---------------------------------------------------------- analysis result */

typedef struct Analysis {
  FrameList frames;
  AVRational frame_rate;     /* frame rate used for the reconstruction */
  AVRational calc_tb;        /* time base the timestamps are computed in */
  AVRational stream_tb;      /* time base of the input video stream */
  const char *in_format;     /* input container name */
  enum AVCodecID codec_id;
  int64_t start_pts;         /* PTS of display order 0 (anchor, in calc_tb units) */
  int poc_step;              /* POC increment; 2 for frame coded H.264 */
  int nseq;                  /* number of coded video sequences */
  int reorder_delay;         /* reorder delay, in frames */
  int broken;                /* number of PTS that disagreed with the computed value */
  int missing_pts;           /* number of frames that carried no PTS */
  int noncontiguous;         /* number of sequences whose display order is not contiguous */
  int fresh;                 /* 1 if there were no original timestamps at all (raw stream) */
  int fps_from_option;       /* whether --fps was given */
} Analysis;

/* ------------------------------------------------------- analysis (pass 1) */

/* Length prefixed (avcC / hvcC) or Annex B?  avcC / hvcC start with
 * configurationVersion = 1 while Annex B starts with 00 00 01 or
 * 00 00 00 01, so the first byte tells them apart. */
static int extradata_is_length_prefixed(const AVCodecParameters *par) {
  return par->extradata && par->extradata_size >= 5 && par->extradata[0] == 1;
}

static const char *annexb_bsf_name(enum AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "h264_mp4toannexb";
    case AV_CODEC_ID_HEVC: return "hevc_mp4toannexb";
    default: return NULL;
  }
}

static void collect_frames(const char *path, int stream_index, Analysis *an) {
  AVFormatContext *fc = NULL;
  int ret = avformat_open_input(&fc, path, NULL, NULL);
  if (ret < 0) die_av("cannot open input", ret);
  if ((ret = avformat_find_stream_info(fc, NULL)) < 0) die_av("cannot read stream information", ret);

  AVStream *st = fc->streams[stream_index];
  an->stream_tb = st->time_base;
  an->in_format = fc->iformat->name;
  an->codec_id = st->codecpar->codec_id;

  if (!annexb_bsf_name(an->codec_id))
    die("unsupported codec: %s (only H.264 and HEVC)",
        avcodec_get_name(an->codec_id));

  /* The parser only accepts Annex B, so convert if the input is length prefixed */
  AVBSFContext *bsf = NULL;
  if (extradata_is_length_prefixed(st->codecpar)) {
    const char *name = annexb_bsf_name(an->codec_id);
    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter) die("bitstream filter %s is not available in this build", name);
    if ((ret = av_bsf_alloc(filter, &bsf)) < 0) die_av("av_bsf_alloc", ret);
    if ((ret = avcodec_parameters_copy(bsf->par_in, st->codecpar)) < 0) die_av("parameters_copy", ret);
    bsf->time_base_in = st->time_base;
    if ((ret = av_bsf_init(bsf)) < 0) die_av("av_bsf_init", ret);
    if (verbose) fprintf(stderr, PROG ": applying %s (length prefixed input)\n", name);
  }

  AVCodecParserContext *ps = av_parser_init(an->codec_id);
  if (!ps)
    die("no %s parser in this build; the POC cannot be read, so nothing can be repaired",
        avcodec_get_name(an->codec_id));
  ps->flags |= PARSER_FLAG_COMPLETE_FRAMES;   /* assume one packet is one frame */

  AVCodecContext *avctx = avcodec_alloc_context3(NULL);
  if (!avctx) die("out of memory");
  avctx->codec_id = an->codec_id;

  AVPacket *pkt = av_packet_alloc();
  if (!pkt) die("out of memory");

  int seq = 0, seq_seen = 0;
  while (av_read_frame(fc, pkt) >= 0) {
    if (pkt->stream_index != stream_index) { av_packet_unref(pkt); continue; }

    const int64_t stored_pts = pkt->pts;
    const int64_t pos = pkt->pos;
    int produced = 0;

    if (bsf) {
      if ((ret = av_bsf_send_packet(bsf, pkt)) < 0) die_av("av_bsf_send_packet", ret);
      while ((ret = av_bsf_receive_packet(bsf, pkt)) >= 0) {
        uint8_t *out = NULL;
        int out_size = 0;
        av_parser_parse2(ps, avctx, &out, &out_size,
                         pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
        if (seq_seen && ps->key_frame && ps->output_picture_number == 0) { seq++; seq_seen = 0; }
        seq_seen = 1;
        fl_push(&an->frames, (Frame){
          .stored_pts = stored_pts, .pos = pos, .poc = ps->output_picture_number,
          .seq = seq, .display = -1, .pict_type = av_get_picture_type_char(ps->pict_type),
        });
        produced++;
        av_packet_unref(pkt);
      }
      if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) die_av("av_bsf_receive_packet", ret);
    } else {
      uint8_t *out = NULL;
      int out_size = 0;
      av_parser_parse2(ps, avctx, &out, &out_size,
                       pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
      /* An IDR resets the POC to 0, which is where a new sequence begins */
      if (seq_seen && ps->key_frame && ps->output_picture_number == 0) { seq++; seq_seen = 0; }
      seq_seen = 1;
      fl_push(&an->frames, (Frame){
        .stored_pts = stored_pts, .pos = pos, .poc = ps->output_picture_number,
        .seq = seq, .display = -1, .pict_type = av_get_picture_type_char(ps->pict_type),
      });
      produced++;
      av_packet_unref(pkt);
    }

    if (produced != 1)
      die("the packet at pos=%" PRId64 " split into %d; this program assumes "
          "one packet is one frame, so it stops here", pos, produced);

    if (verbose && an->frames.n % 5000 == 0)
      fprintf(stderr, "  analysing... %zu frames\n", an->frames.n);
  }

  an->nseq = seq + 1;

  av_packet_free(&pkt);
  avcodec_free_context(&avctx);
  av_parser_close(ps);
  av_bsf_free(&bsf);
  avformat_close_input(&fc);
}

/* Recover the display order from the POC and derive what the reconstruction needs */
static void resolve_display_order(Analysis *an) {
  Frame *f = an->frames.v;
  const size_t n = an->frames.n;
  if (!n) die("there is not a single video frame");

  /* Derive the POC increment from the greatest common divisor.  It is 2 for
   * frame coded H.264 and 1 for HEVC, but take it from the data rather than
   * hardcoding it. */
  int g = 0;
  for (size_t i = 0; i < n; i++) if (f[i].poc) g = gcd_int(g, f[i].poc);
  an->poc_step = g ? g : 1;

  /* Per sequence, turn the rank in POC order into the display order number */
  int64_t base = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && f[j].seq == f[i].seq) j++;   /* [i, j) is one sequence */
    const size_t cnt = j - i;

    int min_poc = f[i].poc;
    for (size_t k = i; k < j; k++) if (f[k].poc < min_poc) min_poc = f[k].poc;

    int max_idx = -1;
    for (size_t k = i; k < j; k++) {
      f[k].display = (int)(base + (f[k].poc - min_poc) / an->poc_step);
      if (f[k].display - (int)base > max_idx) max_idx = f[k].display - (int)base;
    }

    /* Check that the display order is a permutation of 0..cnt-1.  If it is
     * not, suspect dropped frames or field coding. */
    int *idx = malloc(cnt * sizeof *idx);
    if (!idx) die("out of memory");
    for (size_t k = 0; k < cnt; k++) idx[k] = f[i + k].display - (int)base;
    qsort(idx, cnt, sizeof *idx, cmp_int);
    for (size_t k = 0; k < cnt; k++)
      if (idx[k] != (int)k) { an->noncontiguous++; break; }
    free(idx);

    base += max_idx + 1;
    i = j;
  }

  /* Reorder delay: by how many frames the DTS has to run ahead to keep dts <= pts */
  an->reorder_delay = 0;
  for (size_t k = 0; k < n; k++) {
    const int d = (int)k - f[k].display;
    if (d > an->reorder_delay) an->reorder_delay = d;
  }

  for (size_t k = 0; k < n; k++)
    if (f[k].stored_pts == AV_NOPTS_VALUE) an->missing_pts++;
  an->fresh = (an->missing_pts == (int)n);
}

/* Settle the frame rate and the time base used for the computation.
 * For a raw stream r_frame_rate cannot be trusted (it sometimes picks up the
 * field rate and comes out twice too high), so --fps is mandatory there. */
static void resolve_timing(Analysis *an, AVRational fps_opt, AVRational stream_fr) {
  if (fps_opt.num) {
    an->frame_rate = fps_opt;
    an->fps_from_option = 1;
  } else if (an->fresh) {
    die("this input carries no original timestamps (raw stream).\n"
        "       The frame rate cannot be determined, so pass one, e.g. --fps 24000/1001");
  } else if (stream_fr.num && stream_fr.den) {
    an->frame_rate = stream_fr;
  } else {
    die("cannot determine the frame rate; pass one with --fps");
  }

  if (an->fresh) {
    /* There are no original values to reproduce, so use a fine time base that
     * does not round: with 1/fps.num one frame is exactly fps.den ticks. */
    an->calc_tb = (AVRational){ 1, an->frame_rate.num };
    an->start_pts = 0;
  } else {
    /* Compute in the same time base as the input so intact values come out unchanged */
    an->calc_tb = an->stream_tb;
    /* The anchor is the PTS of display order 0.  Being first in the reorder,
     * that one is not broken. */
    an->start_pts = 0;
    for (size_t k = 0; k < an->frames.n; k++) {
      if (an->frames.v[k].display == 0 && an->frames.v[k].stored_pts != AV_NOPTS_VALUE) {
        an->start_pts = an->frames.v[k].stored_pts;
        break;
      }
    }
  }
}

/* Compute the PTS a display order number should have (in calc_tb units) */
static int64_t pts_of_display(const Analysis *an, int64_t display) {
  const AVRational frame_dur = { an->frame_rate.den, an->frame_rate.num };
  return an->start_pts + av_rescale_q(display, frame_dur, an->calc_tb);
}

/* Compute the DTS a decode order position should have (in calc_tb units).
 * Running ahead by the reorder delay keeps the DTS monotonic and always
 * satisfies dts <= pts. */
static int64_t dts_of_decode_index(const Analysis *an, int64_t decode_index) {
  const AVRational frame_dur = { an->frame_rate.den, an->frame_rate.num };
  return an->start_pts + av_rescale_q(decode_index - an->reorder_delay, frame_dur, an->calc_tb);
}

/* Check the reconstruction against the PTS that were not broken */
static void verify(Analysis *an) {
  if (an->fresh) return;   /* nothing to compare against */

  const Frame *f = an->frames.v;
  int shown = 0;
  for (size_t i = 0; i < an->frames.n; i++) {
    if (f[i].stored_pts == AV_NOPTS_VALUE) continue;
    const int64_t want = pts_of_display(an, f[i].display);
    if (want != f[i].stored_pts) {
      an->broken++;
      if (verbose && shown < 20) {
        printf("  [%zu] pos=%-10" PRId64 " %c poc=%-6d display=%-6d "
               "stored=%-8" PRId64 " -> fixed=%-8" PRId64 "\n",
               i, f[i].pos, f[i].pict_type, f[i].poc, f[i].display,
               f[i].stored_pts, want);
        shown++;
      }
    }
  }
}

static void print_analysis(const Analysis *an) {
  const size_t n = an->frames.n;
  printf("Input container      : %s\n", an->in_format);
  printf("Codec                : %s\n", avcodec_get_name(an->codec_id));
  printf("Video frames         : %zu\n", n);
  printf("Frame rate           : %d/%d (%.3f fps)%s\n",
         an->frame_rate.num, an->frame_rate.den,
         (double)an->frame_rate.num / an->frame_rate.den,
         an->fps_from_option ? " [--fps]" : " [from stream]");
  printf("Computation timebase : %d/%d\n", an->calc_tb.num, an->calc_tb.den);
  printf("POC increment        : %d\n", an->poc_step);
  printf("Coded sequences      : %d\n", an->nseq);
  printf("Reorder delay        : %d frames\n", an->reorder_delay);

  if (an->fresh) {
    printf("Original timestamps  : none (raw stream) - assigning to every frame\n");
  } else {
    printf("Anchor PTS (disp 0)  : %" PRId64 "\n", an->start_pts);
    printf("PTS missing          : %d frames\n", an->missing_pts);
    printf("PTS broken           : %d frames (%.1f%%)\n",
           an->broken, n ? 100.0 * an->broken / n : 0.0);
    if (n && an->broken > (int)(n * 9 / 10))
      printf("warning: almost every frame disagrees.  The frame rate may be wrong,\n"
             "         or the material may be variable frame rate (check --fps)\n");
  }
  if (an->noncontiguous)
    printf("warning: %d sequence(s) have a non-contiguous display order"
           " (dropped frames, or field coding?)\n", an->noncontiguous);

  if (n) {
    int max_display = 0;
    for (size_t i = 0; i < n; i++)
      if (an->frames.v[i].display > max_display) max_display = an->frames.v[i].display;
    const int64_t last = pts_of_display(an, max_display);
    printf("Last PTS after fix   : %" PRId64 " (%.3f s, display %d)\n",
           last, last * av_q2d(an->calc_tb), max_display);
    printf("First DTS            : %" PRId64 " (%.3f s)\n",
           dts_of_decode_index(an, 0), dts_of_decode_index(an, 0) * av_q2d(an->calc_tb));
  }
}

/* ---------------------------------------------------------- remux (pass 2) */

/* There is no public function for copying chapters, so duplicate them here.
 * This has to be done before avformat_write_header. */
static void copy_chapters(AVFormatContext *ifc, AVFormatContext *ofc) {
  if (!ifc->nb_chapters) return;

  AVChapter **list = av_calloc(ifc->nb_chapters, sizeof *list);
  if (!list) die("out of memory");
  for (unsigned i = 0; i < ifc->nb_chapters; i++) {
    const AVChapter *in = ifc->chapters[i];
    AVChapter *out = av_mallocz(sizeof *out);
    if (!out) die("out of memory");
    out->id = in->id;
    out->time_base = in->time_base;
    out->start = in->start;
    out->end = in->end;
    av_dict_copy(&out->metadata, in->metadata, 0);
    list[i] = out;
  }
  ofc->chapters = list;
  ofc->nb_chapters = ifc->nb_chapters;
}

static void remux(const char *in_path, const char *out_path, const char *out_format,
                  int video_index, int avoid_negative_ts, const Analysis *an) {
  AVFormatContext *ifc = NULL, *ofc = NULL;
  int ret = avformat_open_input(&ifc, in_path, NULL, NULL);
  if (ret < 0) die_av("cannot open input", ret);
  if ((ret = avformat_find_stream_info(ifc, NULL)) < 0) die_av("stream information", ret);

  if ((ret = avformat_alloc_output_context2(&ofc, NULL, out_format, out_path)) < 0)
    die_av("cannot determine the output container (name one with --format)", ret);
  printf("Output container     : %s\n", ofc->oformat->name);

  if (ofc->oformat->flags & AVFMT_NOTIMESTAMPS)
    fprintf(stderr, PROG ": warning: output format %s cannot carry timestamps; "
                    "the computed values will be discarded\n", ofc->oformat->name);

  ofc->avoid_negative_ts = avoid_negative_ts;

  int *smap = av_calloc(ifc->nb_streams, sizeof *smap);
  if (!smap) die("out of memory");

  for (unsigned i = 0; i < ifc->nb_streams; i++) {
    AVStream *is = ifc->streams[i];
    const enum AVMediaType t = is->codecpar->codec_type;
    if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO &&
        t != AVMEDIA_TYPE_SUBTITLE && t != AVMEDIA_TYPE_ATTACHMENT) {
      smap[i] = -1;
      continue;
    }
    AVStream *os = avformat_new_stream(ofc, NULL);
    if (!os) die("cannot create an output stream");
    if ((ret = avcodec_parameters_copy(os->codecpar, is->codecpar)) < 0)
      die_av("avcodec_parameters_copy", ret);
    os->codecpar->codec_tag = 0;
    os->disposition = is->disposition;
    av_dict_copy(&os->metadata, is->metadata, 0);

    if ((int)i == video_index) {
      /* Ask for a time base in which one frame is exactly frame_rate.den
       * ticks, so the timestamps do not get rounded.  The muxer may pick a
       * different one (Matroska is always 1/1000), so read the value that
       * actually applies after the header has been written. */
      os->time_base = (AVRational){ 1, an->frame_rate.num };
      os->avg_frame_rate = an->frame_rate;
      os->r_frame_rate = an->frame_rate;
    } else {
      os->time_base = is->time_base;
      os->avg_frame_rate = is->avg_frame_rate;
      os->r_frame_rate = is->r_frame_rate;
    }
    smap[i] = os->index;
  }
  av_dict_copy(&ofc->metadata, ifc->metadata, 0);
  copy_chapters(ifc, ofc);

  if (!(ofc->oformat->flags & AVFMT_NOFILE))
    if ((ret = avio_open(&ofc->pb, out_path, AVIO_FLAG_WRITE)) < 0)
      die_av("cannot open the output file", ret);
  if ((ret = avformat_write_header(ofc, NULL)) < 0) die_av("cannot write the header", ret);

  /* Compute the timestamps directly in the output time base.  Rounding once
   * here avoids the double rounding of "round in the computation base, then
   * convert to the output base". */
  const AVRational frame_dur = { an->frame_rate.den, an->frame_rate.num };
  const AVRational vtb = ofc->streams[smap[video_index]]->time_base;
  const int64_t start_out = av_rescale_q(an->start_pts, an->calc_tb, vtb);
  printf("Output timebase      : %d/%d (video)\n", vtb.num, vtb.den);
  AVPacket *pkt = av_packet_alloc();
  if (!pkt) die("out of memory");

  size_t vi = 0;                 /* position in video decode order */
  int64_t other_pkts = 0;
  while (av_read_frame(ifc, pkt) >= 0) {
    const int oi = smap[pkt->stream_index];
    if (oi < 0) { av_packet_unref(pkt); continue; }

    AVStream *is = ifc->streams[pkt->stream_index];
    AVStream *os = ofc->streams[oi];

    if (pkt->stream_index == video_index) {
      if (vi >= an->frames.n)
        die("the second pass saw more video frames than the first (%zu)", an->frames.n);

      /* PTS comes from the display order, DTS from the decode order.  Running
       * ahead by the reorder delay keeps the DTS monotonic and always
       * satisfies dts <= pts. */
      pkt->pts = start_out + av_rescale_q(an->frames.v[vi].display, frame_dur, vtb);
      pkt->dts = start_out + av_rescale_q((int64_t)vi - an->reorder_delay, frame_dur, vtb);
      /* Recompute the duration only when the original cannot be trusted
       * (raw stream and the like) */
      pkt->duration = (an->fresh || pkt->duration <= 0)
                    ? av_rescale_q(1, frame_dur, vtb)
                    : av_rescale_q(pkt->duration, is->time_base, vtb);
      vi++;
    } else {
      av_packet_rescale_ts(pkt, is->time_base, os->time_base);
      other_pkts++;   /* anything that is not video is left alone */
    }

    pkt->stream_index = oi;
    pkt->pos = -1;

    if ((ret = av_interleaved_write_frame(ofc, pkt)) < 0) die_av("cannot write a packet", ret);
    av_packet_unref(pkt);
  }

  if (vi != an->frames.n)
    fprintf(stderr, PROG ": warning: the video frame count differs from the first pass "
            "(%zu != %zu)\n", vi, an->frames.n);

  if ((ret = av_write_trailer(ofc)) < 0) die_av("cannot write the trailer", ret);

  printf("Wrote                : %s\n", out_path);
  printf("  video: PTS/DTS %s for %zu frames, %" PRId64 " other packets untouched\n",
         an->fresh ? "assigned" : "reconstructed", vi, other_pkts);

  av_packet_free(&pkt);
  av_freep(&smap);
  if (ofc->pb) avio_closep(&ofc->pb);
  avformat_free_context(ofc);
  avformat_close_input(&ifc);
}

/* ------------------------------------------------------------ entry point */

static void usage(FILE *o) {
  fprintf(o,
    "usage: " PROG " [options] input [output]\n"
    "\n"
    "  Reconstructs H.264 / HEVC PTS/DTS from the POC (display order) in the\n"
    "  bitstream.  Repairs timestamps broken by an ffmpeg bug (RHBZ #2483137)\n"
    "  and assigns timestamps to a raw elementary stream that has none.\n"
    "\n"
    "  The input may be any container libavformat can open (mkv, mp4, ts, flv,\n"
    "  raw ES, ...).  The output container is derived from the output file name.\n"
    "\n"
    "options:\n"
    "  -a, --analyze          analyse only, write nothing\n"
    "  -f, --fps N/D          set the frame rate (e.g. 24000/1001).  Mandatory\n"
    "                         for a raw stream; for container input the value\n"
    "                         from the stream is used when omitted\n"
    "  -F, --format NAME      name the output container (e.g. matroska, mp4, mpegts)\n"
    "      --avoid-negative-ts MODE\n"
    "                         auto (default) / disabled / make_zero / make_non_negative\n"
    "                         The first DTS is negative by the reorder delay.  With\n"
    "                         auto, libavformat shifts everything only for formats\n"
    "                         that cannot represent negatives (FLV and such);\n"
    "                         disabled writes the negatives as they are\n"
    "  -v, --verbose          list the frames that were broken, one per line\n"
    "  -h, --help             this text\n"
    "\n"
    "examples:\n"
    "  " PROG " -a broken.mkv                        # analyse only\n"
    "  " PROG " broken.mkv fixed.mkv                 # repair a broken MKV\n"
    "  " PROG " broken.mp4 fixed.mp4                 # the same for MP4\n"
    "  " PROG " -f 24000/1001 raw.h264 out.mkv       # timestamp a raw H.264 stream\n"
    "  " PROG " -f 30000/1001 raw.hevc out.mp4       # raw HEVC into MP4\n");
}

static int parse_rational(const char *s, AVRational *out) {
  int num = 0, den = 0;
  if (sscanf(s, "%d/%d", &num, &den) == 2 && num > 0 && den > 0) {
    *out = (AVRational){ num, den };
    return 0;
  }
  double d = 0;
  if (sscanf(s, "%lf", &d) == 1 && d > 0) {
    *out = av_d2q(d, 1 << 24);
    return 0;
  }
  return -1;
}

static int parse_avoid_negative_ts(const char *s) {
  if (!strcmp(s, "disabled")) return AVFMT_AVOID_NEG_TS_DISABLED;
  if (!strcmp(s, "auto")) return AVFMT_AVOID_NEG_TS_AUTO;
  if (!strcmp(s, "make_zero")) return AVFMT_AVOID_NEG_TS_MAKE_ZERO;
  if (!strcmp(s, "make_non_negative")) return AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
  die("cannot parse --avoid-negative-ts: %s", s);
}

int main(int argc, char **argv) {
  const char *in_path = NULL, *out_path = NULL, *out_format = NULL;
  int analyze_only = 0;
  /* auto by default.  The first DTS is negative by the reorder delay, so for
   * output formats that cannot represent negative timestamps (FLV, where a
   * 32 bit millisecond field overflows and breaks) libavformat is left to
   * shift every stream uniformly.  Matroska and MP4 can write negatives as
   * they are, so auto reproduces the original values there (measured). */
  int avoid_negative_ts = AVFMT_AVOID_NEG_TS_AUTO;
  AVRational fps_opt = { 0, 0 };

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    else if (!strcmp(a, "-a") || !strcmp(a, "--analyze")) analyze_only = 1;
    else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) verbose = 1;
    else if (!strcmp(a, "-f") || !strcmp(a, "--fps")) {
      if (++i >= argc) die("--fps needs a value");
      if (parse_rational(argv[i], &fps_opt) < 0) die("cannot parse --fps: %s", argv[i]);
    }
    else if (!strcmp(a, "-F") || !strcmp(a, "--format")) {
      if (++i >= argc) die("--format needs a value");
      out_format = argv[i];
    }
    else if (!strcmp(a, "--avoid-negative-ts")) {
      if (++i >= argc) die("--avoid-negative-ts needs a value");
      avoid_negative_ts = parse_avoid_negative_ts(argv[i]);
    }
    else if (a[0] == '-' && a[1]) { usage(stderr); die("unknown option: %s", a); }
    else if (!in_path) in_path = a;
    else if (!out_path) out_path = a;
    else die("too many arguments: %s", a);
  }
  if (!in_path) { usage(stderr); return 2; }
  if (!analyze_only && !out_path) die("name an output file (or pass -a to analyse only)");

  av_log_set_level(verbose ? AV_LOG_WARNING : AV_LOG_ERROR);

  /* Identify the video stream and remember the frame rate the stream reports */
  int video_index;
  AVRational stream_fr = { 0, 0 };
  {
    AVFormatContext *fc = NULL;
    int ret = avformat_open_input(&fc, in_path, NULL, NULL);
    if (ret < 0) die_av("cannot open input", ret);
    if ((ret = avformat_find_stream_info(fc, NULL)) < 0) die_av("stream information", ret);
    video_index = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index < 0) die("there is no video stream");
    AVStream *st = fc->streams[video_index];
    stream_fr = st->r_frame_rate.num ? st->r_frame_rate : st->avg_frame_rate;
    avformat_close_input(&fc);
  }

  Analysis an = { 0 };
  printf("== Analysis ==\n");
  collect_frames(in_path, video_index, &an);
  resolve_display_order(&an);
  resolve_timing(&an, fps_opt, stream_fr);
  verify(&an);
  print_analysis(&an);

  if (analyze_only) { free(an.frames.v); return an.broken ? 1 : 0; }

  printf("\n== Writing ==\n");
  remux(in_path, out_path, out_format, video_index, avoid_negative_ts, &an);

  free(an.frames.v);
  return 0;
}
