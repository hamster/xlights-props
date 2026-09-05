package com.xlightsprops.ddpdebugger.ui;

import com.xlightsprops.ddpdebugger.net.DdpSender;
import com.xlightsprops.ddpdebugger.settings.AppSettings;

import javafx.application.Platform;
import javafx.collections.FXCollections;
import javafx.geometry.Insets;
import javafx.scene.control.Button;
import javafx.scene.control.CheckBox;
import javafx.scene.control.ChoiceBox;
import javafx.scene.control.ColorPicker;
import javafx.scene.control.Label;
import javafx.scene.control.Spinner;
import javafx.scene.control.TextField;
import javafx.scene.layout.BorderPane;
import javafx.scene.layout.ColumnConstraints;
import javafx.scene.layout.GridPane;
import javafx.scene.layout.Priority;
import javafx.scene.layout.VBox;
import javafx.scene.paint.Color;
import javafx.util.StringConverter;

/** The "Sender" tab: a bench-test DDP transmitter for exercising the receiver (or a real device). */
public final class SenderPane extends BorderPane {

    private final TextField hostField = new TextField("127.0.0.1");
    private final Spinner<Integer> portSpinner = new Spinner<>(1, 65535, 4048);
    private final Spinner<Integer> destinationIdSpinner = new Spinner<>(0, 255, 1);
    private final Spinner<Integer> pixelCountSpinner = new Spinner<>(1, 5000, 50);
    private final ChoiceBox<Integer> bytesPerPixelChoice = new ChoiceBox<>(FXCollections.observableArrayList(3, 4));
    private final Spinner<Integer> startOffsetSpinner = new Spinner<>(0, 1_000_000, 0);
    private final Spinner<Double> frameRateSpinner = new Spinner<>(0.1, 60.0, 1.0, 0.5);
    private final ChoiceBox<DdpSender.Pattern> patternChoice =
            new ChoiceBox<>(FXCollections.observableArrayList(DdpSender.Pattern.values()));
    private final ColorPicker solidColorPicker = new ColorPicker(Color.RED);
    private final CheckBox marchingCheck = new CheckBox("March pattern along the string");
    private final Button startStopButton = new Button("Start sending");
    private final Label statusLabel = new Label("Not sending");
    private final GridCanvas previewCanvas = new GridCanvas();

    private final DdpSender sender;

    public SenderPane(AppSettings settings) {
        sender = new DdpSender(
                error -> Platform.runLater(() -> statusLabel.setText(error)),
                frames -> Platform.runLater(() -> statusLabel.setText("Sending... " + frames + " frames sent")),
                frame -> Platform.runLater(() -> renderPreview(frame))
        );

        patternChoice.setConverter(new StringConverter<>() {
            @Override
            public String toString(DdpSender.Pattern p) {
                if (p == null) return "";
                return switch (p) {
                    case SOLID -> "Solid color";
                    case PURE_RED -> "Pure red";
                    case PURE_GREEN -> "Pure green";
                    case PURE_BLUE -> "Pure blue";
                    case RGBW_CYCLE -> "Red / Green / Blue / White";
                    case RGBWK_CYCLE -> "Red / Green / Blue / White / Black";
                };
            }

            @Override
            public DdpSender.Pattern fromString(String s) {
                return null;
            }
        });

        for (Spinner<?> spinner : new Spinner<?>[]{portSpinner, destinationIdSpinner, pixelCountSpinner, startOffsetSpinner, frameRateSpinner}) {
            spinner.setEditable(true);
        }

        applySettingsToControls(settings);
        setLeft(buildControls());
        setCenter(buildPreview());

        patternChoice.valueProperty().addListener((o, a, b) ->
                solidColorPicker.setDisable(b != DdpSender.Pattern.SOLID));
        startStopButton.setOnAction(e -> toggleSending());
        wireLiveUpdateListeners();
    }

    /** Pushes the current form values straight to the sender while it's running, so pattern/color/rate
     *  changes take effect on the next frame instead of requiring a stop/start. */
    private void wireLiveUpdateListeners() {
        hostField.textProperty().addListener((o, a, b) -> pushConfigIfRunning());
        portSpinner.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        destinationIdSpinner.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        pixelCountSpinner.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        bytesPerPixelChoice.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        startOffsetSpinner.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        frameRateSpinner.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        patternChoice.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        solidColorPicker.valueProperty().addListener((o, a, b) -> pushConfigIfRunning());
        marchingCheck.selectedProperty().addListener((o, a, b) -> pushConfigIfRunning());
    }

    private void pushConfigIfRunning() {
        if (sender.isRunning()) {
            sender.updateConfig(buildConfig());
        }
    }

    private VBox buildControls() {
        GridPane form = new GridPane();
        form.setHgap(8);
        form.setVgap(6);
        ColumnConstraints labelCol = new ColumnConstraints();
        labelCol.setMinWidth(150);
        ColumnConstraints controlCol = new ColumnConstraints();
        controlCol.setHgrow(Priority.ALWAYS);
        form.getColumnConstraints().addAll(labelCol, controlCol);
        int r = 0;
        form.addRow(r++, new Label("Target host:"), hostField);
        form.addRow(r++, new Label("Target port:"), portSpinner);
        form.addRow(r++, new Label("Destination ID:"), destinationIdSpinner);
        form.addRow(r++, new Label("Pixel count:"), pixelCountSpinner);
        form.addRow(r++, new Label("Bytes/pixel:"), bytesPerPixelChoice);
        form.addRow(r++, new Label("Start byte offset:"), startOffsetSpinner);
        form.addRow(r++, new Label("Frame rate (fps):"), frameRateSpinner);
        form.addRow(r++, new Label("Pattern:"), patternChoice);
        form.addRow(r++, new Label("Solid color:"), solidColorPicker);

        VBox box = new VBox(10, form, marchingCheck, startStopButton, statusLabel);
        box.setPadding(new Insets(10));
        box.setPrefWidth(320);
        box.setMinWidth(320);
        return box;
    }

    private VBox buildPreview() {
        VBox box = new VBox(8, new Label("Live preview of what's being transmitted:"), previewCanvas);
        box.setPadding(new Insets(10));
        return box;
    }

    private void renderPreview(byte[] frame) {
        int pixels = pixelCountSpinner.getValue();
        int cols = Math.min(pixels, 64);
        int rows = (int) Math.ceil(pixels / (double) cols);
        int bpp = bytesPerPixelChoice.getValue() == null ? 3 : bytesPerPixelChoice.getValue();
        previewCanvas.configure(cols, rows, 14, 0, bpp, false, false);
        previewCanvas.render(frame);
    }

    private void toggleSending() {
        if (sender.isRunning()) {
            sender.stop();
            startStopButton.setText("Start sending");
            statusLabel.setText("Stopped");
        } else {
            sender.start(buildConfig());
            startStopButton.setText("Stop sending");
        }
    }

    private DdpSender.Config buildConfig() {
        DdpSender.Config cfg = new DdpSender.Config();
        cfg.targetHost = hostField.getText().trim();
        cfg.targetPort = portSpinner.getValue();
        cfg.destinationId = destinationIdSpinner.getValue();
        cfg.pixelCount = pixelCountSpinner.getValue();
        cfg.bytesPerPixel = bytesPerPixelChoice.getValue() == null ? 3 : bytesPerPixelChoice.getValue();
        cfg.startByteOffset = startOffsetSpinner.getValue();
        cfg.frameRateFps = frameRateSpinner.getValue();
        cfg.pattern = patternChoice.getValue() == null ? DdpSender.Pattern.SOLID : patternChoice.getValue();
        Color c = solidColorPicker.getValue();
        cfg.solidR = (int) Math.round(c.getRed() * 255);
        cfg.solidG = (int) Math.round(c.getGreen() * 255);
        cfg.solidB = (int) Math.round(c.getBlue() * 255);
        cfg.solidW = 0;
        cfg.marching = marchingCheck.isSelected();
        return cfg;
    }

    private void applySettingsToControls(AppSettings settings) {
        hostField.setText(settings.senderTargetHost);
        portSpinner.getValueFactory().setValue(settings.senderTargetPort);
        destinationIdSpinner.getValueFactory().setValue(settings.senderDestinationId);
        pixelCountSpinner.getValueFactory().setValue(settings.senderPixelCount);
        bytesPerPixelChoice.setValue(settings.senderBytesPerPixel);
        startOffsetSpinner.getValueFactory().setValue(settings.senderStartByteOffset);
        frameRateSpinner.getValueFactory().setValue(settings.senderFrameRateFps);
        patternChoice.setValue(parsePattern(settings.senderPattern));
        solidColorPicker.setValue(Color.rgb(settings.senderColorR, settings.senderColorG, settings.senderColorB));
        marchingCheck.setSelected(settings.senderMarching);
        solidColorPicker.setDisable(patternChoice.getValue() != DdpSender.Pattern.SOLID);
    }

    private static DdpSender.Pattern parsePattern(String name) {
        try {
            return DdpSender.Pattern.valueOf(name);
        } catch (Exception e) {
            return DdpSender.Pattern.SOLID;
        }
    }

    public void writeBackTo(AppSettings settings) {
        settings.senderTargetHost = hostField.getText().trim();
        settings.senderTargetPort = portSpinner.getValue();
        settings.senderDestinationId = destinationIdSpinner.getValue();
        settings.senderPixelCount = pixelCountSpinner.getValue();
        settings.senderBytesPerPixel = bytesPerPixelChoice.getValue() == null ? 3 : bytesPerPixelChoice.getValue();
        settings.senderStartByteOffset = startOffsetSpinner.getValue();
        settings.senderFrameRateFps = frameRateSpinner.getValue();
        settings.senderPattern = (patternChoice.getValue() == null ? DdpSender.Pattern.SOLID : patternChoice.getValue()).name();
        Color c = solidColorPicker.getValue();
        settings.senderColorR = (int) Math.round(c.getRed() * 255);
        settings.senderColorG = (int) Math.round(c.getGreen() * 255);
        settings.senderColorB = (int) Math.round(c.getBlue() * 255);
        settings.senderMarching = marchingCheck.isSelected();
    }

    public void shutdown() {
        sender.stop();
    }
}
