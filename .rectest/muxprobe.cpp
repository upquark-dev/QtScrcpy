// 验证新编译 FFmpeg：mp4/mov muxer 对 AAC 音频轨的支持（AAC-LC 48kHz stereo）
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include <cstdio>
#include <cstring>

static int tryMux(const char *fmtName, const char *file) {
    AVFormatContext *ctx = nullptr;
    avformat_alloc_output_context2(&ctx, nullptr, nullptr, file);
    if (!ctx) { printf("  alloc fail\n"); return 1; }
    AVStream *vs = avformat_new_stream(ctx, nullptr);
    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id = AV_CODEC_ID_H264;
    vs->codecpar->width = 720; vs->codecpar->height = 1600;
    vs->time_base = AVRational{1, 1000};
    AVStream *as = avformat_new_stream(ctx, nullptr);
    as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    as->codecpar->codec_id = AV_CODEC_ID_AAC;
    as->codecpar->sample_rate = 48000;
    as->codecpar->channels = 2;
    as->codecpar->profile = FF_PROFILE_AAC_LOW;
    uint8_t asc[2] = { 0x12, 0x10 }; // AAC-LC, 48kHz, 2ch
    as->codecpar->extradata = (uint8_t *)av_malloc(2);
    memcpy(as->codecpar->extradata, asc, 2);
    as->codecpar->extradata_size = 2;
    as->time_base = AVRational{1, 48000};
    int ret = avio_open(&ctx->pb, file, AVIO_FLAG_WRITE);
    if (ret < 0) { printf("  avio_open fail %d\n", ret); return 1; }
    ret = avformat_write_header(ctx, nullptr);
    if (ret < 0) {
        char buf[128] = {0}; av_strerror(ret, buf, 127);
        printf("  [%s] write_header FAIL: %s\n", fmtName, buf);
        avio_closep(&ctx->pb); avformat_free_context(ctx);
        return 1;
    }
    // 50 个静音 AAC 帧（伪数据，仅验证封装）
    for (int i = 0; i < 50; i++) {
        AVPacket *p = av_packet_alloc();
        av_new_packet(p, 16);
        p->stream_index = as->index;
        p->pts = i * 1024LL; p->dts = p->pts; p->duration = 1024;
        av_packet_rescale_ts(p, AVRational{1, 48000}, as->time_base);
        if (av_write_frame(ctx, p) < 0) { printf("  write frame fail\n"); }
        av_packet_free(&p);
    }
    av_write_trailer(ctx);
    avio_closep(&ctx->pb);
    printf("  [%s] OK -> %s (audio codec_tag: %08x)\n",
           fmtName, file, as->codecpar->codec_tag);
    avformat_free_context(ctx);
    return 0;
}

int main() {
    // 先确认 aac encoder 在（定制构建的核心目标）
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    printf("aac encoder: %s\n", enc ? enc->name : "MISSING");
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    printf("aac decoder: %s\n", dec ? dec->name : "MISSING");
    const AVCodec *h264dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    printf("h264 decoder: %s\n", h264dec ? h264dec->name : "MISSING");
    tryMux("mp4", "E:/QtScrcpy/.rectest/muxtest.mp4");
    tryMux("mov", "E:/QtScrcpy/.rectest/muxtest.mov");
    return 0;
}
