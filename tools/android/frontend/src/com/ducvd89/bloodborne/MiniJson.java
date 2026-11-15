package com.ducvd89.bloodborne;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** A small JSON reader for mods.json (no Android classes: org.json is not on a desktop JDK). Objects become
 *  LinkedHashMap, arrays ArrayList, numbers Long or Double, strings String. Its error messages follow Python's
 *  json module ("Expecting value: line 1 column 1 (char 0)"), which the app showed before. */
final class MiniJson {
    static final class ParseException extends Exception { ParseException(String m) { super(m); } }

    private final String s;
    private int i;

    private MiniJson(String s) { this.s = s; }

    static Object parse(String text) throws ParseException {
        MiniJson p = new MiniJson(text);
        p.space();
        Object value = p.value();
        p.space();
        if (p.i != text.length()) throw p.error("Extra data", p.i);
        return value;
    }

    private ParseException error(String what, int at) {
        int line = 1, last = -1;
        for (int k = 0; k < at && k < s.length(); ++k) if (s.charAt(k) == '\n') { ++line; last = k; }
        return new ParseException(what + ": line " + line + " column " + (at - last) + " (char " + at + ")");
    }
    private void space() {
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i; else break;
        }
    }
    private Object value() throws ParseException {
        if (i >= s.length()) throw error("Expecting value", i);
        char c = s.charAt(i);
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return string();
        if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
        if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
        if (s.startsWith("null", i)) { i += 4; return null; }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        throw error("Expecting value", i);
    }
    private Object number() throws ParseException {
        int start = i;
        if (s.charAt(i) == '-') ++i;
        int digits = i;
        while (i < s.length() && Character.isDigit(s.charAt(i)) && s.charAt(i) < 128) ++i;
        if (i == digits) throw error("Expecting value", start);
        boolean real = false;
        if (i < s.length() && s.charAt(i) == '.' && i + 1 < s.length() && isDigit(s.charAt(i + 1))) {
            real = true; ++i;
            while (i < s.length() && isDigit(s.charAt(i))) ++i;
        }
        if (i < s.length() && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) {
            int save = i++;
            if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-')) ++i;
            int exp = i;
            while (i < s.length() && isDigit(s.charAt(i))) ++i;
            if (i == exp) i = save; else real = true;
        }
        String text = s.substring(start, i);
        try { return real ? (Object)Double.valueOf(text) : (Object)Long.valueOf(text); }
        catch (NumberFormatException e) { return Double.valueOf(text); }
    }
    private static boolean isDigit(char c) { return c >= '0' && c <= '9'; }
    private String string() throws ParseException {
        int start = i++;
        StringBuilder out = new StringBuilder();
        while (true) {
            if (i >= s.length()) throw error("Unterminated string starting at", start);
            char c = s.charAt(i++);
            if (c == '"') return out.toString();
            if (c < 0x20) throw error("Invalid control character at", i - 1);
            if (c != '\\') { out.append(c); continue; }
            if (i >= s.length()) throw error("Unterminated string starting at", start);
            char e = s.charAt(i++);
            switch (e) {
                case '"': out.append('"'); break;
                case '\\': out.append('\\'); break;
                case '/': out.append('/'); break;
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'u': {
                    if (i + 4 > s.length()) throw error("Invalid \\uXXXX escape", i - 1);
                    try { out.append((char)Integer.parseInt(s.substring(i, i + 4), 16)); }
                    catch (NumberFormatException x) { throw error("Invalid \\uXXXX escape", i - 1); }
                    i += 4;
                    break;
                }
                default: throw error("Invalid \\escape", i - 2);
            }
        }
    }
    private List<Object> array() throws ParseException {
        List<Object> out = new ArrayList<>();
        ++i; space();
        if (i < s.length() && s.charAt(i) == ']') { ++i; return out; }
        while (true) {
            space();
            out.add(value());
            space();
            if (i < s.length() && s.charAt(i) == ',') { ++i; continue; }
            if (i < s.length() && s.charAt(i) == ']') { ++i; return out; }
            throw error("Expecting ',' delimiter", i);
        }
    }
    private Map<String, Object> object() throws ParseException {
        Map<String, Object> out = new LinkedHashMap<>();
        ++i; space();
        if (i < s.length() && s.charAt(i) == '}') { ++i; return out; }
        while (true) {
            space();
            if (i >= s.length() || s.charAt(i) != '"') throw error("Expecting property name enclosed in double quotes", i);
            String key = string();
            space();
            if (i >= s.length() || s.charAt(i) != ':') throw error("Expecting ':' delimiter", i);
            ++i; space();
            out.put(key, value());
            space();
            if (i < s.length() && s.charAt(i) == ',') { ++i; continue; }
            if (i < s.length() && s.charAt(i) == '}') { ++i; return out; }
            throw error("Expecting ',' delimiter", i);
        }
    }
}
