import javafx.application.Application;
import javafx.application.Platform;
import javafx.scene.Scene;
import javafx.scene.control.Alert;
import javafx.scene.web.WebView;
import javafx.stage.Stage;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * 桌面软件入口：JavaFX 窗口 + 本地服务 + C++ 记录器托管。
 * 双击即打开窗口，C++ 记录器在后台自动抓链接，界面实时显示。
 */
public class DesktopApp extends Application {
    private Process watcher;

    public static void main(String[] args) {
        launch(args);
    }

    @Override
    public void start(Stage stage) {
        try {
            LedgerFrontend.init();
            LedgerFrontend.startServer();
        } catch (Exception e) {
            Alert a = new Alert(Alert.AlertType.ERROR);
            a.setTitle("推文台账");
            a.setHeaderText("本地服务启动失败（端口 8780 可能被占用）");
            a.setContentText(String.valueOf(e));
            a.showAndWait();
            Platform.exit();
            return;
        }

        watcher = startWatcher();

        WebView web = new WebView();
        web.getEngine().load("http://127.0.0.1:" + LedgerFrontend.PORT + "/");

        Scene scene = new Scene(web, 1380, 900);
        stage.setTitle("推文台账 · 公众号链接自动记录");
        stage.setScene(scene);
        stage.setOnCloseRequest(e -> shutdown());
        stage.show();
    }

    /** 静默拉起 C++ 记录器（无控制台窗口，自己写日志） */
    private Process startWatcher() {
        try {
            Path exe = LedgerFrontend.baseDir.resolve("cpp").resolve("wechat_link_watcher.exe");
            if (!Files.isRegularFile(exe)) return null;
            ProcessBuilder pb = new ProcessBuilder(exe.toString());
            pb.directory(exe.getParent().toFile());
            pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
            pb.redirectError(ProcessBuilder.Redirect.DISCARD);
            return pb.start();
        } catch (Exception e) {
            return null;
        }
    }

    private void shutdown() {
        if (watcher != null) {
            watcher.destroy();
            try { watcher.waitFor(3, java.util.concurrent.TimeUnit.SECONDS); } catch (Exception ignored) {}
            watcher = null;
        }
        Platform.exit();
    }

    @Override
    public void stop() {
        shutdown();
    }
}
