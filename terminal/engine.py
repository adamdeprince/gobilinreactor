"""Android host adapter for the pinned upstream kitty terminal engine."""
import os
import _goblin_android as android
from kitty.fast_data_types import (
    Screen, set_options, set_font_data, set_send_sprite_to_gpu,
    create_test_font_group, sprite_map_set_limits, encode_key_for_tty, SCROLL_FULL,
)
from kitty.options.types import defaults
from kitty.rgb import Color, to_color

set_options(defaults._replace(foreground=Color(255,255,255), background=Color(0,0,0)))
sprite_map_set_limits(1024, 4)
set_send_sprite_to_gpu(android.sprite)
font = next(p for p in ("/system/fonts/DroidSansMono.ttf", "/system/fonts/RobotoMono-Regular.ttf") if os.path.isfile(p))
set_font_data(lambda index: (font, False, False), 0, 0, 0, 0, (), 12.0, ())
cell_width, cell_height, baseline = create_test_font_group(12.0, 160.0, 160.0)


def set_font_size(points):
    global cell_width, cell_height, baseline
    cell_width, cell_height, baseline = create_test_font_group(points, 160.0, 160.0)
    for screen in screens.values():
        screen.mark_as_dirty()


class Callbacks:
    def __init__(self, ident):
        self.ident = ident

    def write(self, data):
        android.write(data)

    def set_dynamic_color(self, code, value=''):
        # These replies are part of terminal I/O: editors query them before
        # changing modes, including while saving and exiting.
        current = screens.get(self.ident)
        if current is None:  # Screen construction also resets its colors.
            return
        if isinstance(value, (bytes, memoryview)):
            value = bytes(value).decode('utf-8', 'replace')
        names = {10: 'default_fg', 11: 'default_bg', 12: 'cursor_color',
                 17: 'highlight_bg', 19: 'highlight_fg'}
        for item in value.split(';'):
            name = names.get(code if code < 100 else code - 100)
            if name:
                profile = current.color_profile
                if item == '?' and code < 100:
                    color = getattr(profile, name) or Color(0, 0, 0)
                    rgb = '/'.join(f'{c * 257:04x}' for c in (color.red, color.green, color.blue))
                    self.write(f'\x1b]{code};rgb:{rgb}\x1b\\'.encode())
                elif code >= 100:
                    delattr(profile, name)
                elif (color := to_color(item)) is not None:
                    setattr(profile, name, color)
                current.mark_as_dirty()
            code += 1

    def __getattr__(self, name):
        # Window management, clipboard, notifications and remote control have
        # no host authority in this initial Android adapter.
        return lambda *args: None


screens = {}
screen = None


def select(ident):
    global screen
    if ident not in screens:
        callbacks = Callbacks(ident)
        value = Screen(callbacks, 24, 80, 2000, cell_width, cell_height, 0, callbacks)
        value.color_profile.default_fg = 0xffffff
        value.color_profile.default_bg = 0x000000
        screens[ident] = value
    screen = screens[ident]
    return screen


def close(ident):
    screens.pop(ident, None)


select(0)


def key(code, modifiers, text):
    screen.scroll(SCROLL_FULL, False)
    value = encode_key_for_tty(key=code, mods=modifiers, text=text,
                               cursor_key_mode=screen.cursor_key_mode,
                               key_encoding_flags=screen.current_key_encoding_flags())
    android.write(value.encode())


def scroll(lines):
    screen.scroll(abs(lines), lines > 0)


def dump():
    return "\n".join(str(screen.visual_line(y)) for y in range(screen.lines)).encode()
