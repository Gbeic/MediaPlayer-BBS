package net.hacker.mediaplayer;

import net.minecraft.client.Minecraft;

import java.io.File;
import java.nio.file.Path;

/**
 * 面向 BBS++ 暴露的视频后端入口。
 * 该类负责把 BBS 资产相对路径解析到固定的视频目录，并提供温和的可用性检测，
 * 让 BBS++ 可以把 MediaPlayer-BBS 当成可选依赖使用，而不是在 native 缺失时直接崩溃。
 */
public final class BBSVideoBackend {
    private static final String ASSETS_DIRECTORY = "config/bbs/assets";

    private BBSVideoBackend() {
    }

    public static boolean isAvailable() {
        return MediaPlayer.isNativeAvailable();
    }

    public static String getUnavailableReason() {
        return MediaPlayer.getUnavailableReason();
    }

    public static Path getAssetsDirectory() {
        return Minecraft.getInstance().gameDirectory.toPath().resolve(ASSETS_DIRECTORY).normalize();
    }

    public static Path getVideoDirectory() {
        return getAssetsDirectory().resolve("video").normalize();
    }

    public static File resolveAssetVideo(String relativePath) {
        if (relativePath == null || relativePath.isBlank()) {
            throw new IllegalArgumentException("视频路径不能为空");
        }

        Path relative = Path.of(relativePath.replace('\\', '/')).normalize();
        if (relative.isAbsolute()) {
            throw new IllegalArgumentException("视频路径必须是 BBS 资产相对路径");
        }

        Path assets = getAssetsDirectory();
        Path resolved = assets.resolve(relative).normalize();
        if (!resolved.startsWith(assets)) {
            throw new IllegalArgumentException("视频路径不能离开 BBS 资产目录");
        }

        return resolved.toFile();
    }

    public static BBSVideoDecoder openAssetVideo(String relativePath) {
        return BBSVideoDecoder.open(resolveAssetVideo(relativePath));
    }
}
