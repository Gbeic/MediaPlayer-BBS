package net.hacker.mediaplayer;

import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.network.chat.Component;
import net.minecraft.world.entity.Entity;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.InputStream;
import java.lang.ref.Cleaner;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.function.BiFunction;

public final class MediaPlayer {
    public static final String MOD_ID = "mediaplayer";
    static final Cleaner cleaner = Cleaner.create();
    public static BiFunction<Audio, Entity, SoundInstance> audioFactory;
    public static final Logger LOGGER = LoggerFactory.getLogger(MediaPlayer.class);
    private static final boolean NATIVE_AVAILABLE;
    private static final String UNAVAILABLE_REASON;
    private static Path nativeTempDir;

    static {
        boolean available = false;
        String reason = "";

        if (!System.getProperty("os.name").toLowerCase().contains("windows")) {
            reason = "当前系统不支持 MediaPlayer-BBS native 后端";
        } else {
            try {
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
        Path lib = getNativeTempDir().resolve(name);
        try (InputStream in = MediaPlayer.class.getResourceAsStream("/" + name)) {
            if (in == null) {
                throw new IllegalStateException("缺少 " + name + " 资源");
            }

            Files.copy(in, lib, StandardCopyOption.REPLACE_EXISTING);
        }

        lib.toFile().deleteOnExit();
        System.load(lib.toAbsolutePath().toString());
    }

    private static Path createNativeTempDir() throws Exception {
        Path dir = Files.createTempDirectory("mediaplayer-bbs-" + ProcessHandle.current().pid() + "-");
        dir.toFile().deleteOnExit();
        return dir;
    }

    private static Path getNativeTempDir() throws Exception {
        if (nativeTempDir == null) {
            nativeTempDir = createNativeTempDir();
        }

        return nativeTempDir;
    }

    public static native void init(long proc);
}
