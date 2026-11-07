package com.Android.MoFlash.data;

/**
 * 与 OZJFLASH 应用里 Kotlin 的 class FdPass 完全同签名（包名、类名、方法名、参数类型）。
 * 用于验证：新 libfdpass.so 能被 JVM 按短名解析，应用侧无需改任何代码。
 */
public class FdPass {
    static { System.loadLibrary("fdpass"); }

    public native int serveFdLoop(int fd, String name);
    public native int sendFd(int fd, String name);
    public native void stopServeLoop();
}
