package com.xlightsprops.ddpdebugger;

import com.xlightsprops.ddpdebugger.settings.AppSettings;
import com.xlightsprops.ddpdebugger.settings.SettingsStore;
import com.xlightsprops.ddpdebugger.ui.ReceiverPane;
import com.xlightsprops.ddpdebugger.ui.SenderPane;

import javafx.application.Application;
import javafx.scene.Scene;
import javafx.scene.control.Tab;
import javafx.scene.control.TabPane;
import javafx.stage.Stage;

public final class Main extends Application {

    private AppSettings settings;
    private ReceiverPane receiverPane;
    private SenderPane senderPane;

    @Override
    public void start(Stage stage) {
        settings = SettingsStore.load();

        receiverPane = new ReceiverPane(settings);
        senderPane = new SenderPane(settings);

        Tab receiverTab = new Tab("Receiver", receiverPane);
        receiverTab.setClosable(false);
        Tab senderTab = new Tab("Sender", senderPane);
        senderTab.setClosable(false);

        TabPane tabs = new TabPane(receiverTab, senderTab);

        Scene scene = new Scene(tabs, 1100, 750);
        stage.setTitle("DDP Debugger");
        stage.setScene(scene);
        stage.setOnCloseRequest(e -> shutdown());
        stage.show();
    }

    private void shutdown() {
        receiverPane.writeBackTo(settings);
        senderPane.writeBackTo(settings);
        SettingsStore.save(settings);
        receiverPane.shutdown();
        senderPane.shutdown();
    }

    public static void main(String[] args) {
        launch(args);
    }
}
