package net.hacker.mediaplayer;

import net.minecraft.client.Minecraft;

import java.io.File;
import java.lang.ref.Cleaner;

import static org.lwjgl.glfw.GLFW.glfwGetTime;

public final class VideoDecoder implements AutoCloseable {
    @NativeUsed
    private final long ptr;
    @NativeUsed
    private double frameRate;
    @NativeUsed
    private double framesPerSecond;
    @NativeUsed
    private int width;
    @NativeUsed
    private int height;
    @NativeUsed
    private double duration;
    @NativeUsed
    private boolean hasAudio;
    private final Cleaner.Cleanable cleaner;
    public final VideoFrame frame;
    public final AudioDecoder audio;
    private double lastTime;
    private double deltaTime;
    private double lastTimelineSeconds = Double.NaN;
    private double lastDecodedTimelineSeconds = Double.NaN;
    private final Minecraft minecraft = Minecraft.getInstance();
    private static boolean timelineNativeAvailable = true;

    public VideoDecoder(String path, DeviceType type) {
        this(path, type, true);
    }

    public VideoDecoder(String path, DeviceType type, boolean enableAudio) {
        MediaPlayer.requireNativeAvailable();

        if (path == null || path.isBlank()) {
            throw new IllegalArgumentException("Video path is null or blank");
        }

        File file = new File(path);
        if (!file.exists() || !file.isFile()) {
            throw new IllegalArgumentException("File not found: " + path);
        }

        AudioDecoder a = null;
        if (enableAudio) {
            try {
                a = new AudioDecoder(path);
            } catch (Throwable ignore) {
                MediaPlayer.LOGGER.warn("无法创建音频解码器: {}", path);
            }
        }
        audio = a;

        frame = new VideoFrame();
        frame.setFilter(false, false);

        var textureId = frame.getId();
        if (textureId == 0) {
            throw new IllegalStateException("VideoFrame texture ID is 0");
        }

        try {
            long p = open(path, textureId, type.value);
            if (p == 0) {
                throw new RuntimeException("Native open() failed: returned NULL ptr (path=" + path + ", type=" + type + ")");
            }
            
            ptr = p;
            cleaner = MediaPlayer.cleaner.register(this, () -> release(p));
        } catch (UnsatisfiedLinkError e) {
            throw new RuntimeException("Native method open() not found or incompatible", e);
        }

        lastTime = glfwGetTime();
        deltaTime = 0.0;
    }


    public double getFrameRate() {
        return frameRate;
    }

    public double getFramesPerSecond() {
        return framesPerSecond;
    }

    public int getVideoWidth() {
        return width;
    }

    public int getVideoHeight() {
        return height;
    }

    public double getDuration() {
        return duration;
    }

    public boolean hasAudio() {
        return hasAudio;
    }

    public int getTextureId() {
        return frame.getId();
    }

    public void fetch() {
        var currentTime = glfwGetTime();
        var frameInterval = currentTime - lastTime;
        lastTime = currentTime;
        deltaTime += frameInterval;
        if (deltaTime >= frameRate) {
            deltaTime -= frameRate;
            if (!minecraft.isPaused()) decode();
        }
    }

    public void renderTime(double seconds) {
        var targetSeconds = Double.isFinite(seconds) ? Math.max(0.0, seconds) : 0.0;
        var frameInterval = frameRate > 0.0 ? frameRate : 1.0 / 60.0;
        var maxContinuousGap = Math.max(0.25, frameInterval * 8.0);
        var firstFrame = Double.isNaN(lastDecodedTimelineSeconds);
        var movedBack = !Double.isNaN(lastTimelineSeconds)
                && targetSeconds < lastTimelineSeconds - frameInterval * 0.5;
        var jumpedForward = !firstFrame
                && targetSeconds - lastDecodedTimelineSeconds > maxContinuousGap;
        var needsSeek = firstFrame || movedBack || jumpedForward;

        if (timelineNativeAvailable && needsSeek) {
            try {
                renderTimeNative(targetSeconds);
                lastTimelineSeconds = targetSeconds;
                lastDecodedTimelineSeconds = targetSeconds;
                return;
            } catch (UnsatisfiedLinkError e) {
                timelineNativeAvailable = false;
                MediaPlayer.LOGGER.warn("native 时间轴寻帧接口不可用，退回连续解码路径");
            }
        }

        // 老 native 没有时间轴寻帧接口时只能顺序解码，至少保持首次画面和正向播放可用。
        if (!timelineNativeAvailable && firstFrame) {
            decode();
            lastDecodedTimelineSeconds = targetSeconds;
        } else if (!needsSeek && targetSeconds - lastDecodedTimelineSeconds >= frameInterval * 0.5) {
            decode();
            lastDecodedTimelineSeconds = targetSeconds;
        }

        lastTimelineSeconds = targetSeconds;
    }

    private native long open(String path, int texture, int type);

    private static native void release(long ptr);

    public native void decode();

    private native void renderTimeNative(double seconds);

    public static VideoDecoder create(File file) {
        try {
            return new VideoDecoder(file.getAbsolutePath(), DeviceType.CUDA);
        } catch (Throwable e) {
            try {
                return new VideoDecoder(file.getAbsolutePath(), DeviceType.D3D12VA);
            } catch (Throwable e1) {
                try {
                    return new VideoDecoder(file.getAbsolutePath(), DeviceType.D3D11VA);
                } catch (Throwable e2) {
                    try {
                        return new VideoDecoder(file.getAbsolutePath(), DeviceType.NONE);
                    } catch (Throwable e3) {
                        throw new RuntimeException(e3);
                    }
                }
            }
        }
    }

    public static VideoDecoder createTimeline(File file) {
        try {
            return new VideoDecoder(file.getAbsolutePath(), DeviceType.CUDA, false);
        } catch (Throwable e) {
            try {
                return new VideoDecoder(file.getAbsolutePath(), DeviceType.D3D12VA, false);
            } catch (Throwable e1) {
                try {
                    return new VideoDecoder(file.getAbsolutePath(), DeviceType.D3D11VA, false);
                } catch (Throwable e2) {
                    try {
                        return new VideoDecoder(file.getAbsolutePath(), DeviceType.NONE, false);
                    } catch (Throwable e3) {
                        throw new RuntimeException(e3);
                    }
                }
            }
        }
    }

    @Override
    public void close() {
        if (cleaner != null) cleaner.clean();
    }
}
