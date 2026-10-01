import sys

p = 'E:/QtScrcpy/.rectest/rectest.cpp'
src = open(p, encoding='utf-8').read()

start = src.index('static bool verifyMp4')
end = src.index('int main(int argc, char *argv[])')

new_fn = '''// 从 moov 里找第 idx 个 trak 的内容起点（0-based）
static long findTrak(long moov, size_t sz, int idx, size_t *trakEnd)
{
    long off = moov;
    int seen = 0;
    while (off + 8 <= (long)sz) {
        uint32_t size = rd32(off);
        if (size < 8 || off + size > (long)sz) {
            return -1;
        }
        if (!memcmp(g_data.data() + off + 4, "trak", 4)) {
            if (seen == idx) {
                if (trakEnd) *trakEnd = off + size;
                return off + 8;
            }
            seen++;
        }
        off += size;
    }
    return -1;
}

// trak -> mdia -> hdlr 内容 +8 处是 handler_type（vide/soun）
static char trackKind(long trak, size_t sz)
{
    long mdia = findBox(trak, sz, "mdia");
    long hdlr = mdia >= 0 ? findBox(mdia, sz, "hdlr") : -1;
    if (hdlr < 0 || hdlr + 12 > (long)sz) {
        return '?';
    }
    const uint8_t *t = g_data.data() + hdlr + 8;
    if (!memcmp(t, "vide", 4)) return 'v';
    if (!memcmp(t, "soun", 4)) return 'a';
    return '?';
}

static bool verifyRecording(const QString &path, int minFrames)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qCritical() << "[FAIL] cannot open" << path;
        return false;
    }
    { QByteArray ba = f.readAll(); g_data.assign(ba.constBegin(), ba.constEnd()); }
    f.close();
    size_t sz = g_data.size();
    qCritical() << "[verify] phase1: file loaded, size =" << (int)sz;
    long moov = findBox(0, sz, "moov");
    if (moov < 0) {
        qCritical() << "[FAIL] no moov";
        return false;
    }
    long videoTrak = -1, audioTrak = -1;
    size_t videoTrakEnd = 0;
    for (int i = 0; i < 4; i++) {
        size_t tEnd = 0;
        long t = findTrak(moov, sz, i, &tEnd);
        if (t < 0) break;
        char kind = trackKind(t, sz);
        if (kind == 'v' && videoTrak < 0) { videoTrak = t; videoTrakEnd = tEnd; }
        if (kind == 'a' && audioTrak < 0) { audioTrak = t; }
    }
    if (videoTrak < 0) {
        qCritical() << "[FAIL] no video trak";
        return false;
    }
    if (audioTrak < 0) {
        qCritical() << "[FAIL] no audio trak (mov recording must carry sound)";
        return false;
    }

    long mdia = findBox(videoTrak, videoTrakEnd, "mdia");
    long minf = mdia >= 0 ? findBox(mdia, videoTrakEnd, "minf") : -1;
    long stbl = minf >= 0 ? findBox(minf, videoTrakEnd, "stbl") : -1;
    if (stbl < 0) {
        qCritical() << "[FAIL] video stbl missing";
        return false;
    }
    long stsd = findBox(stbl, sz, "stsd");
    long avc1 = stsd >= 0 ? findBox(stsd + 8, stsd + rd32(stsd - 8), "avc1") : -1;
    long avcC = avc1 >= 0 ? findBox(avc1 + 78, avc1 + rd32(avc1 - 8), "avcC") : -1;
    long stsz = findBox(stbl, sz, "stsz");
    long stco = findBox(stbl, sz, "stco");
    long stss = findBox(stbl, sz, "stss");
    if (avcC < 0 || stsz < 0 || stco < 0) {
        qCritical() << "[FAIL] required boxes missing";
        return false;
    }
    size_t avcCSize = rd32(avcC - 8);
    const uint8_t *extradata = g_data.data() + avcC;
    int nalLenSize = (extradata[4] & 3) + 1;
    uint32_t sampleCount = rd32(stsz + 8);
    uint64_t chunkOff = rd32(stco + 8);
    std::vector<uint32_t> sync;
    if (stss >= 0) {
        uint32_t n = rd32(stss + 4);
        for (uint32_t i = 0; i < n; i++) {
            sync.push_back(rd32(stss + 8 + i * 4));
        }
    }
    qCritical() << "[verify] phase2: video samples =" << sampleCount
                << "syncSamples =" << sync.size();

    std::vector<std::vector<uint8_t>> annexb;
    uint64_t pos = chunkOff;
    for (uint32_t i = 0; i < sampleCount; i++) {
        uint32_t sSize = rd32(stsz + 12 + i * 4);
        if (pos + sSize > sz) {
            qCritical() << "[FAIL] sample" << i << "overruns file";
            return false;
        }
        annexb.emplace_back();
        std::vector<uint8_t> &out = annexb.back();
        const uint8_t sc[] = { 0, 0, 0, 1 };
        uint64_t p = pos;
        uint64_t end = pos + sSize;
        while (p + nalLenSize <= end) {
            uint32_t nalLen = 0;
            for (int b = 0; b < nalLenSize; b++) {
                nalLen = (nalLen << 8) | g_data[p + b];
            }
            p += nalLenSize;
            if (p + nalLen > end) {
                qCritical() << "[FAIL] nal overruns sample" << i;
                return false;
            }
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), g_data.data() + p, g_data.data() + p + nalLen);
            p += nalLen;
        }
        pos += sSize;
    }

    qCritical() << "[verify] phase3: annexb done";
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_malloc(avcCSize);
    memcpy(ctx->extradata, extradata, avcCSize);
    ctx->extradata_size = (int)avcCSize;
    int oret = avcodec_open2(ctx, codec, nullptr);
    qCritical() << "[verify] phase4: codec open, ret =" << oret;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int decoded = 0, decErr = 0;
    int w = 0, h = 0;
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            w = frame->width;
            h = frame->height;
            decoded++;
            av_frame_unref(frame);
        }
    };
    auto sendAll = [&](const std::vector<uint8_t> &buf) {
        if (buf.empty()) {
            return;
        }
        AVPacket *p = av_packet_alloc();
        av_new_packet(p, (int)buf.size());
        memcpy(p->data, buf.data(), buf.size());
        p->pts = decoded + decErr;
        p->dts = p->pts;
        if (avcodec_send_packet(ctx, p) < 0) {
            decErr++;
        }
        av_packet_free(&p);
        drain();
    };
    {
        const uint8_t sc[] = { 0, 0, 0, 1 };
        std::vector<uint8_t> header;
        int p2 = 6;
        int spsN = extradata[5] & 0x1f;
        for (int i = 0; i < spsN; i++) {
            int l = (extradata[p2] << 8) | extradata[p2 + 1];
            header.insert(header.end(), sc, sc + 4);
            header.insert(header.end(), extradata + p2 + 2, extradata + p2 + 2 + l);
            p2 += 2 + l;
        }
        int ppsN = extradata[p2];
        p2++;
        for (int i = 0; i < ppsN; i++) {
            int l = (extradata[p2] << 8) | extradata[p2 + 1];
            header.insert(header.end(), sc, sc + 4);
            header.insert(header.end(), extradata + p2 + 2, extradata + p2 + 2 + l);
            p2 += 2 + l;
        }
        {
            AVPacket *p = av_packet_alloc();
            av_new_packet(p, (int)header.size());
            memcpy(p->data, header.data(), header.size());
            p->pts = 0;
            p->dts = 0;
            avcodec_send_packet(ctx, p);
            av_packet_free(&p);
            drain();
        }
    }
    for (auto &v : annexb) {
        sendAll(v);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    avcodec_free_context(&ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    qInfo() << "[verify] video: decoded frames =" << decoded << "sendErrors =" << decErr
            << "resolution =" << w << "x" << h;
    bool videoOk = decoded >= minFrames && decErr == 0 && !sync.empty();

    long amd = findBox(audioTrak, sz, "mdia");
    long aminf = amd >= 0 ? findBox(amd, sz, "minf") : -1;
    long astbl = aminf >= 0 ? findBox(aminf, sz, "stbl") : -1;
    long asz = astbl >= 0 ? findBox(astbl, sz, "stsz") : -1;
    long aco = astbl >= 0 ? findBox(astbl, sz, "stco") : -1;
    if (asz < 0 || aco < 0) {
        qCritical() << "[FAIL] audio stbl missing";
        return false;
    }
    uint32_t aCount = rd32(asz + 8);
    uint64_t aOff = rd32(aco + 8);
    qint64 aBytes = 0;
    double aRmsSum = 0.0, aRmsPeak = 0.0;
    int aRmsN = 0;
    for (uint32_t i = 0; i < aCount; i++) {
        uint32_t sSize = rd32(asz + 12 + i * 4);
        if (aOff + sSize > sz) {
            qCritical() << "[FAIL] audio sample" << i << "overruns file";
            return false;
        }
        const int16_t *sm = reinterpret_cast<const int16_t *>(g_data.data() + aOff);
        int n = sSize / 2;
        if (n > 0) {
            double sum = 0;
            for (int j = 0; j < n; j++) {
                double v = sm[j] / 32768.0;
                sum += v * v;
            }
            double rms = std::sqrt(sum / n);
            aRmsSum += rms;
            if (rms > aRmsPeak) aRmsPeak = rms;
            aRmsN++;
        }
        aBytes += sSize;
        aOff += sSize;
    }
    double aAvg = aRmsN ? aRmsSum / aRmsN : 0.0;
    qInfo() << "[verify] audio: packets =" << aCount << "bytes =" << aBytes
            << "avg RMS =" << QString::number(aAvg, 'f', 1)
            << "peak RMS =" << QString::number(aRmsPeak, 'f', 1)
            << "(RMS=0 只说明录制期间手机媒体静默)";
    bool audioOk = aCount >= 50 && aBytes >= 48000LL * 2 * 2 * minFrames / 30;
    qInfo() << "[verify] audioOk =" << audioOk << "videoOk =" << videoOk;
    return videoOk && audioOk;
}

'''
src = src[:start] + new_fn + src[end:]
src = src.replace('verifyMp4(mp4s[mp4s.size() - 2].absoluteFilePath(), 15)', 'verifyRecording(mp4s[mp4s.size() - 2].absoluteFilePath(), 15)')
src = src.replace('verifyMp4(mp4s[mp4s.size() - 1].absoluteFilePath(), 15)', 'verifyRecording(mp4s[mp4s.size() - 1].absoluteFilePath(), 15)')
open(p, 'w', encoding='utf-8', newline='\n').write(src)
print("verifyRecording written")
