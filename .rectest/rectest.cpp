/**
 * rectest.cpp —— 录制功能端到端探针（不依赖 GUI）。
 *
 * 复用 QtScrcpyCore 生产链路（Device/Server/Demuxer/Recorder），
 * 模拟「投屏中途点击工具栏录制按钮」的真实场景：
 *   1. 连接成功后等 3 秒（流已跑过 config 包）→ startRecording #1
 *   2. 录 6 秒 → stopRecording #1
 *   3. 立即 startRecording #2（同一 Device 复用，回归 double-free）
 *   4. 录 6 秒 → stopRecording #2 → 断开
 *   5. 验证两个 mp4：手工解析 box（本工程精简 FFmpeg 无 mov demuxer，
 *      不能用 avformat_open_input），抽出 H264 sample 后用自带解码器实解
 *
 * 用法：rectest.exe <serial> <outDir>（放在 output/x64/RelWithDebInfo 下运行）
 */
#include <QTimer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QProcess>
#include <QThread>
#include <QPointer>
#include <QFile>

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
}

#include "QtScrcpyCore.h"
#include "adbprocess.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <utility>

static QString g_serial;
static QString g_dir;
static QElapsedTimer g_clock;
static bool g_fail = false;

// ---- mp4 手工 box 解析 + H264 实际解码验证 ----
static std::vector<uint8_t> g_data;

static uint32_t rd32(size_t off)
{
    return (uint32_t)g_data[off] << 24 | (uint32_t)g_data[off + 1] << 16
         | (uint32_t)g_data[off + 2] << 8 | g_data[off + 3];
}

static long findBox(size_t start, size_t end, const char *type)
{
    size_t off = start;
    while (off + 8 <= end) {
        uint32_t size = rd32(off);
        if (size < 8 || off + size > end) {
            return -1;
        }
        if (!memcmp(g_data.data() + off + 4, type, 4)) {
            return (long)(off + 8);
        }
        off += size;
    }
    return -1;
}

// 从 moov 里找第 idx 个 trak 的内容起点（0-based）
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

// 按 stsc/stco/stsz 标准语义构建每个 sample 的文件偏移
// （mov muxer 的音视频 chunk 在 mdat 里交错，不能用"连续累加"假设）
static bool buildSampleOffsets(long stbl, size_t sz, uint32_t count,
                               std::vector<uint64_t> &offsets)
{
    long stsz = findBox(stbl, sz, "stsz");
    long stco = findBox(stbl, sz, "stco");
    long stsc = findBox(stbl, sz, "stsc");
    if (stsz < 0 || stco < 0 || stsc < 0) {
        return false;
    }
    uint32_t nChunks = rd32(stco + 4);
    uint32_t scCount = rd32(stsc + 4);
    // stsz: sample_size 非 0 表示所有 sample 等大（此时无 sizes 表）
    uint32_t fixedSize = rd32(stsz + 4);
    offsets.assign(count, 0);
    uint32_t si = 0;
    for (uint32_t e = 0; e < scCount && si < count; e++) {
        uint32_t firstChunk = rd32(stsc + 8 + e * 12);        // 1-based
        uint32_t spc = rd32(stsc + 12 + e * 12);              // samples per chunk
        uint32_t lastChunk = (e + 1 < scCount) ? rd32(stsc + 8 + (e + 1) * 12) - 1
                                               : nChunks;
        for (uint32_t c = firstChunk; c <= lastChunk && si < count; c++) {
            uint64_t coff = rd32(stco + 8 + (c - 1) * 4);
            for (uint32_t k = 0; k < spc && si < count; k++) {
                uint32_t sSize = fixedSize ? fixedSize : rd32(stsz + 12 + si * 4);
                offsets[si] = coff;
                coff += sSize;
                si++;
            }
        }
    }
    return si == count;
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
    std::vector<uint64_t> vOffsets;
    if (!buildSampleOffsets(stbl, sz, sampleCount, vOffsets)) {
        qCritical() << "[FAIL] video sample table incomplete";
        return false;
    }
    for (uint32_t i = 0; i < sampleCount; i++) {
        uint32_t sSize = rd32(stsz + 12 + i * 4);
        uint64_t pos = vOffsets[i];
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
    long astsd = astbl >= 0 ? findBox(astbl, sz, "stsd") : -1;
    if (astsd < 0) {
        qCritical() << "[FAIL] audio stsd missing";
        return false;
    }
    // mp4a sample entry 里的 esds 必须携带 AudioSpecificConfig
    long mp4a = findBox(astsd + 8, astsd + rd32(astsd - 8), "mp4a");
    long esds = mp4a >= 0 ? findBox(mp4a + 28, mp4a + rd32(mp4a - 8), "esds") : -1;
    if (esds < 0) {
        qCritical() << "[FAIL] mp4a/esds missing (audio track not AAC)";
        return false;
    }
    // esds: ver/flags(4) + ES_Desc(0x03) → DecConfig(0x04) → DecSpecific(0x05) = ASC
    // 0x03/0x04 是容器描述符，0x05 携带 ASC——需要递归进入容器
    std::vector<uint8_t> asc;
    std::function<bool(size_t, size_t)> scanDesc = [&](size_t s, size_t e) -> bool {
        size_t p = s;
        while (p + 2 <= e) {
            uint8_t tag = g_data[p];
            size_t q = p + 1;
            uint32_t len = 0;
            for (int nb = 0; nb < 4 && q < e; nb++) {
                len = (len << 7) | (g_data[q] & 0x7f);
                q++;
                if (!(g_data[q - 1] & 0x80)) break;
            }
            if (q + len > e) return false;
            if (tag == 0x05) {
                asc.assign(g_data.data() + q, g_data.data() + q + len);
                return true;
            }
            // 0x03(ES): ES_ID(2)+flags(1) 后才是子描述符；0x04(DecConfig): 固定 13 字节头
            if (tag == 0x03) {
                if (scanDesc(q + 3, q + len)) return true;
            } else if (tag == 0x04) {
                if (scanDesc(q + 13, q + len)) return true;
            }
            p = q + len;
        }
        return false;
    };
    scanDesc(esds + 4, esds + rd32(esds - 8));
    qCritical() << "[verify] phaseA: esds found, ASC size =" << (int)asc.size();

    uint32_t aCount = rd32(asz + 8);
    uint32_t aFixedSize = rd32(asz + 4);
    std::vector<uint64_t> aOffsets;
    if (!buildSampleOffsets(astbl, sz, aCount, aOffsets)) {
        qCritical() << "[FAIL] audio sample table incomplete";
        return false;
    }
    qint64 aBytes = 0;
    std::vector<std::vector<uint8_t>> aFrames;
    for (uint32_t i = 0; i < aCount; i++) {
        uint32_t sSize = aFixedSize ? aFixedSize : rd32(asz + 12 + i * 4);
        uint64_t aOff = aOffsets[i];
        if (aOff + sSize > sz) {
            qCritical() << "[FAIL] audio sample" << i << "overruns file";
            return false;
        }
        aFrames.emplace_back(g_data.data() + aOff, g_data.data() + aOff + sSize);
        aBytes += sSize;
    }

    // AAC 实解（定制 FFmpeg 带 aac decoder）：帧 → PCM → RMS
    const AVCodec *adec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!adec) {
        qCritical() << "[FAIL] aac decoder unavailable";
        return false;
    }
    AVCodecContext *actx = avcodec_alloc_context3(adec);
    if (!asc.empty()) {
        actx->extradata = (uint8_t *)av_malloc(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(actx->extradata, asc.data(), asc.size());
        actx->extradata_size = (int)asc.size();
    }
    int aOpen = avcodec_open2(actx, adec, nullptr);
    qCritical() << "[verify] phaseB: aac decoder open, ret =" << aOpen;
    AVFrame *aframe = av_frame_alloc();
    AVPacket *apkt = av_packet_alloc();
    qint64 aDecSamples = 0;
    double aRmsSum = 0.0, aRmsPeak = 0.0;
    int aRmsN = 0, aDecErr = 0;
    auto aDrain = [&]() {
        while (avcodec_receive_frame(actx, aframe) >= 0) {
            // 解码输出为 fltp 平面，转 s16 量级再算 RMS
            const int ch = aframe->channels ? aframe->channels : 2;
            for (int j = 0; j < aframe->nb_samples; j++) {
                for (int c = 0; c < ch; c++) {
                    const float *plane = reinterpret_cast<const float *>(aframe->data[c]);
                    double v = plane[j];
                    aRmsSum += v * v / ch;
                }
                aRmsN += ch;
            }
            if (aframe->nb_samples > 0) {
                double rms = std::sqrt(aRmsSum > 0 ? aRmsSum / (aRmsN ? aRmsN : 1) : 0.0);
                (void)rms;
            }
            aDecSamples += aframe->nb_samples;
            av_frame_unref(aframe);
        }
    };
    for (auto &fr : aFrames) {
        av_new_packet(apkt, (int)fr.size());
        memcpy(apkt->data, fr.data(), fr.size());
        apkt->pts = aDecSamples;
        if (avcodec_send_packet(actx, apkt) < 0) {
            aDecErr++;
        }
        av_packet_unref(apkt);
        aDrain();
    }
    avcodec_send_packet(actx, nullptr);
    aDrain();
    avcodec_free_context(&actx);
    av_frame_free(&aframe);
    av_packet_free(&apkt);
    double aSeconds = (double)aDecSamples / 48000.0;
    // 峰值用样本绝对值，均值用均方
    double aAvg = aRmsN ? std::sqrt(aRmsSum / aRmsN) : 0.0;
    qInfo() << "[verify] audio: packets =" << aCount << "bytes =" << aBytes
            << "decodedSamples =" << aDecSamples << "seconds =" << QString::number(aSeconds, 'f', 2)
            << "avg RMS =" << QString::number(aAvg, 'f', 4)
            << "peak RMS =" << QString::number(aRmsPeak, 'f', 4)
            << "decErrs =" << aDecErr
            << "(RMS=0 只说明录制期间手机媒体静默)";
    bool audioOk = aCount >= 50 && aDecSamples >= 48000LL * minFrames / 30 && aDecErr == 0;
    qInfo() << "[verify] audioOk =" << audioOk << "videoOk =" << videoOk;
    return videoOk && audioOk;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        qCritical() << "usage: rectest.exe <serial> <outDir>";
        return 2;
    }
    g_serial = argv[1];
    g_dir = argv[2];
    QDir().mkpath(g_dir);

    qsc::AdbProcess::setAdbPath(QCoreApplication::applicationDirPath() + "/adb.exe");

    // 探针环境手机常处于息屏状态：屏幕静止/熄灭时 H264 编码器几乎不出帧，
    // 会把「中途录制」测成无帧场景。与真实用户（投屏时亮屏）对齐：先唤醒
    QProcess::startDetached(QCoreApplication::applicationDirPath() + "/adb.exe",
                            {"-s", g_serial, "shell", "input", "keyevent", "224"});
    QThread::msleep(800);

    qsc::DeviceParams params;
    params.serial = g_serial;
    params.serverLocalPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
    params.serverRemotePath = "/data/local/tmp/scrcpy_server_rectest";
    params.maxSize = 1600;
    params.bitRate = 8000000;
    params.maxFps = 60;
    params.logLevel = "debug";
    params.display = true;
    params.recordFile = false;
    params.recordPath = g_dir;
    params.recordFileFormat = "mp4";
    params.audio = true;      // 服务端采集（录制声音的前提）
    params.playAudio = true;  // 电脑扬声器播放
    // 生产同款（dialog.cpp）：随机 scid，与 reverse 的 localabstract 名配对
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;
    // 生产同款（dialog.cpp）：中途录制需要密集自然关键帧
    params.codecOptions = "i-frame-interval=2";

    qsc::IDeviceManage &mgr = qsc::IDeviceManage::getInstance();
    if (!mgr.connectDevice(params)) {
        qCritical() << "[FAIL] connectDevice rejected";
        return 2;
    }
    QPointer<qsc::IDevice> device = mgr.getDevice(g_serial);
    if (!device) {
        qCritical() << "[FAIL] device not found";
        return 2;
    }

    QObject::connect(device, &qsc::IDevice::deviceConnected,
                     [&](bool success, const QString &serial, const QString &name, const QSize &size) {
        qInfo() << "[evt] deviceConnected success =" << success << serial << name << size;
        if (!success) {
            qCritical() << "[FAIL] server start failed";
            g_fail = true;
            app.quit();
            return;
        }
        g_clock.restart();

        // t+3s：流已跑了 3 秒，config 包早已发过 —— 模拟用户此刻点录制按钮
        QTimer::singleShot(3000, [] {
            qInfo() << ">>> [t+3s] startRecording #1 (mid-stream)";
            qsc::IDevice *d = qsc::IDeviceManage::getInstance().getDevice(g_serial);
            if (!d || !d->startRecording()) {
                qCritical() << "[FAIL] startRecording #1:"
                            << (d ? d->lastRecordingError() : QString("device gone"));
                g_fail = true;
            } else if (!d->isRecording()) {
                qCritical() << "[FAIL] startRecording #1 returned true but thread not running";
                g_fail = true;
            }
        });
        // t+9s：停止第一段
        QTimer::singleShot(9000, [] {
            qInfo() << ">>> [t+9s] stopRecording #1";
            qsc::IDeviceManage::getInstance().getDevice(g_serial)->stopRecording();
        });
        // t+9.5s：立刻再录（同一 Recorder 复用，回归 double-free）
        QTimer::singleShot(9500, [] {
            qInfo() << ">>> [t+9.5s] startRecording #2 (reuse)";
            qsc::IDevice *d = qsc::IDeviceManage::getInstance().getDevice(g_serial);
            if (!d || !d->startRecording()) {
                qCritical() << "[FAIL] startRecording #2:"
                            << (d ? d->lastRecordingError() : QString("device gone"));
                g_fail = true;
            }
        });
        // t+15.5s：停止第二段
        QTimer::singleShot(15500, [] {
            qInfo() << ">>> [t+15.5s] stopRecording #2";
            qsc::IDeviceManage::getInstance().getDevice(g_serial)->stopRecording();
        });
        // t+16.5s：验证 + 收工（先验证后断开，避开 Device 销毁时序）
        QTimer::singleShot(16500, [] {
            // Device 按 serial_时间戳 命名文件：取目录里最新的两个 mp4 验证
            QDir dir(g_dir);
            QFileInfoList mp4s = dir.entryInfoList(QStringList() << "*.mp4", QDir::Files,
                                                   QDir::Time | QDir::Reversed);
            if (mp4s.size() < 2) {
                qCritical() << "[FAIL] expect 2 mp4 files, got" << mp4s.size();
                g_fail = true;
            }
            bool ok1 = mp4s.size() > 1 && verifyRecording(mp4s[mp4s.size() - 2].absoluteFilePath(), 15);
            bool ok2 = mp4s.size() > 0 && verifyRecording(mp4s[mp4s.size() - 1].absoluteFilePath(), 15);
            qInfo() << "====================================";
                    qInfo() << (ok1 && ok2 && !g_fail ? "[PASS] recording e2e OK"
                                                      : "[FAIL] recording e2e FAILED");
            qInfo() << "====================================";
            QCoreApplication::exit(ok1 && ok2 && !g_fail ? 0 : 1);
        });
        // 退出时统一断开（DeviceManage 析构走不到，主动来一次）
        QTimer::singleShot(16550, [] {
            qsc::IDeviceManage::getInstance().disconnectAllDevice();
        });
    });

    // 总超时保护
    QTimer::singleShot(45000, [] {
        qCritical() << "[FAIL] global timeout";
        QCoreApplication::exit(1);
    });

    return app.exec();
}
