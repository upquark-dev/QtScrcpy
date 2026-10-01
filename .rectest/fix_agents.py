p = 'E:/QtScrcpy/AGENTS.md'
lines = open(p, encoding='utf-8').read().split('\n')

# 1) 去重（保留首次出现的完全相同行）
seen = set()
dedup = []
for ln in lines:
    key = ln.strip()
    if key and key in seen:
        continue
    if key:
        seen.add(key)
    dedup.append(ln)
lines = dedup

# 2) 替换"录制带声音"行为新版两行
NEW_AUDIO = [
    '- **录制带声音（2026-10-01/02，方案 B：定制 FFmpeg + AAC 编码）**：`Recorder` 支持 PCM 音频轨——`setAudioFormat(rate, ch)`（open 前调用）+ `pushAudio(data, size)` + `flushAudio()`（stopRecorder 前冲刷编码器残余帧）；`Device` 在 audioData 回调里把 PCM 分流给录制。**音频链路**：s16 交错 → swr 转 FLTP 平面 → AVAudioFifo 攒 1024 帧 → native aac encoder → AAC 包随视频线程写 mux。**容器兼容性（muxprobe 实测）**：mp4/mov/matroska 都支持 AAC（标准 mp4a+esds），mp4 的 raw PCM 被容器规范拒绝；formatBox 现为 [mp4, mov, mkv] 全部带声音，音频轨丢帧逻辑与视频对齐（关键帧前丢弃，保证音视频同起点）。',
    '- **FFmpeg 定制构建（2026-10-02，替换 Windows x64）**：`third_party/ffmpeg` 原 n4.4.2 裁剪版缺 aac encoder/parser，已用官方 n4.4.2 源码（gitee 镜像）重编替换 **Windows x64**（x86/arm64/macOS/Linux 仍为旧库；代码对无 aac encoder 的平台已兜底——`avcodec_find_encoder(AAC)` 为空则降级纯视频录制并警告）。**configure**（本机 D:\\mingw64 gcc 14.2 + nasm，源码与日志在 `.ffmpegbuild/`）：`--enable-shared --disable-static --disable-programs --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages --disable-everything --enable-decoder=h264 --enable-decoder=aac --enable-demuxer=h264 --enable-demuxer=aac --enable-encoder=aac --enable-parser=h264 --enable-parser=aac --enable-muxer=mp4 --enable-muxer=mov --enable-muxer=matroska --enable-protocol=file --enable-swscale --enable-swresample --disable-inline-asm`。**坑**：① GCC 14 + FFmpeg 4.4 内联汇编报 `operand type mismatch for shr`（avc.o/av1.o），必须 `--disable-inline-asm`（nasm 外部汇编保留，h264 解码性能无虞）；② `--disable-everything` 连 parsers 一起关，漏 `--enable-parser=h264` 会 demuxer `Could not initialize parser` 直接挂；③ make 用 `D:/mingw64/bin/mingw32-make.exe`（git-bash 无 make）；④ 新库 avutil 有 av_mallocz/av_free（旧精简版没有）。',
]
for i, ln in enumerate(lines):
    if ln.startswith('- **录制带声音'):
        lines[i] = NEW_AUDIO[0]
        # 插入第二条（若下一行不是它）
        if not lines[i + 1].startswith('- **FFmpeg 定制构建'):
            lines.insert(i + 1, NEW_AUDIO[1])
        break

# 3) 更新探针描述行（mov → mp4 双轨）
for i, ln in enumerate(lines):
    if '双轨验证探针' in ln:
        lines[i] = '- **双轨验证探针**：`.rectest/rectest.cpp`（audio=true + mp4 录制；验证 = box 解析 [stsc/stco/stsz 标准语义，mov/mp4 的音视频 chunk 在 mdat 里物理交错、音频 stsz 常为固定 sample_size 无表] + H264 逐帧实解 + esds 提取 ASC（0x03 跳 3 字节、0x04 跳 13 字节后递归找 0x05）+ AAC 实解 RMS）；注意 rectest.exe 是静态链库的，libQtScrcpyCore.a 更新后必须重链'
        break

open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
print("AGENTS.md cleaned and updated")
