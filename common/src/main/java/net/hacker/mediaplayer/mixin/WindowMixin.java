package net.hacker.mediaplayer.mixin;

import com.mojang.blaze3d.platform.DisplayData;
import com.mojang.blaze3d.platform.ScreenManager;
import com.mojang.blaze3d.platform.Window;
import com.mojang.blaze3d.platform.WindowEventHandler;
import net.hacker.mediaplayer.MediaPlayer;
import org.lwjgl.glfw.GLFW;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Window.class)
public class WindowMixin {
    /**
     * 注入窗口初始化结束点，用当前 OpenGL 过程地址初始化 native 渲染桥。
     * BBS++ 把本模组作为可选视频后端使用，因此 native 不可用时只记录状态并跳过初始化，
     * 避免客户端因为缺少后端而在启动阶段崩溃。
     */
    @Inject(method = "<init>", at = @At("RETURN"))
    private void init(WindowEventHandler eventHandler, ScreenManager screenManager, DisplayData displayData, String preferredFullscreenVideoMode, String title, CallbackInfo ci) {
        if (MediaPlayer.isNativeAvailable()) {
            MediaPlayer.init(GLFW.Functions.GetProcAddress);
        }
    }
}
