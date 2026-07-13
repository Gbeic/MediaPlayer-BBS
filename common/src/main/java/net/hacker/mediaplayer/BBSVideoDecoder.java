package net.hacker.mediaplayer;

import java.io.File;

/**
 * BBS++ 时间轴使用的视频解码器包装层。
 * 它屏蔽原 MediaPlayer 命令播放器的音频和实体播放状态，只暴露按秒数渲染画面、
 * 获取 OpenGL 纹理和读取元数据这几个 BBS 影片系统真正需要的能力。
 */
public final class BBSVideoDecoder implements AutoCloseable {
    private final File file;
    private final VideoDecoder decoder;
    private final VideoMetadata metadata;

    private BBSVideoDecoder(File file, VideoDecoder decoder) {
        this.file = file;
        this.decoder = decoder;
        this.metadata = VideoMetadata.available(
                decoder.getVideoWidth(),
                decoder.getVideoHeight(),
                decoder.getDuration(),
                decoder.getFramesPerSecond(),
                decoder.hasAudio()
        );
    }

    public static BBSVideoDecoder open(File file) {
        if (file == null) {
            throw new IllegalArgumentException("视频文件不能为空");
        }

        if (!file.exists() || !file.isFile()) {
            throw new IllegalArgumentException("视频文件不存在: " + file);
        }

        return new BBSVideoDecoder(file, VideoDecoder.createTimeline(file));
    }

    public File getFile() {
        return file;
    }

    public VideoMetadata getMetadata() {
        return metadata;
    }

    public void renderTime(double seconds) {
        decoder.renderTime(seconds);
    }

    public int getTextureId() {
        return decoder.getTextureId();
    }

    public VideoFrame getFrame() {
        return decoder.frame;
    }

    @Override
    public void close() {
        decoder.close();
    }
}
