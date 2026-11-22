package com.example.helloworld.im;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Gson JsonObject 的宽松读取工具。
 * 服务端的 uint64 在 JSON 里是数字，但历史消息数组、事件中偶尔以字符串下发，
 * 因此所有整型读取都同时兼容 number / 数字字符串（对齐桌面端 imstypes.cpp 的解析口径）。
 */
public final class Json {

    private Json() {
    }

    public static JsonElement child(JsonObject o, String k) {
        if (o == null || !o.has(k) || o.get(k).isJsonNull()) {
            return null;
        }
        return o.get(k);
    }

    public static long num(JsonObject o, String k, long def) {
        JsonElement e = child(o, k);
        if (e == null) {
            return def;
        }
        try {
            if (e.isJsonPrimitive() && e.getAsJsonPrimitive().isString()) {
                String s = e.getAsString().trim();
                return s.isEmpty() ? def : Long.parseLong(s);
            }
            // 直接取 long：userId / msg_id 这类 uint64 一旦超过 2^53，
            // 经 double 中转会掉精度（两个不同 ID 可能解析成同一个值）
            return e.getAsLong();
        } catch (Exception ex) {
            // 极端情况（带小数的 ID）再退回 double
            try {
                return (long) e.getAsDouble();
            } catch (Exception ignored) {
                return def;
            }
        }
    }

    public static long num(JsonObject o, String k) {
        return num(o, k, 0);
    }

    public static int integer(JsonObject o, String k, int def) {
        return (int) num(o, k, def);
    }

    public static int integer(JsonObject o, String k) {
        return integer(o, k, 0);
    }

    public static String str(JsonObject o, String k, String def) {
        JsonElement e = child(o, k);
        if (e == null) {
            return def;
        }
        try {
            return e.isJsonPrimitive() ? e.getAsString() : e.toString();
        } catch (Exception ex) {
            return def;
        }
    }

    public static String str(JsonObject o, String k) {
        return str(o, k, "");
    }

    /** is_group 在协议里接受 bool 与非 0 int */
    public static boolean bool(JsonObject o, String k, boolean def) {
        JsonElement e = child(o, k);
        if (e == null) {
            return def;
        }
        try {
            if (e.isJsonPrimitive() && e.getAsJsonPrimitive().isNumber()) {
                return e.getAsDouble() != 0;
            }
            return e.getAsBoolean();
        } catch (Exception ex) {
            return def;
        }
    }

    public static JsonArray arr(JsonObject o, String k) {
        JsonElement e = child(o, k);
        if (e != null && e.isJsonArray()) {
            return e.getAsJsonArray();
        }
        return new JsonArray();
    }

    public static JsonObject obj(JsonObject o, String k) {
        JsonElement e = child(o, k);
        if (e != null && e.isJsonObject()) {
            return e.getAsJsonObject();
        }
        return new JsonObject();
    }

    public static List<JsonObject> objList(JsonArray a) {
        List<JsonObject> out = new ArrayList<>();
        for (JsonElement e : a) {
            if (e != null && e.isJsonObject()) {
                out.add(e.getAsJsonObject());
            }
        }
        return out;
    }

    public static long[] toLongArray(JsonArray a) {
        long[] out = new long[a.size()];
        int i = 0;
        for (JsonElement e : a) {
            try {
                out[i++] = e.getAsLong();
            } catch (Exception ex) {
                out[i++] = 0;
            }
        }
        return out;
    }

    public static JsonArray longArray(long[] values) {
        JsonArray a = new JsonArray();
        for (long v : values) {
            a.add(v);
        }
        return a;
    }
}
