package net.hacker.mediaplayer;

import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.network.chat.Component;
import net.minecraft.world.entity.Entity;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.ref.Cleaner;
import java.util.function.BiFunction;

public final class MediaPlayer {
    public static final String MOD_ID = "mediaplayer";
    static final Cleaner cleaner = Cleaner.create();
    public static BiFunction<Audio, Entity, SoundInstance> audioFactory;
    public static final Logger LOGGER = LoggerFactory.getLogger(MediaPlayer.class);
    private static final boolean NATIVE_AVAILABLE;
    private static final String UNAVAILABLE_REASON;
    private static final String[] NATIVE_DEPENDENCIES = {
            "avutil-61.dll",
            "swresample-7.dll",
            "swscale-10.dll",
            "avcodec-63.dll",
            "avformat-63.dll"
    };

    static {
        boolean available = false;
        String reason = "";

        if (!System.getProperty("os.name").toLowerCase().contains("windows")) {
            reason = "当前系统不支持 MediaPlayer-BBS native 后端";
        } else {
            try {
                for (String dependency : NATIVE_DEPENDENCIES) {
                    copyAndLoadNativeLibrary(dependency);
                }

                copyAndLoadNativeLibrary("MediaPlayer.dll");
                available = true;
            } catch (Throwable e) {
                reason = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                LOGGER.warn("MediaPlayer-BBS native 后端不可用: {}", reason);
            }
        }

        NATIVE_AVAILABLE = available;
        UNAVAILABLE_REASON = reason;
    }

    public static String getText(String key) {
        return Component.translatable(key).getString();
    }

    public static boolean isNativeAvailable() {
        return NATIVE_AVAILABLE;
    }

    public static String getUnavailableReason() {
        return UNAVAILABLE_REASON;
    }

    public static void requireNativeAvailable() {
        if (!NATIVE_AVAILABLE) {
            throw new IllegalStateException(UNAVAILABLE_REASON);
        }
    }

    private static void copyAndLoadNativeLibrary(String name) throws Exception {
        var lib = System.getProperty("java.io.tmpdir") + File.separator + name;
        try (InputStream in = MediaPlayer.class.getResourceAsStream("/" + name); var fo = new FileOutputStream(lib)) {
            if (in == null) {
                throw new IllegalStateException("缺少 " + name + " 资源");
            }

            fo.write(in.readAllBytes());
        }

        System.load(lib);
    }

    public static native void init(long proc);
}
