package net.hacker.mediaplayer;

/**
 * BBS 时间轴视频后端返回的元数据快照。
 * 它把视频尺寸、时长、帧率和音轨信息集中成不可变对象，BBS++ 可以用它决定表单默认值、
 * 画面比例和错误展示，而不需要直接触碰底层 native 解码器。
 */
public record VideoMetadata(
        int width,
        int height,
        double durationSeconds,
        double frameRate,
        boolean hasAudio,
        boolean available,
        String failureReason
) {
    public static VideoMetadata available(int width, int height, double durationSeconds, double frameRate, boolean hasAudio) {
        return new VideoMetadata(width, height, durationSeconds, frameRate, hasAudio, true, "");
    }

    public static VideoMetadata unavailable(String failureReason) {
        return new VideoMetadata(0, 0, 0.0, 0.0, false, false, failureReason);
    }
}
