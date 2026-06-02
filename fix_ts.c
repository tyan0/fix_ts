#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int index;
	int display_rank;
} FrameMap;

static int cmp_frame(const void *a, const void *b) {
	return ((FrameMap*)a)->display_rank - ((FrameMap*)b)->display_rank;
}

#define MAX_REORDER_DELAY 16

void cleanup(AVFormatContext *ifmt, AVFormatContext *ofmt,
		AVCodecParserContext *parser, AVCodecContext *codec_ctx,
		FrameMap *map, FrameMap *sorted) {
	if (parser) av_parser_close(parser);
	if (codec_ctx) avcodec_free_context(&codec_ctx);
	if (ifmt) avformat_close_input(&ifmt);
	if (ofmt) {
		if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
		avformat_free_context(ofmt);
	}
	if (map) free(map);
	if (sorted) free(sorted);
}

int main(int argc, char **argv) {
	int ret = 1;
	if (argc < 3) {
		fprintf(stderr, "Usage: %s in.mkv out.mkv [m/n]\n"
				"\tm/n: frame_rate "
				"(e.g. 30000/1001: 30fps in NTSC)\n", argv[0]);
		return 1;
	}

	AVFormatContext *ifmt = NULL, *ofmt = NULL;
	AVCodecParserContext *parser = NULL;
	AVCodecContext *codec_ctx = NULL;
	FrameMap *map = NULL, *sorted = NULL;
	int frame_count = 0, frame_cap = 10000;

	if (avformat_open_input(&ifmt, argv[1], NULL, NULL) < 0) {
		fprintf(stderr, "avformat_open_input() failed.\n");
		goto end;
	}
	if (avformat_find_stream_info(ifmt, NULL) < 0) {
		fprintf(stderr, "avformat_find_stream_info() failed.\n");
		goto end;
	}

	int video_index = -1;
	for (unsigned i = 0; i < ifmt->nb_streams; i++)
		if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			video_index = i;

	parser = av_parser_init(AV_CODEC_ID_H264);
	codec_ctx = avcodec_alloc_context3(NULL);
	avcodec_parameters_to_context(codec_ctx,
			ifmt->streams[video_index]->codecpar);

	map = malloc(frame_cap * sizeof(FrameMap));
	AVPacket pkt;
	uint64_t pict_num_msb = 0;
	int prev_pict_num = -1;
	int max_pict_num = 0;

	while (av_read_frame(ifmt, &pkt) >= 0) {
		if (pkt.stream_index == video_index) {
			if (frame_count >= frame_cap) {
				frame_cap *= 2;
				map = realloc(map, frame_cap * sizeof(FrameMap));
			}
			uint8_t *data; int size;
			do {
				int len = av_parser_parse2(parser, codec_ctx, &data, &size,
						pkt.data, pkt.size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
				pkt.data += len;
				pkt.size -= len;
			} while (!size);
			if (parser->output_picture_number > max_pict_num)
				max_pict_num = parser->output_picture_number;
			if (prev_pict_num >= 0 &&
					parser->output_picture_number - prev_pict_num
					< -MAX_REORDER_DELAY) {
				pict_num_msb += max_pict_num + 1;
				max_pict_num = 0;
			}
			map[frame_count] = (FrameMap){frame_count,
				parser->output_picture_number + pict_num_msb};
			prev_pict_num = parser->output_picture_number;
			frame_count++;
		}
		av_packet_unref(&pkt);
	}

	sorted = malloc(frame_count * sizeof(FrameMap));
	memcpy(sorted, map, frame_count * sizeof(FrameMap));
	qsort(sorted, frame_count, sizeof(FrameMap), cmp_frame);
	for (int i = 0; i < frame_count; i++)
		map[sorted[i].index].display_rank = i;

	if (avformat_alloc_output_context2(&ofmt, NULL, NULL, argv[2]) != 0) {
		fprintf(stderr, "avformat_alloc_output_context2() failed.\n");
		goto end;
	}
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *out_st = avformat_new_stream(ofmt, NULL);
		avcodec_parameters_copy(out_st->codecpar, ifmt->streams[i]->codecpar);
		if (strstr(ifmt->iformat->name, ofmt->oformat->name) == NULL)
			out_st->codecpar->codec_tag = 0;
	}

	AVStream *in_st = ifmt->streams[video_index];

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
		avio_open(&ofmt->pb, argv[2], AVIO_FLAG_WRITE);
	if (avformat_write_header(ofmt, NULL) < 0) {
		fprintf(stderr, "avformat_write_header() failed.\n");
		goto end;
	}

	AVRational out_tb = ofmt->streams[video_index]->time_base;

	int num, den;
	AVRational frame_rate;
	if (argc >= 4 && sscanf(argv[3], "%d/%d", &num, &den) == 2) {
		frame_rate = (AVRational){num, den};
	} else {
		frame_rate = in_st->avg_frame_rate;
		if (frame_rate.num == 0)
			frame_rate = in_st->r_frame_rate;
	}

	if (av_seek_frame(ifmt, video_index, 0, AVSEEK_FLAG_BACKWARD) <= 0)
		av_seek_frame(ifmt, video_index, 0, AVSEEK_FLAG_BYTE);
	int current = 0;
	while (av_read_frame(ifmt, &pkt) >= 0) {
		if (pkt.stream_index == video_index) {
			pkt.pts = av_rescale_q(map[current].display_rank,
					av_inv_q(frame_rate), out_tb);
			pkt.dts = av_rescale_q(current - MAX_REORDER_DELAY,
					av_inv_q(frame_rate), out_tb);
			pkt.duration = av_rescale_q(1, av_inv_q(frame_rate), out_tb);
			current++;
		}
		if (ifmt->streams[pkt.stream_index]->codecpar->codec_type
				== AVMEDIA_TYPE_AUDIO)
			av_packet_rescale_ts(&pkt,
					ifmt->streams[pkt.stream_index]->time_base,
					ofmt->streams[pkt.stream_index]->time_base);
		av_interleaved_write_frame(ofmt, &pkt);
		av_packet_unref(&pkt);
	}
	av_write_trailer(ofmt);
	ret = 0;

end:
	cleanup(ifmt, ofmt, parser, codec_ctx, map, sorted);
	return ret;
}
