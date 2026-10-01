#include <QAudioOutput>
#include <QDebug>
#include <QIODevice>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

#include "audiooutput.h"

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::isRunning() const
{
    return !m_outputDevice.isNull();
}

void AudioOutput::stop()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
    }
#else
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
#endif
    m_outputDevice = nullptr;
    // 设备已销毁，残留的 PCM 属于旧格式，不能再续播
    m_pending.clear();
    m_sampleRate = 0;
    m_channelCount = 0;
    m_droppedBytes = 0;
    m_dropEvents = 0;
}

qint64 AudioOutput::bytesFree() const
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    return m_audioOutput ? m_audioOutput->bytesFree() : 0;
#else
    return m_audioSink ? m_audioSink->bytesFree() : 0;
#endif
}

int AudioOutput::latencyBytes(int ms) const
{
    if (m_sampleRate <= 0 || m_channelCount <= 0) {
        return 0;
    }
    return m_sampleRate * m_channelCount * BYTES_PER_SAMPLE * ms / 1000;
}

int AudioOutput::maxPendingBytes() const
{
    return latencyBytes(MAX_LATENCY_MS);
}

int AudioOutput::targetPendingBytes() const
{
    return latencyBytes(TARGET_LATENCY_MS);
}

void AudioOutput::flushPending()
{
    // raw 通道的 PCM 是交错的立体声采样，写入量必须按帧对齐，
    // 否则会左右声道错位（听感上是人声飘到一边）。
    const int frameSize = m_channelCount * BYTES_PER_SAMPLE;
    if (frameSize <= 0) {
        return;
    }

    while (!m_pending.isEmpty()) {
        const qint64 free = bytesFree();
        if (free <= 0) {
            break;
        }

        int chunk = m_pending.size();
        if (chunk > free) {
            chunk = static_cast<int>(free);
        }
        chunk -= chunk % frameSize;
        if (chunk <= 0) {
            // 剩余空间连一帧都放不下，等设备消费后再续
            break;
        }

        const qint64 written = m_outputDevice->write(m_pending.constData(), chunk);
        if (written <= 0) {
            // 设备出错或已停止，保留数据等下次重试
            break;
        }
        m_pending.remove(0, static_cast<int>(written));
    }
}

void AudioOutput::ensureOutput(int sampleRate, int channelCount)
{
    if (isRunning() && sampleRate == m_sampleRate && channelCount == m_channelCount) {
        return;
    }

    if (sampleRate <= 0 || channelCount <= 0) {
        return;
    }

    // 首次播放或参数变化：按新格式重建输出
    stop();

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channelCount);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
    if (!info.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }

    m_audioOutput = new QAudioOutput(format, this);
    connect(m_audioOutput, &QAudioOutput::stateChanged, this, [](QAudio::State state) {
        qInfo() << "AudioOutput::audio state changed:" << state;
    });
    m_outputDevice = m_audioOutput->start();
#else
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channelCount);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }

    m_audioSink = new QAudioSink(device, format, this);
    m_outputDevice = m_audioSink->start();
#endif

    if (m_outputDevice.isNull()) {
        qWarning() << "AudioOutput::audio output device not available, cannot play audio.";
        stop();
        return;
    }

    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    qInfo() << "AudioOutput::audio playback started at" << sampleRate << "Hz" << channelCount << "channels";
}

void AudioOutput::writePcm(const QByteArray &pcm, int sampleRate, int channelCount)
{
    if (pcm.isEmpty()) {
        return;
    }

    ensureOutput(sampleRate, channelCount);
    if (m_outputDevice.isNull()) {
        // 没有可用的音频设备（例如缺少 Qt 音频插件）：直接丢弃，不缓存
        m_pending.clear();
        return;
    }

    // 上一包还没写完就接到队尾，保证播放连续
    if (m_pending.isEmpty()) {
        m_pending = pcm;
    } else {
        m_pending.append(pcm);
    }

    // 设备消费不过来时丢弃最旧的数据，并一路丢到低水位，
    // 让待播量回落而不是永久贴在上限
    const int maxPending = maxPendingBytes();
    if (maxPending > 0 && m_pending.size() > maxPending) {
        const int frameSize = m_channelCount * BYTES_PER_SAMPLE;
        int dropped = m_pending.size() - targetPendingBytes();
        if (frameSize > 0) {
            // 向前对齐到帧边界，避免丢出半个采样帧导致左右声道错位
            dropped += (frameSize - dropped % frameSize) % frameSize;
        }
        if (dropped > m_pending.size()) {
            dropped = m_pending.size();
        }
        m_pending.remove(0, dropped);

        m_droppedBytes += dropped;
        // 每次丢完都会回落到低水位，因此丢弃是稀疏事件，但仍做限流以防刷屏
        if (0 == m_dropEvents % 100) {
            qWarning() << "AudioOutput::audio buffer overflow, dropped" << dropped
                       << "bytes (total" << m_droppedBytes << "bytes, pending now"
                       << m_pending.size() << "bytes, cap" << MAX_LATENCY_MS << "ms)";
        }
        ++m_dropEvents;
    }

    flushPending();
}
