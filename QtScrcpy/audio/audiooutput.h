#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QByteArray>
#include <QObject>
#include <QPointer>

class QAudioOutput;
class QAudioSink;
class QIODevice;

/// 把设备回传的裸 PCM 写入系统默认音频输出。
///
/// PCM 来自 QtScrcpyCore 的 AudioDemuxer（服务端 audio_codec=raw，
/// 固定 48000Hz / 2 声道 / 16bit），因此这里不需要解码器，
/// 也不再使用早期基于 sndcpy 的「另起进程 + 监听 28200 端口」方案。
///
/// 音频数据经队列连接回到主线程，因此本类的所有方法都在主线程执行，无需加锁。
///
/// 播放侧做了缓冲回压：音频设备可用空间不足时，多余的 PCM 先留在 m_pending，
/// 等下次写入再续上。这样能吸收网络抖动造成的突发，而不是直接丢数据。
/// 待播量超过 MAX_LATENCY_MS 时按帧丢弃最旧数据，并一路丢到 TARGET_LATENCY_MS
/// —— 宁可丢一小段，也不能让延迟随时间无界增长（否则声音会越来越滞后于画面）。
class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    /// 写入一段 PCM；采样率/声道数变化时会自动重建输出设备
    void writePcm(const QByteArray &pcm, int sampleRate, int channelCount);
    /// 停止播放并释放音频设备
    void stop();
    /// 当前是否已打开音频输出
    bool isRunning() const;

private:
    void ensureOutput(int sampleRate, int channelCount);
    /// 在音频设备可用空间允许的范围内尽量写出 m_pending
    void flushPending();
    /// 音频设备当前可写入的字节数（0 表示不可写或设备未就绪）
    qint64 bytesFree() const;
    /// 把时长换算成字节数；格式未知时返回 0
    int latencyBytes(int ms) const;
    /// 待播队列的溢出阈值：超过就丢数据
    int maxPendingBytes() const;
    /// 溢出后回落到的低水位：低于阈值即停止丢弃
    int targetPendingBytes() const;

    /// 每个采样点的字节数（raw 通道固定 16bit）
    static const int BYTES_PER_SAMPLE = 2;
    /// 待播队列的溢出阈值（高水位）
    static const int MAX_LATENCY_MS = 300;
    /// 溢出后回落到的低水位。故意取得比高水位低不少：
    /// 若只丢到高水位，队列会一直贴在上限，一次网络卡顿就会让音频永久滞后
    /// 约 (设备缓冲 + 高水位)。丢到低水位能让延迟自行回落。
    static const int TARGET_LATENCY_MS = 100;

private:
    QPointer<QIODevice> m_outputDevice;
    QByteArray m_pending; // 尚未写入音频设备的 PCM 余量
    int m_sampleRate = 0;
    int m_channelCount = 0;
    qint64 m_droppedBytes = 0; // 累计丢弃字节数，仅用于日志
    int m_dropEvents = 0;      // 丢弃事件次数，用于限制日志频率
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput *m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};

#endif // AUDIOOUTPUT_H
