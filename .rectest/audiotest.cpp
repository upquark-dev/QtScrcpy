/**
 * audiotest.cpp —— 音频端到端探针（不依赖 GUI）。
 *
 * 复用 QtScrcpyCore 生产链路（audio=true），验证：
 *   1. audio socket 是否建立（服务端是否收到 audio=true）
 *   2. AudioDemuxer → DeviceObserver::onAudioData 是否有 PCM 流
 *   3. PCM 的 RMS（判定有声/静音，手机需在播放媒体）
 *
 * 用法：audiotest.exe <serial> <seconds>
 */
#include <QTimer>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QApplication>
#include <QPointer>

#include <cmath>

#include "QtScrcpyCore.h"
#include "adbprocess.h"
#include "audiooutput.h"

static QString g_serial;
static int g_targetSec = 10;
static QElapsedTimer g_clock;

// PCM 统计
static qint64 g_pcmPackets = 0;
static qint64 g_pcmBytes = 0;
static double g_rmsSum = 0.0;
static double g_rmsPeak = 0.0;
static int g_sampleRate = 0;
static int g_channels = 0;

static AudioOutput *g_audioOut = nullptr;

class ProbeObserver : public qsc::DeviceObserver
{
public:
    void onAudioData(const QByteArray &pcm, int sampleRate, int channelCount) override
    {
        g_pcmPackets++;
        if (g_audioOut) {
            g_audioOut->writePcm(pcm, sampleRate, channelCount);
        }
        g_pcmBytes += pcm.size();
        g_sampleRate = sampleRate;
        g_channels = channelCount;
        // RMS（16bit 小端）
        const int16_t *samples = reinterpret_cast<const int16_t *>(pcm.constData());
        int n = pcm.size() / 2;
        if (n > 0) {
            double sum = 0;
            for (int i = 0; i < n; i++) {
                double v = samples[i] / 32768.0;
                sum += v * v;
            }
            double rms = std::sqrt(sum / n);
            g_rmsSum += rms;
            if (rms > g_rmsPeak) {
                g_rmsPeak = rms;
            }
        }
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);  // QAudioOutput 的 Windows 后端需要 GUI(COM) 初始化
    if (argc < 2) {
        qCritical() << "usage: audiotest.exe <serial> [seconds]";
        return 2;
    }
    g_serial = argv[1];
    if (argc >= 3) {
        g_targetSec = atoi(argv[2]);
    }

    qsc::AdbProcess::setAdbPath(QCoreApplication::applicationDirPath() + "/adb.exe");

    qsc::DeviceParams params;
    params.serial = g_serial;
    params.serverLocalPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
    params.serverRemotePath = "/data/local/tmp/scrcpy_server_rectest";
    params.maxSize = 1600;
    params.bitRate = 8000000;
    params.maxFps = 60;
    params.logLevel = "info";
    params.display = true;
    params.audio = true;          // 本次验证音频
    params.audioCodec = "raw";
    params.audioSource = "playback";
    params.codecOptions = "i-frame-interval=2";
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;

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

    g_audioOut = new AudioOutput();
    ProbeObserver obs;
    device->registerDeviceObserver(&obs);

    g_clock.restart();
    int duration = g_targetSec * 1000;

    QTimer::singleShot(duration + 2000, [&] {
        // 打印统计
        double avgRms = g_pcmPackets > 0 ? g_rmsSum / g_pcmPackets : 0.0;
        qInfo() << "====================";
        qInfo() << "pcm packets   =" << g_pcmPackets;
        qInfo() << "pcm bytes     =" << g_pcmBytes;
        qInfo() << "format        =" << g_sampleRate << "Hz x" << g_channels << "ch";
        qInfo() << "avg RMS       =" << QString::number(avgRms, 'f', 1);
        qInfo() << "peak RMS      =" << QString::number(g_rmsPeak, 'f', 1);
        bool streamOk = g_pcmPackets > 100;
        bool soundOk = g_rmsPeak > 100.0;
        if (!streamOk) {
            qCritical() << "[FAIL] no audio stream (audio socket never delivered PCM)";
            qInfo() << "====================";
            QCoreApplication::exit(1);
            return;
        }
        if (!soundOk) {
            qWarning() << "[WARN] stream OK but RMS=0 (device silent: play some media on the phone)";
        }
        qInfo() << "[PASS] audio stream verified, RMS peak =" << QString::number(g_rmsPeak, 'f', 1);
        qInfo() << "audio output isRunning =" << g_audioOut->isRunning()
                << "(扬声器应已播放探针期间的设备声音)";
        qInfo() << "====================";
        g_audioOut->stop();
        delete g_audioOut;
        g_audioOut = nullptr;
        QCoreApplication::exit(0);
    });
    QTimer::singleShot(duration, [&] {
        mgr.disconnectDevice(g_serial);
    });

    QTimer::singleShot((duration + 15000), [] {
        qCritical() << "[FAIL] global timeout";
        QCoreApplication::exit(1);
    });

    return app.exec();
}
