package com.xlightsprops.ddpdebugger.settings;

import java.lang.reflect.Field;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Tiny hand-rolled JSON reader/writer for a flat, single-level settings bean. Deliberately
 * avoids pulling in a reflection-heavy JSON library (Gson/Jackson) so that packaging this app
 * later (jpackage app-image, or a GraalVM native-image build) only ever needs reflection
 * metadata for {@link AppSettings} itself, not a third-party framework's internals.
 */
final class JsonUtil {
    private JsonUtil() {}

    static String toJson(Object bean) {
        StringBuilder sb = new StringBuilder("{\n");
        Field[] fields = bean.getClass().getFields();
        for (int i = 0; i < fields.length; i++) {
            Field f = fields[i];
            try {
                Object value = f.get(bean);
                sb.append("  \"").append(f.getName()).append("\": ").append(renderValue(value));
            } catch (IllegalAccessException e) {
                continue;
            }
            if (i < fields.length - 1) sb.append(",");
            sb.append("\n");
        }
        sb.append("}\n");
        return sb.toString();
    }

    private static String renderValue(Object value) {
        if (value instanceof String s) return "\"" + escape(s) + "\"";
        if (value instanceof Boolean || value instanceof Number) return String.valueOf(value);
        return "null";
    }

    private static String escape(String s) {
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /** Parses a flat JSON object into a String-keyed map of raw scalar values (String/Double/Boolean). */
    static Map<String, Object> parseFlatObject(String json) {
        Map<String, Object> result = new LinkedHashMap<>();
        int[] pos = {0};
        skipWs(json, pos);
        expect(json, pos, '{');
        skipWs(json, pos);
        if (peek(json, pos) == '}') return result;
        while (true) {
            skipWs(json, pos);
            String key = parseString(json, pos);
            skipWs(json, pos);
            expect(json, pos, ':');
            skipWs(json, pos);
            Object value = parseValue(json, pos);
            result.put(key, value);
            skipWs(json, pos);
            char c = json.charAt(pos[0]);
            if (c == ',') {
                pos[0]++;
                continue;
            }
            if (c == '}') {
                pos[0]++;
                break;
            }
            throw new IllegalArgumentException("Malformed settings JSON near position " + pos[0]);
        }
        return result;
    }

    private static Object parseValue(String json, int[] pos) {
        char c = peek(json, pos);
        if (c == '"') return parseString(json, pos);
        if (c == 't') {
            expectLiteral(json, pos, "true");
            return Boolean.TRUE;
        }
        if (c == 'f') {
            expectLiteral(json, pos, "false");
            return Boolean.FALSE;
        }
        if (c == 'n') {
            expectLiteral(json, pos, "null");
            return null;
        }
        int start = pos[0];
        while (pos[0] < json.length() && "-+.eE0123456789".indexOf(json.charAt(pos[0])) >= 0) pos[0]++;
        return Double.parseDouble(json.substring(start, pos[0]));
    }

    private static void expectLiteral(String json, int[] pos, String literal) {
        if (!json.startsWith(literal, pos[0])) throw new IllegalArgumentException("Expected " + literal + " near " + pos[0]);
        pos[0] += literal.length();
    }

    private static String parseString(String json, int[] pos) {
        expect(json, pos, '"');
        StringBuilder sb = new StringBuilder();
        while (true) {
            char c = json.charAt(pos[0]++);
            if (c == '"') break;
            if (c == '\\') {
                char esc = json.charAt(pos[0]++);
                sb.append(switch (esc) {
                    case 'n' -> '\n';
                    case 't' -> '\t';
                    case '"' -> '"';
                    case '\\' -> '\\';
                    default -> esc;
                });
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }

    private static char peek(String json, int[] pos) {
        return json.charAt(pos[0]);
    }

    private static void expect(String json, int[] pos, char c) {
        if (json.charAt(pos[0]) != c) throw new IllegalArgumentException("Expected '" + c + "' near position " + pos[0]);
        pos[0]++;
    }

    private static void skipWs(String json, int[] pos) {
        while (pos[0] < json.length() && Character.isWhitespace(json.charAt(pos[0]))) pos[0]++;
    }
}
