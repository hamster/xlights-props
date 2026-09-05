package com.xlightsprops.ddpdebugger.settings;

import java.io.IOException;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

/** Loads/saves {@link AppSettings} as human-readable JSON in the platform's per-user config dir. */
public final class SettingsStore {
    private SettingsStore() {}

    public static Path settingsFile() {
        String appData = System.getenv("APPDATA");
        Path baseDir = (appData != null && !appData.isBlank())
                ? Path.of(appData, "DDPDebugger")
                : Path.of(System.getProperty("user.home"), ".ddpdebugger");
        return baseDir.resolve("settings.json");
    }

    public static AppSettings load() {
        AppSettings settings = new AppSettings();
        Path file = settingsFile();
        if (!Files.isRegularFile(file)) {
            return settings;
        }
        try {
            String json = Files.readString(file, StandardCharsets.UTF_8);
            Map<String, Object> values = JsonUtil.parseFlatObject(json);
            applyValues(settings, values);
        } catch (Exception e) {
            System.err.println("Could not load settings from " + file + ", using defaults: " + e.getMessage());
        }
        return settings;
    }

    public static void save(AppSettings settings) {
        Path file = settingsFile();
        try {
            Files.createDirectories(file.getParent());
            Files.writeString(file, JsonUtil.toJson(settings), StandardCharsets.UTF_8);
        } catch (IOException e) {
            System.err.println("Could not save settings to " + file + ": " + e.getMessage());
        }
    }

    private static void applyValues(AppSettings settings, Map<String, Object> values) {
        for (Field f : AppSettings.class.getFields()) {
            Object raw = values.get(f.getName());
            if (raw == null) continue;
            try {
                Class<?> type = f.getType();
                if (type == int.class) f.setInt(settings, ((Number) raw).intValue());
                else if (type == double.class) f.setDouble(settings, ((Number) raw).doubleValue());
                else if (type == boolean.class) f.setBoolean(settings, (Boolean) raw);
                else if (type == String.class) f.set(settings, String.valueOf(raw));
            } catch (Exception e) {
                // ignore a single bad field, keep the default
            }
        }
    }
}
