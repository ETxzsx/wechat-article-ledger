import javafx.application.Application;
import javafx.application.Platform;
import javafx.scene.Scene;
import javafx.scene.control.Label;
import javafx.scene.layout.StackPane;
import javafx.scene.web.WebView;
import javafx.stage.Stage;

/**
 * 桌面 UI 壳：只负责窗口，业务全部由 C++ 内核（127.0.0.1:8780）提供。
 */
public class DesktopApp extends Application {

    public static void main(String[] args) {
        launch(args);
    }

    @Override
    public void start(Stage stage) {
        WebView web = new WebView();
        web.getEngine().load("http://127.0.0.1:8780/");

        // 页面加载失败时给出提示（内核没起来）
        web.getEngine().getLoadWorker().stateProperty().addListener((obs, oldS, newS) -> {
            if (newS == javafx.concurrent.Worker.State.FAILED) {
                Label tip = new Label("内核未启动或端口 8780 被占用。\n请先运行 推文台账.exe。");
                tip.setStyle("-fx-font-size:15px; -fx-text-fill:#9C2B24;");
                StackPane pane = new StackPane(tip);
                stage.setScene(new Scene(pane, 720, 460));
            }
        });

        Scene scene = new Scene(web, 1380, 900);
        stage.setTitle("推文台账 · 公众号链接自动记录");
        stage.setScene(scene);
        stage.show();
    }
}
