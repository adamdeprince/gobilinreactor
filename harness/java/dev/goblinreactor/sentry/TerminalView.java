package dev.goblinreactor.sentry;

import android.content.Context;
import android.graphics.*;
import android.view.*;
import java.util.*;

/** Bounded VT-style screen with scrollback; session ownership stays in the service. */
final class TerminalView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<String> history = new ArrayList<>();
    private final int[] palette = {0xff11161d, 0xffeb6f78, 0xff9acb8e, 0xffeac179, 0xff7db4e0, 0xffbd9be6, 0xff8bd5cc, 0xffd4d9df};
    private int rows = 24, columns = 80, row, column, savedRow, savedColumn, foreground = 7, scroll;
    private char[][] screen;
    private int[][] colors;
    private String previous = "";
    private final StringBuilder escape = new StringBuilder();
    private int parser;
    private float cell, line, baseline, touch;
    TerminalView(Context context) {
        super(context); paint.setTypeface(Typeface.MONOSPACE); paint.setTextSize(14 * getResources().getDisplayMetrics().scaledDensity);
        cell = paint.measureText("M"); line = paint.getFontSpacing(); baseline = -paint.ascent();
        setBackgroundColor(palette[0]); reset();
        setContentDescription("Debian terminal output"); setFocusable(true);
    }
    private void reset() {
        screen = new char[rows][columns]; colors = new int[rows][columns];
        for (int r = 0; r < rows; r++) clear(r, 0, columns);
        row = column = savedRow = savedColumn = parser = scroll = 0; foreground = 7; escape.setLength(0); history.clear();
    }
    private void clear(int r, int begin, int end) {
        Arrays.fill(screen[r], Math.max(0, begin), Math.min(columns, end), ' ');
        Arrays.fill(colors[r], Math.max(0, begin), Math.min(columns, end), foreground);
    }
    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        int nextColumns = Math.max(1, Math.min(500, (int)((width - 16) / cell)));
        int nextRows = Math.max(1, Math.min(500, (int)((height - 12) / line)));
        if (nextColumns != columns || nextRows != rows) {
            rows = nextRows; columns = nextColumns; String replay = previous; previous = ""; reset(); transcript(replay);
            SessionService.resizeNative(rows, columns);
        }
    }
    void transcript(String value) {
        if (value.equals(previous)) return;
        if (previous.isEmpty()) SessionService.resizeNative(rows, columns);
        int start = previous.length();
        if (!value.startsWith(previous)) { reset(); start = 0; }
        for (int i = start; i < value.length(); i++) consume(value.charAt(i));
        previous = value; invalidate();
    }
    private void newline() {
        if (++row < rows) return;
        history.add(new String(screen[0])); if (history.size() > 1000) history.remove(0);
        for (int r = 0; r < rows - 1; r++) { screen[r] = screen[r + 1]; colors[r] = colors[r + 1]; }
        row = rows - 1; screen[row] = new char[columns]; colors[row] = new int[columns]; clear(row, 0, columns);
    }
    private void consume(char c) {
        if (parser == 3) { if (c == 7) parser = 0; else if (c == 27) parser = 4; return; }
        if (parser == 4) { parser = c == '\\' ? 0 : 3; return; }
        if (parser == 1) {
            if (c == '[') { parser = 2; escape.setLength(0); }
            else if (c == ']') parser = 3;
            else { parser = 0; if (c == '7') { savedRow = row; savedColumn = column; } else if (c == '8') { row = savedRow; column = savedColumn; } else if (c == 'c') reset(); }
            return;
        }
        if (parser == 2) {
            if (c >= 0x40 && c <= 0x7e) { command(c, escape.toString()); parser = 0; }
            else if (escape.length() < 128) escape.append(c);
            else parser = 0;
            return;
        }
        if (c == 27) { parser = 1; return; }
        if (c == '\r') { column = 0; return; }
        if (c == '\n') { newline(); return; }
        if (c == '\b') { column = Math.max(0, column - 1); return; }
        if (c == '\t') { column = Math.min(columns, (column / 8 + 1) * 8); return; }
        if (c < 32 || c == 127) return;
        if (column >= columns) { column = 0; newline(); }
        screen[row][column] = c; colors[row][column++] = foreground;
    }
    private void command(char command, String text) {
        if (text.startsWith("?")) return;
        String[] parts = text.split(";", -1); int[] n = new int[Math.min(parts.length, 16)];
        for (int i = 0; i < n.length; i++) try { n[i] = Math.max(0, Math.min(10000, Integer.parseInt(parts[i]))); } catch (NumberFormatException ignored) {}
        int first = n.length == 0 ? 0 : n[0], amount = Math.max(1, first);
        switch (command) {
            case 'A': row = Math.max(0, row - amount); break;
            case 'B': row = Math.min(rows - 1, row + amount); break;
            case 'C': column = Math.min(columns - 1, column + amount); break;
            case 'D': column = Math.max(0, column - amount); break;
            case 'E': row = Math.min(rows - 1, row + amount); column = 0; break;
            case 'F': row = Math.max(0, row - amount); column = 0; break;
            case 'G': column = Math.min(columns - 1, amount - 1); break;
            case 'H': case 'f': row = Math.min(rows - 1, amount - 1); column = Math.min(columns - 1, Math.max(1, n.length > 1 ? n[1] : 1) - 1); break;
            case 'J':
                if (first == 2 || first == 3) { for (int r = 0; r < rows; r++) clear(r, 0, columns); if (first == 3) history.clear(); }
                else if (first == 0) { clear(row, column, columns); for (int r = row + 1; r < rows; r++) clear(r, 0, columns); }
                else if (first == 1) { for (int r = 0; r < row; r++) clear(r, 0, columns); clear(row, 0, column + 1); }
                break;
            case 'K': clear(row, first == 0 ? column : 0, first == 1 ? column + 1 : columns); break;
            case 'm': for (int value : n) { if (value == 0 || value == 39) foreground = 7; else if (value >= 30 && value <= 37) foreground = value - 30; else if (value >= 90 && value <= 97) foreground = value - 90; } break;
            case 's': savedRow = row; savedColumn = column; break;
            case 'u': row = Math.min(rows - 1, savedRow); column = Math.min(columns - 1, savedColumn); break;
            case 'P':
                amount = Math.min(amount, columns - Math.min(column, columns));
                System.arraycopy(screen[row], column + amount, screen[row], column, columns - column - amount);
                System.arraycopy(colors[row], column + amount, colors[row], column, columns - column - amount);
                clear(row, columns - amount, columns); break;
            default: break;
        }
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        for (int r = 0; r < rows; r++) {
            int index = history.size() + r - scroll;
            if (index < 0) continue;
            if (index < history.size()) { paint.setColor(palette[7]); canvas.drawText(history.get(index), 8, 6 + baseline + r * line, paint); }
            else {
                int source = index - history.size(); if (source >= rows) continue;
                for (int c = 0; c < columns; c++) { paint.setColor(palette[colors[source][c]]); canvas.drawText(screen[source], c, 1, 8 + c * cell, 6 + baseline + r * line, paint); }
            }
        }
        if (scroll == 0) { paint.setColor(0xff9acb8e); canvas.drawRect(8 + Math.min(column, columns - 1) * cell, 6 + (row + 1) * line - 2, 8 + (Math.min(column, columns - 1) + 1) * cell, 6 + (row + 1) * line, paint); }
    }
    @Override public boolean onTouchEvent(android.view.MotionEvent event) {
        if (event.getAction() == MotionEvent.ACTION_DOWN) { touch = event.getY(); return true; }
        if (event.getAction() == MotionEvent.ACTION_MOVE) { int delta = (int)((event.getY() - touch) / line); if (delta != 0) { scroll = Math.max(0, Math.min(history.size(), scroll + delta)); touch = event.getY(); invalidate(); } return true; }
        return true;
    }
}
