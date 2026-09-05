package com.xlightsprops.ddpdebugger.ui;

import com.xlightsprops.ddpdebugger.ddp.DdpConstants;
import com.xlightsprops.ddpdebugger.ddp.DdpPacket;
import com.xlightsprops.ddpdebugger.model.SourceKey;
import com.xlightsprops.ddpdebugger.model.SourceState;
import com.xlightsprops.ddpdebugger.net.DdpListener;
import com.xlightsprops.ddpdebugger.settings.AppSettings;

import javafx.animation.AnimationTimer;
import javafx.application.Platform;
import javafx.beans.property.ReadOnlyObjectWrapper;
import javafx.collections.FXCollections;
import javafx.collections.ObservableList;
import javafx.geometry.Insets;
import javafx.geometry.Pos;
import javafx.scene.control.Button;
import javafx.scene.control.CheckBox;
import javafx.scene.control.ChoiceBox;
import javafx.scene.control.ComboBox;
import javafx.scene.control.Label;
import javafx.scene.control.ScrollPane;
import javafx.scene.control.Separator;
import javafx.scene.control.Spinner;
import javafx.scene.control.Tab;
import javafx.scene.control.TabPane;
import javafx.scene.control.TableColumn;
import javafx.scene.control.TableRow;
import javafx.scene.control.TableView;
import javafx.scene.control.TextField;
import javafx.scene.layout.BorderPane;
import javafx.scene.layout.ColumnConstraints;
import javafx.scene.layout.GridPane;
import javafx.scene.layout.HBox;
import javafx.scene.layout.Priority;
import javafx.scene.layout.VBox;
import javafx.scene.paint.Color;
import javafx.scene.shape.Circle;

import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.function.Function;

/** The "Receiver" tab: listens for DDP traffic and visualizes it. */
public final class ReceiverPane extends BorderPane {

    private static final int MAX_LOG_ROWS = 500;
    private static final int MAX_DRAINED_PER_TICK = 2000;
    private static final DateTimeFormatter TIME_FMT = DateTimeFormatter.ofPattern("HH:mm:ss.SSS").withZone(ZoneId.systemDefault());

    private final ConcurrentLinkedQueue<DdpPacket> incoming = new ConcurrentLinkedQueue<>();
    private final Map<SourceKey, SourceState> sources = new LinkedHashMap<>();
    private final ObservableList<SourceKey> sourceItems = FXCollections.observableArrayList();
    private final ObservableList<DdpPacket> logRows = FXCollections.observableArrayList();

    private final GridCanvas gridCanvas = new GridCanvas();
    private final ComboBox<SourceKey> sourceSelector = new ComboBox<>(sourceItems);
    private final Label statusLabel = new Label("Not listening");
    private final Circle statusDot = new Circle(5, Color.GRAY);
    private final Label warningLabel = new Label();

    private final Spinner<Integer> gridWidthSpinner = new Spinner<>(1, 200, 16);
    private final Spinner<Integer> gridHeightSpinner = new Spinner<>(1, 200, 16);
    private final Spinner<Integer> boxSizeSpinner = new Spinner<>(2, 100, 12);
    private final Spinner<Integer> startOffsetSpinner = new Spinner<>(0, 1_000_000, 0);
    private final ChoiceBox<Integer> bytesPerPixelChoice = new ChoiceBox<>(FXCollections.observableArrayList(3, 4));
    private final CheckBox addressOverlayCheck = new CheckBox("Show channel address");
    private final CheckBox dataOverlayCheck = new CheckBox("Show pixel data");
    private final TextField portField = new TextField("4048");
    private final Button applyPortButton = new Button("Apply");

    private final Label lastSeenLabel = new Label("-");
    private final Label packetsLabel = new Label("0");
    private final Label rateLabel = new Label("0 pps / 0 B/s");
    private final Label sizeLabel = new Label("-");
    private final Label malformedLabel = new Label("0");
    private final Label duplicateLabel = new Label("0");
    private final Label flagsLabel = new Label("-");
    private final Label pixelInfoLabel = new Label("-");

    private final TableView<DdpPacket> logTable = new TableView<>(logRows);

    private DdpListener listener;

    public ReceiverPane(AppSettings settings) {
        applySettingsToControls(settings);

        setLeft(buildControls());
        setCenter(buildCenter());
        setBottom(buildBottom());

        sourceSelector.setOnAction(e -> renderSelectedSource());
        wireControlListeners();
        renderSelectedSource();

        AnimationTimer timer = new AnimationTimer() {
            @Override
            public void handle(long now) {
                drainIncoming();
            }
        };
        timer.start();

        restartListener(settings.listenPort);
    }

    private VBox buildControls() {
        VBox box = new VBox(10);
        box.setPadding(new Insets(10));
        box.setPrefWidth(320);
        box.setMinWidth(320);

        HBox portRow = new HBox(6, new Label("Listen port:"), portField, applyPortButton);
        portRow.setAlignment(Pos.CENTER_LEFT);
        portField.setPrefWidth(70);

        HBox statusRow = new HBox(6, statusDot, statusLabel);
        statusRow.setAlignment(Pos.CENTER_LEFT);

        gridWidthSpinner.setEditable(true);
        gridHeightSpinner.setEditable(true);
        boxSizeSpinner.setEditable(true);
        startOffsetSpinner.setEditable(true);

        GridPane gridConfig = new GridPane();
        gridConfig.setHgap(8);
        gridConfig.setVgap(6);
        ColumnConstraints labelCol = new ColumnConstraints();
        labelCol.setMinWidth(150);
        ColumnConstraints controlCol = new ColumnConstraints();
        controlCol.setHgrow(Priority.ALWAYS);
        gridConfig.getColumnConstraints().addAll(labelCol, controlCol);
        gridConfig.addRow(0, new Label("Grid width:"), gridWidthSpinner);
        gridConfig.addRow(1, new Label("Grid height:"), gridHeightSpinner);
        gridConfig.addRow(2, new Label("Box size (px):"), boxSizeSpinner);
        gridConfig.addRow(3, new Label("Start byte offset:"), startOffsetSpinner);
        gridConfig.addRow(4, new Label("Bytes/pixel:"), bytesPerPixelChoice);

        sourceSelector.setMaxWidth(Double.MAX_VALUE);
        sourceSelector.setPromptText("(no sources yet)");
        VBox sourceBox = new VBox(4, new Label("Active source:"), sourceSelector);

        box.getChildren().addAll(
                new Label("Network"), portRow, statusRow, new Separator(),
                new Label("Grid"), gridConfig, new Separator(),
                sourceBox, new Separator(),
                new Label("Overlays"), addressOverlayCheck, dataOverlayCheck
        );
        return box;
    }

    private VBox buildCenter() {
        VBox center = new VBox(8);
        center.setPadding(new Insets(10));
        warningLabel.setStyle("-fx-text-fill: #cc6600; -fx-font-weight: bold;");
        warningLabel.setWrapText(true);
        warningLabel.setVisible(false);
        warningLabel.setManaged(false);

        ScrollPane scroll = new ScrollPane(gridCanvas);
        VBox.setVgrow(scroll, Priority.ALWAYS);

        center.getChildren().addAll(warningLabel, scroll);
        return center;
    }

    private TabPane buildBottom() {
        setupLogTable();

        GridPane diag = new GridPane();
        diag.setHgap(10);
        diag.setVgap(6);
        diag.setPadding(new Insets(10));
        int r = 0;
        diag.addRow(r++, new Label("Last seen:"), lastSeenLabel);
        diag.addRow(r++, new Label("Total packets:"), packetsLabel);
        diag.addRow(r++, new Label("Rate:"), rateLabel);
        diag.addRow(r++, new Label("Packet size (min / last / max):"), sizeLabel);
        diag.addRow(r++, new Label("Malformed packets:"), malformedLabel);
        diag.addRow(r++, new Label("Duplicate sequence numbers:"), duplicateLabel);
        diag.addRow(r++, new Label("Flags seen (push / query / reply / storage / timecode):"), flagsLabel);
        diag.addRow(r++, new Label("Last frame:"), pixelInfoLabel);

        Tab logTab = new Tab("Packet Log", logTable);
        logTab.setClosable(false);
        Tab diagTab = new Tab("Diagnostics", diag);
        diagTab.setClosable(false);

        TabPane tabs = new TabPane(logTab, diagTab);
        tabs.setPrefHeight(220);
        return tabs;
    }

    private void setupLogTable() {
        TableColumn<DdpPacket, String> timeCol = column("Time", p -> TIME_FMT.format(p.receivedAt()));
        TableColumn<DdpPacket, String> sourceCol = column("Source", p -> p.sourceAddress().getHostAddress() + ":" + p.sourcePort());
        TableColumn<DdpPacket, String> sizeCol = column("Size", p -> p.totalSize() + " B");
        TableColumn<DdpPacket, String> seqCol = column("Seq", p -> p.header() == null ? "-" : String.valueOf(p.header().sequence()));
        TableColumn<DdpPacket, String> flagsCol = column("Flags", this::describeFlags);
        TableColumn<DdpPacket, String> typeCol = column("Type", p -> p.header() == null ? "-" : DdpConstants.describeDataType(p.header().dataType()));
        TableColumn<DdpPacket, String> idCol = column("Dest ID", p -> p.header() == null ? "-" : DdpConstants.describeId(p.header().id()));
        TableColumn<DdpPacket, String> offsetCol = column("Offset", p -> p.header() == null ? "-" : String.valueOf(p.header().dataOffset()));
        TableColumn<DdpPacket, String> lenCol = column("Length", p -> p.header() == null ? "-" : p.header().dataLength() + " B");
        TableColumn<DdpPacket, String> pixelsCol = column("Pixels", p -> p.header() == null ? "-" : String.valueOf(p.pixelCount(currentBytesPerPixel())));

        logTable.getColumns().setAll(timeCol, sourceCol, sizeCol, seqCol, flagsCol, typeCol, idCol, offsetCol, lenCol, pixelsCol);
        logTable.setRowFactory(tv -> new TableRow<>() {
            @Override
            protected void updateItem(DdpPacket item, boolean empty) {
                super.updateItem(item, empty);
                setStyle(!empty && item != null && item.isMalformed() ? "-fx-background-color: #4a1f1f;" : "");
            }
        });
    }

    private static TableColumn<DdpPacket, String> column(String title, Function<DdpPacket, String> extractor) {
        TableColumn<DdpPacket, String> col = new TableColumn<>(title);
        col.setCellValueFactory(cd -> new ReadOnlyObjectWrapper<>(extractor.apply(cd.getValue())));
        return col;
    }

    private String describeFlags(DdpPacket p) {
        if (p.header() == null) return "malformed: " + p.malformedReason();
        var h = p.header();
        StringBuilder sb = new StringBuilder();
        if (h.isPush()) sb.append("PUSH ");
        if (h.isQuery()) sb.append("QUERY ");
        if (h.isReply()) sb.append("REPLY ");
        if (h.isStorage()) sb.append("STORAGE ");
        if (h.isTimecodeFlagSet()) sb.append("TIME ");
        if (p.isMalformed()) sb.append("[").append(p.malformedReason()).append("]");
        String result = sb.toString().trim();
        return result.isEmpty() ? "-" : result;
    }

    private void wireControlListeners() {
        Runnable onGridChange = this::renderSelectedSource;
        gridWidthSpinner.valueProperty().addListener((o, a, b) -> onGridChange.run());
        gridHeightSpinner.valueProperty().addListener((o, a, b) -> onGridChange.run());
        boxSizeSpinner.valueProperty().addListener((o, a, b) -> onGridChange.run());
        startOffsetSpinner.valueProperty().addListener((o, a, b) -> onGridChange.run());
        bytesPerPixelChoice.valueProperty().addListener((o, a, b) -> onGridChange.run());
        addressOverlayCheck.selectedProperty().addListener((o, a, b) -> onGridChange.run());
        dataOverlayCheck.selectedProperty().addListener((o, a, b) -> onGridChange.run());

        applyPortButton.setOnAction(e -> {
            try {
                int port = Integer.parseInt(portField.getText().trim());
                restartListener(port);
            } catch (NumberFormatException ex) {
                statusLabel.setText("Invalid port");
            }
        });
    }

    private void restartListener(int port) {
        if (listener != null) {
            listener.stop();
        }
        portField.setText(String.valueOf(port));
        listener = new DdpListener(port, incoming::add,
                error -> Platform.runLater(() -> {
                    statusDot.setFill(Color.RED);
                    statusLabel.setText(error);
                }));
        listener.start();
        statusDot.setFill(Color.LIMEGREEN);
        statusLabel.setText("Listening on UDP " + port);
    }

    private void drainIncoming() {
        DdpPacket packet;
        boolean touchedSelected = false;
        int processed = 0;
        while (processed < MAX_DRAINED_PER_TICK && (packet = incoming.poll()) != null) {
            processed++;
            logRows.add(0, packet);
            if (logRows.size() > MAX_LOG_ROWS) {
                logRows.remove(logRows.size() - 1);
            }

            if (packet.header() == null) {
                continue;
            }

            SourceKey key = new SourceKey(packet.sourceAddress(), packet.sourcePort(), packet.header().id());
            SourceState state = sources.computeIfAbsent(key, k -> {
                sourceItems.add(k);
                return new SourceState(k);
            });
            state.stats().record(packet);
            if (!packet.isMalformed() && packet.data().length > 0) {
                state.frameBuffer().write(packet.header().dataOffset(), packet.data(), 0, packet.data().length);
            }
            state.setLastPixelCount(state.frameBuffer().length() / Math.max(1, currentBytesPerPixel()));
            state.setLastDataType(packet.header().dataType());

            if (sourceSelector.getValue() == null) {
                sourceSelector.setValue(key);
            }
            if (key.equals(sourceSelector.getValue())) {
                touchedSelected = true;
            }
        }
        if (touchedSelected) {
            renderSelectedSource();
        }
    }

    private void renderSelectedSource() {
        gridCanvas.configure(gridWidthSpinner.getValue(), gridHeightSpinner.getValue(), boxSizeSpinner.getValue(),
                startOffsetSpinner.getValue(), currentBytesPerPixel(), addressOverlayCheck.isSelected(), dataOverlayCheck.isSelected());

        SourceKey key = sourceSelector.getValue();
        SourceState state = key == null ? null : sources.get(key);
        if (state == null) {
            clearDiagnostics();
            warningLabel.setVisible(false);
            warningLabel.setManaged(false);
            return;
        }

        gridCanvas.render(state.frameBuffer().snapshot());
        updateDiagnostics(state);

        int capacity = gridWidthSpinner.getValue() * gridHeightSpinner.getValue();
        int received = state.lastPixelCount();
        if (received > capacity) {
            warningLabel.setText("Source has " + received + " pixels but the grid only shows " + capacity
                    + " (" + gridWidthSpinner.getValue() + "×" + gridHeightSpinner.getValue()
                    + ") -- increase the grid size to see them all.");
            warningLabel.setVisible(true);
            warningLabel.setManaged(true);
        } else {
            warningLabel.setVisible(false);
            warningLabel.setManaged(false);
        }
    }

    private void updateDiagnostics(SourceState state) {
        var stats = state.stats();
        lastSeenLabel.setText(stats.lastSeen() == null ? "-" : formatAgo(stats.lastSeen()));
        packetsLabel.setText(String.valueOf(stats.totalPackets()));
        rateLabel.setText(String.format("%.1f pps / %.0f B/s", stats.packetsPerSecond(), stats.bytesPerSecond()));
        sizeLabel.setText(stats.minSize() + " / " + stats.lastSize() + " / " + stats.maxSize() + " B");
        malformedLabel.setText(String.valueOf(stats.malformedPackets()));
        duplicateLabel.setText(String.valueOf(stats.duplicatePackets()));
        flagsLabel.setText(stats.pushCount() + " / " + stats.queryCount() + " / " + stats.replyCount() + " / "
                + stats.storageCount() + " / " + stats.timecodeCount());
        pixelInfoLabel.setText(state.lastPixelCount() + " px, " + DdpConstants.describeDataType(state.lastDataType()));
    }

    private void clearDiagnostics() {
        lastSeenLabel.setText("-");
        packetsLabel.setText("0");
        rateLabel.setText("0 pps / 0 B/s");
        sizeLabel.setText("-");
        malformedLabel.setText("0");
        duplicateLabel.setText("0");
        flagsLabel.setText("-");
        pixelInfoLabel.setText("-");
    }

    private static String formatAgo(Instant instant) {
        long ms = Duration.between(instant, Instant.now()).toMillis();
        if (ms < 1000) return "just now";
        return TIME_FMT.format(instant) + " (" + (ms / 1000) + "s ago)";
    }

    private int currentBytesPerPixel() {
        Integer v = bytesPerPixelChoice.getValue();
        return v == null ? 3 : v;
    }

    private void applySettingsToControls(AppSettings settings) {
        gridWidthSpinner.getValueFactory().setValue(settings.gridWidth);
        gridHeightSpinner.getValueFactory().setValue(settings.gridHeight);
        boxSizeSpinner.getValueFactory().setValue(settings.boxSize);
        startOffsetSpinner.getValueFactory().setValue(settings.startByteOffset);
        bytesPerPixelChoice.setValue(settings.bytesPerPixel);
        addressOverlayCheck.setSelected(settings.showAddressOverlay);
        dataOverlayCheck.setSelected(settings.showDataOverlay);
        portField.setText(String.valueOf(settings.listenPort));
    }

    public void writeBackTo(AppSettings settings) {
        settings.gridWidth = gridWidthSpinner.getValue();
        settings.gridHeight = gridHeightSpinner.getValue();
        settings.boxSize = boxSizeSpinner.getValue();
        settings.startByteOffset = startOffsetSpinner.getValue();
        settings.bytesPerPixel = currentBytesPerPixel();
        settings.showAddressOverlay = addressOverlayCheck.isSelected();
        settings.showDataOverlay = dataOverlayCheck.isSelected();
        try {
            settings.listenPort = Integer.parseInt(portField.getText().trim());
        } catch (NumberFormatException ignored) {
            // keep whatever was already in settings
        }
    }

    public void shutdown() {
        if (listener != null) listener.stop();
    }
}
