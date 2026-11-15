package com.ducvd89.bloodborne;

import java.nio.charset.Charset;
import java.util.LinkedHashMap;
import java.util.Map;

/** A PS4 param.sfo (no Android classes, so it is tested on a desktop JDK). */
final class Sfo {
    private Sfo() {}

    /** param.sfo is not one: the message says why. */
    static final class Invalid extends Exception { Invalid(String m) { super("param.sfo: " + m); } }

    private static long u32(byte[] d, int at) throws Invalid {
        if (at < 0 || at + 4 > d.length) throw new Invalid("unpack requires a buffer of 4 bytes");
        return (d[at] & 0xffL) | (d[at + 1] & 0xffL) << 8 | (d[at + 2] & 0xffL) << 16 | (d[at + 3] & 0xffL) << 24;
    }
    private static int u16(byte[] d, int at) throws Invalid {
        if (at < 0 || at + 2 > d.length) throw new Invalid("unpack requires a buffer of 2 bytes");
        return (d[at] & 0xff) | (d[at + 1] & 0xff) << 8;
    }

    /** {key: String or Long}. Throws Invalid when the data is not a param.sfo. */
    static Map<String, Object> parse(byte[] data) throws Invalid {
        if (data.length < 4 || data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') throw new Invalid("not a param.sfo");
        long keyStart = u32(data, 8), dataStart = u32(data, 12), count = u32(data, 16);
        Map<String, Object> values = new LinkedHashMap<>();
        for (long index = 0; index < count; ++index) {
            int entry = (int)(20 + 16 * index);
            if (20 + 16 * index + 16 > data.length) throw new Invalid("unpack_from requires a buffer of at least 16 bytes");
            long keyOffset = u16(data, entry), format = u16(data, entry + 2), length = u32(data, entry + 4), dataOffset = u32(data, entry + 12);
            long keyAt = keyStart + keyOffset;
            long end = -1;
            for (long k = keyAt; k < data.length; ++k) if (data[(int)k] == 0) { end = k; break; }
            if (end < 0) throw new Invalid("subsection not found");
            StringBuilder key = new StringBuilder();
            for (long k = keyAt; k < end; ++k) {
                if (data[(int)k] < 0) throw new Invalid("'ascii' codec can't decode byte");
                key.append((char)data[(int)k]);
            }
            long from = Math.min(dataStart + dataOffset, data.length), to = Math.min(from + length, data.length);
            if (format == 0x0404) {
                if (to - from < 4) throw new Invalid("unpack requires a buffer of 4 bytes");
                values.put(key.toString(), u32(data, (int)from));
            } else {
                int stop = (int)from;
                while (stop < to && data[stop] != 0) ++stop;
                values.put(key.toString(), new String(data, (int)from, stop - (int)from, Charset.forName("UTF-8")));
            }
        }
        return values;
    }

    /** The value as text (null when missing or empty); integers as decimal. */
    static String text(Map<String, Object> sfo, String key) {
        if (sfo == null) return null;
        Object v = sfo.get(key);
        if (v == null) return null;
        String s = String.valueOf(v);
        return s.isEmpty() ? null : s;
    }
}
