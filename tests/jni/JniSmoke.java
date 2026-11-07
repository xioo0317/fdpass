import com.Android.MoFlash.data.FdPass;

/** JVM smoke：起线程跑 serveFdLoop，用 python 客户端收两次 fd，再 stop 校验返回值。 */
public class JniSmoke {
    public static void main(String[] args) throws Exception {
        String name = args[0];
        FdPass p = new FdPass();

        final int[] served = { -999 };
        Thread t = new Thread(() -> served[0] = p.serveFdLoop(1, name), "fdpass-serve");
        t.setDaemon(true);
        t.start();

        Thread.sleep(2500);          // 留给 python 客户端完成 2 次连接
        p.stopServeLoop();
        t.join(2000);

        System.out.println("JNI_SERVED=" + served[0]);
        if (served[0] != 2) {
            System.err.println("期望 serveFdLoop 返回 2");
            System.exit(2);
        }

        // 非法 name 应立即返回 -2（验证 jstring → C 字符串与错误码路径）
        int err = p.sendFd(1, "no_at_prefix");
        System.out.println("JNI_SEND_ERR=" + err);
        if (err != -2) {
            System.err.println("期望 sendFd 返回 -2");
            System.exit(3);
        }
        System.out.println("JNI_SEND_ERR_OK");
    }
}
