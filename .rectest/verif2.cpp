// mp4 结构解析 + H264 实际解码验证（不依赖 mov demuxer）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>

static FILE *g_f;
static std::vector<uint8_t> g_data;

static uint32_t rd32(size_t off) {
    return (uint32_t)g_data[off] << 24 | (uint32_t)g_data[off+1] << 16
         | (uint32_t)g_data[off+2] << 8 | g_data[off+3];
}

// 在 [start,end) 里找 box 类型，返回 box 内容起始偏移（跳过 8 字节头）
static long findBox(size_t start, size_t end, const char *type, size_t *bodyEnd = nullptr) {
    size_t off = start;
    while (off + 8 <= end) {
        uint32_t size = rd32(off);
        if (size < 8 || off + size > end) return -1;
        if (!memcmp(g_data.data() + off + 4, type, 4)) {
            if (bodyEnd) *bodyEnd = off + size;
            return (long)(off + 8);
        }
        off += size;
    }
    return -1;
}

int main(int argc, char **argv) {
    av_log_set_level(AV_LOG_ERROR);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("open fail\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    g_data.resize(sz);
    fread(g_data.data(), 1, sz, f);
    fclose(f);
    printf("file size=%ld\n", sz);

    long moov = findBox(0, sz, "moov");
    if (moov < 0) { printf("FAIL no moov\n"); return 1; }
    long trak = findBox(moov, sz, "trak");
    long mdia = findBox(trak, sz, "mdia");
    long minf = findBox(mdia, sz, "minf");
    long stbl = findBox(minf, sz, "stbl");
    size_t stblEnd = sz;
    if (stbl < 0) { printf("FAIL no stbl\n"); return 1; }

    // stsd → avc1 → avcC（extradata，annexb 已转 avcC）
    long stsd = findBox(stbl, stblEnd, "stsd");
    long avc1 = findBox(stsd + 8, stsd + rd32(stsd - 4), "avc1");
    long avcC = findBox(avc1 + 86, avc1 + rd32(avc1 - 4), "avcC");
    if (avcC < 0) { printf("FAIL no avcC\n"); return 1; }
    size_t avcCSize = rd32(avcC - 4);
    const uint8_t *extradata = g_data.data() + avcC;
    int nalLenSize = (extradata[4] & 3) + 1;
    printf("avcC size=%zu nalLenSize=%d profile=%d\n", avcCSize, nalLenSize, extradata[5]);

    // stsz：sample 数与大小表
    long stsz = findBox(stbl, stblEnd, "stsz");
    uint32_t sampleCount = rd32(stsz + 12);
    printf("samples=%u\n", sampleCount);

    // stco：chunk 偏移（单 chunk 场景直接用第一个）
    long stco = findBox(stbl, stblEnd, "stco");
    uint32_t chunkCount = rd32(stco + 8);
    uint64_t chunkOff = ((uint64_t)rd32(stco + 12) << 32) | rd32(stco + 16);
    printf("chunks=%u firstOffset=%llu\n", chunkCount, (unsigned long long)chunkOff);

    // stss：关键帧 sample 序号（1-based）
    long stss = findBox(stbl, stblEnd, "stss");
    std::vector<uint32_t> sync;
    if (stss >= 0) {
        uint32_t n = rd32(stss + 4);
        for (uint32_t i = 0; i < n; i++) sync.push_back(rd32(stss + 8 + i * 4));
    }
    printf("sync samples=%zu\n", sync.size());

    // 切分 samples 并转 annexb
    std::vector<std::vector<uint8_t>> annexb;
    uint64_t pos = chunkOff;
    for (uint32_t i = 0; i < sampleCount; i++) {
        uint32_t sSize = rd32(stsz + 16 + i * 4);
        if (pos + sSize > (uint64_t)sz) { printf("FAIL sample %u overruns file\n", i); return 1; }
        annexb.emplace_back();
        std::vector<uint8_t> &out = annexb.back();
        const uint8_t sc[] = {0, 0, 0, 1};
        uint64_t p = pos;
        uint64_t end = pos + sSize;
        while (p + nalLenSize <= end) {
            uint32_t nalLen = 0;
            for (int b = 0; b < nalLenSize; b++) nalLen = (nalLen << 8) | g_data[p + b];
            p += nalLenSize;
            if (p + nalLen > end) { printf("FAIL nal overruns sample %u\n", i); return 1; }
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), g_data.data() + p, g_data.data() + p + nalLen);
            p += nalLen;
        }
        pos += sSize;
    }
    printf("converted %zu samples to annexb, total bytes=%zu\n", annexb.size(),
           [&]{ size_t t = 0; for (auto &v : annexb) t += v.size(); return t; }());

    // avcC 的 SPS/PPS 也补进流头部
    std::vector<uint8_t> header;
    {
        const uint8_t sc[] = {0, 0, 0, 1};
        int pos2 = 8;  // skip version..numOfSequence
        int spsN = extradata[5] & 0x1f;
        for (int i = 0; i < spsN; i++) {
            int l = (extradata[pos2] << 8) | extradata[pos2 + 1];
            header.insert(header.end(), sc, sc + 4);
            header.insert(header.end(), extradata + pos2 + 2, extradata + pos2 + 2 + l);
            pos2 += 2 + l;
        }
        int ppsN = extradata[pos2]; pos2++;
        for (int i = 0; i < ppsN; i++) {
            int l = (extradata[pos2] << 8) | extradata[pos2 + 1];
            header.insert(header.end(), sc, sc + 4);
            header.insert(header.end(), extradata + pos2 + 2, extradata + pos2 + 2 + l);
            pos2 += 2 + l;
        }
        printf("header(SPS/PPS) bytes=%zu\n", header.size());
    }

    // 用工程自带 H264 解码器实际解码
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_malloc(avcCSize);
    memcpy(ctx->extradata, extradata, avcCSize);
    ctx->extradata_size = avcCSize;
    avcodec_open2(ctx, codec, nullptr);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int decoded = 0, decErr = 0;
    auto sendAll = [&](const std::vector<uint8_t> &buf) {
        av_new_packet(pkt, (int)buf.size());
        memcpy(pkt->data, buf.data(), buf.size());
        pkt->pts = decErr + decoded; pkt->dts = pkt->pts;
        int r = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (r < 0) decErr++;
    };
    sendAll(header);
    for (auto &v : annexb) sendAll(v);
    avcodec_send_packet(ctx, nullptr);  // flush
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        if (decoded == 0) printf("first frame %dx%d fmt=%d\n", frame->width, frame->height, frame->format);
        decoded++;
        av_frame_unref(frame);
    }
    printf("decoded frames=%d sendErrors=%d\n", decoded, decErr);
    bool ok = decoded >= 20 && decErr == 0 && sync.size() >= 1;
    printf("%s\n", ok ? "VERIFY PASS" : "VERIFY FAIL");
    return ok ? 0 : 1;
}
