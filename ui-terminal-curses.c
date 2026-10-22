/* This file is included from ui-terminal.c */
#include <curses.h>

#define UI_TERMKEY_FLAGS (TERMKEY_FLAG_NOTERMIOS)

#ifndef A_ITALIC
#define A_ITALIC A_NORMAL
#endif

#ifdef NCURSES_VERSION
# ifndef NCURSES_EXT_COLORS
#  define NCURSES_EXT_COLORS 0
# endif
# if !NCURSES_EXT_COLORS
#  define MAX_COLOR_PAIRS MIN(COLOR_PAIRS, 256)
# endif
#endif
#ifndef MAX_COLOR_PAIRS
# define MAX_COLOR_PAIRS COLOR_PAIRS
#endif

#define MAX_COLOR_CLOBBER 240
#define DIRECT_COLOR_INDEXED_BASE 0x1000000
#define DIRECT_COLOR_COUNT (DIRECT_COLOR_INDEXED_BASE + 256)

VIS_INTERNAL VisCellStyle
vis_ui_backend_style_default(Ui *ui)
{
	VisCellStyle result = {0};
	result.properties |= (VisCellProperty_IndexedFG|VisCellProperty_IndexedBG);
	result.properties |= (VisCellProperty_FGSet|VisCellProperty_BGSet);
	VisCellStyleFGIndexSet(&result, ui->curses.default_fg);
	VisCellStyleBGIndexSet(&result, ui->curses.default_bg);
	return result;
}

/* Calculate r,g,b components of one of the standard upper 240 colors */
static void get_6cube_rgb(unsigned int n, int *r, int *g, int *b)
{
	if (n < 16) {
		return;
	} else if (n < 232) {
		n -= 16;
		*r = (n / 36) ? (n / 36) * 40 + 55 : 0;
		*g = ((n / 6) % 6) ? ((n / 6) % 6) * 40 + 55 : 0;
		*b = (n % 6) ? (n % 6) * 40 + 55 : 0;
	} else if (n < 256) {
		n -= 232;
		*r = n * 10 + 8;
		*g = n * 10 + 8;
		*b = n * 10 + 8;
	}
}

/* Reset color palette to default values using OSC 104 */
static void undo_palette(void)
{
	str8 cmd = str8("\033]104;\a");
	os_write_file(os_error_handle(), cmd.data, cmd.length);
}

/* Work out the nearest color from the 256 color set, or perhaps exactly. */
VIS_INTERNAL VisTerminalStyle
vis_terminal_style_rgb(Vis *vis, u8 r, u8 g, u8 b)
{
	VisTerminalStyle result = {.indexed = true};

	VisCursesUI *cui = &vis->ui.curses;
	if (cui->direct_color) {
		result.indexed = false;
		result.color.rgb.r = r;
		result.color.rgb.g = g;
		result.color.rgb.b = b;
		return result;
	}
	static short color_clobber_idx = 0;
	static uint32_t clobbering_colors[MAX_COLOR_CLOBBER];

	if (cui->change_colors == -1)
		cui->change_colors = vis->change_colors && can_change_color() && COLORS >= 256;
	if (cui->change_colors) {
		uint32_t hexrep = (r << 16) | (g << 8) | b;
		for (short i = 0; i < MAX_COLOR_CLOBBER; ++i) {
			if (clobbering_colors[i] == hexrep) {
				result.color.index = i + 16;
				return result;
			} else if (!clobbering_colors[i]) {
				break;
			}
		}

		short i = color_clobber_idx;
		clobbering_colors[i] = hexrep;
		init_color(i + 16, (r * 1000 + 0xfe) / 0xff, (g * 1000 + 0xfe) / 0xff,
		           (b * 1000 + 0xfe) / 0xff);

		/* in the unlikely case a user requests this many colors, reuse old slots */
		if (++color_clobber_idx >= MAX_COLOR_CLOBBER)
			color_clobber_idx = 0;

		result.color.index = i + 16;
		return result;
	}

	static const unsigned char color_256_to_16[256] = {
		 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
		 0,  4,  4,  4, 12, 12,  2,  6,  4,  4, 12, 12,  2,  2,  6,  4,
		12, 12,  2,  2,  2,  6, 12, 12, 10, 10, 10, 10, 14, 12, 10, 10,
		10, 10, 10, 14,  1,  5,  4,  4, 12, 12,  3,  8,  4,  4, 12, 12,
		 2,  2,  6,  4, 12, 12,  2,  2,  2,  6, 12, 12, 10, 10, 10, 10,
		14, 12, 10, 10, 10, 10, 10, 14,  1,  1,  5,  4, 12, 12,  1,  1,
		 5,  4, 12, 12,  3,  3,  8,  4, 12, 12,  2,  2,  2,  6, 12, 12,
		10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14,  1,  1,  1,  5,
		12, 12,  1,  1,  1,  5, 12, 12,  1,  1,  1,  5, 12, 12,  3,  3,
		 3,  7, 12, 12, 10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14,
		 9,  9,  9,  9, 13, 12,  9,  9,  9,  9, 13, 12,  9,  9,  9,  9,
		13, 12,  9,  9,  9,  9, 13, 12, 11, 11, 11, 11,  7, 12, 10, 10,
		10, 10, 10, 14,  9,  9,  9,  9,  9, 13,  9,  9,  9,  9,  9, 13,
		 9,  9,  9,  9,  9, 13,  9,  9,  9,  9,  9, 13,  9,  9,  9,  9,
		 9, 13, 11, 11, 11, 11, 11, 15,  0,  0,  0,  0,  0,  0,  8,  8,
		 8,  8,  8,  8,  7,  7,  7,  7,  7,  7, 15, 15, 15, 15, 15, 15
	};

	int i = 0;
	if ((!r || (r - 55) % 40 == 0) &&
	    (!g || (g - 55) % 40 == 0) &&
	    (!b || (b - 55) % 40 == 0)) {
		i = 16;
		i += r ? ((r - 55) / 40) * 36 : 0;
		i += g ? ((g - 55) / 40) * 6 : 0;
		i += g ? ((b - 55) / 40) : 0;
	} else if (r == g && g == b && (r - 8) % 10 == 0 && r < 239) {
		i = 232 + ((r - 8) / 10);
	} else {
		unsigned lowest = UINT_MAX;
		for (int j = 16; j < 256; ++j) {
			int jr = 0, jg = 0, jb = 0;
			get_6cube_rgb(j, &jr, &jg, &jb);
			int dr = jr - r;
			int dg = jg - g;
			int db = jb - b;
			unsigned int distance = dr * dr + dg * dg + db * db;
			if (distance < lowest) {
				lowest = distance;
				i = j;
			}
		}
	}

	result.color.index = i;
	if (COLORS <= 16)
		result.color.index = color_256_to_16[i];
	return result;
}

VIS_INTERNAL VisTerminalStyle
vis_terminal_style_indexed(u8 index)
{
	VisTerminalStyle result = {0};
	result.indexed     = true;
	result.color.index = index;
	return result;
}

VIS_INTERNAL short
vis_ui_curses_color_pair_get(Ui *ui, int fg, int bg)
{
	VisCursesUI *cui = &ui->curses;
	if (!cui->palette)
		return 0;

	size_t count = (size_t)cui->color_pairs_max * 2;
	size_t slot = ((u32)fg * 2654435761u ^ (u32)bg) % count;
	while (cui->palette[slot].id) {
		VisCursesColorPair *pair = &cui->palette[slot];
		if (pair->fg == fg && pair->bg == bg)
			return pair->id;
		slot = (slot + 1) % count;
	}

	/* shortcut: full caches use defaults; add frame-safe reclamation if exhaustion becomes common. */
	if (cui->color_pair_current + 1 >= cui->color_pairs_max)
		return 0;
	short id = cui->color_pair_current + 1;
#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS && defined(NCURSES_EXT_FUNCS)
	int status = init_extended_pair(id, fg, bg);
#else
	int status = init_pair(id, fg, bg);
#endif
	if (status == ERR)
		return 0;
	cui->color_pair_current = id;
	cui->palette[slot] = (VisCursesColorPair){.fg = fg, .bg = bg, .id = id};
	return id;
}

static int curses_color(Ui *ui, bool indexed, u16 index, u8 r, u8 g, u8 b) {
	if (!indexed)
		return ((int)r << 16) | ((int)g << 8) | b;
	if (index == UINT16_MAX)
		return -1;
	if (ui->curses.direct_color)
		return index < 256 ? DIRECT_COLOR_INDEXED_BASE + index : -1;
	return index < COLORS ? index : -1;
}

VIS_INTERNAL void
vis_ui_curses_style_set(Ui *ui, WINDOW *window, VisCellStyle style)
{
	attr_t result = 0;
	result |= (style.attributes & VisCellAttribute_Underline) ? A_UNDERLINE : 0;
	result |= (style.attributes & VisCellAttribute_Reverse)   ? A_REVERSE   : 0;
	result |= (style.attributes & VisCellAttribute_Blink)     ? A_BLINK     : 0;
	result |= (style.attributes & VisCellAttribute_Bold)      ? A_BOLD      : 0;
	result |= (style.attributes & VisCellAttribute_Italic)    ? A_ITALIC    : 0;
	result |= (style.attributes & VisCellAttribute_Dim)       ? A_DIM       : 0;
	int fg = curses_color(ui, style.properties & VisCellProperty_IndexedFG,
	                      VisCellStyleFGIndexGet(&style), style.fg_r, style.fg_g, style.fg_b);
	int bg = curses_color(ui, style.properties & VisCellProperty_IndexedBG,
	                      VisCellStyleBGIndexGet(&style), style.bg_r, style.bg_g, style.bg_b);
	wattr_set(window, result, vis_ui_curses_color_pair_get(ui, fg, bg), NULL);
}

static void ui_term_backend_blit(Ui *tui) {
	int w = tui->width, h = tui->height;
	VisCellData  *cell  = tui->cell_buffer.cells;
	VisCellStyle *style = tui->cell_buffer.styles;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			vis_ui_curses_style_set(tui, stdscr, *style);
			mvaddnstr(y, x, (char *)cell->data, cell->data_length);
			cell++;
			style++;
		}
	}
	vis_terminal_draw(tui);
	move(tui->cur_row, tui->cur_col);
	wnoutrefresh(stdscr);
	if (tui->doupdate)
		doupdate();
}

static void ui_term_backend_clear(Ui *tui) {
	clear();
}

static bool ui_term_backend_resize(Ui *tui, int width, int height) {
	return resizeterm(height, width) == OK &&
	       wresize(stdscr, height, width) == OK;
}

static void ui_term_backend_save(Ui *tui, bool fscr) {
	if (fscr) {
		def_prog_mode();
		endwin();
	} else {
		reset_shell_mode();
	}
}

static void ui_term_backend_restore(Ui *tui) {
	reset_prog_mode();
	wclear(stdscr);
}

int ui_terminal_colors(void) {
	return COLORS;
}

static bool
ui_backend_init(Ui *ui, char *term)
{
	SCREEN *screen = NULL;
#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS && defined(NCURSES_EXT_FUNCS)
	const char *colorterm = getenv("COLORTERM");
	bool wants_rgb = (colorterm && (!strcmp(colorterm, "truecolor") || !strcmp(colorterm, "24bit"))) ||
	                 strstr(term, "-direct");
	if (wants_rgb) {
		screen = newterm("vis-direct", stderr, stdin);
		ui->curses.direct_color = screen != NULL;
		if (!screen)
			snprintf(ui->info, sizeof(ui->info), "Warning: vis-direct terminfo missing; using indexed colors");
	}
#endif
	if (!screen)
		screen = newterm(term, stderr, stdin);
	if (!screen) {
		snprintf(ui->info, sizeof(ui->info), "Warning: unknown term `%s'", term);
		screen = newterm(strstr(term, "-256color") ? "xterm-256color" : "xterm", stderr, stdin);
	}

	if (screen) {
		start_color();
		/* Do not interpret another direct profile's RGB IDs as palette indices. */
		if (!ui->curses.direct_color && COLORS > 256) {
			endwin();
			delscreen(screen);
			screen = newterm("xterm-256color", stderr, stdin);
			if (!screen)
				return false;
			start_color();
		}
		if (ui->curses.direct_color && COLORS != DIRECT_COLOR_COUNT) {
			endwin();
			delscreen(screen);
			return false;
		}
		use_default_colors();
		cbreak();
		noecho();
		nonl();
		keypad(stdscr, TRUE);
		curs_set(0);

		pair_content(0, &ui->curses.default_fg, &ui->curses.default_bg);
		ui->curses.change_colors   = -1;
		ui->curses.color_pairs_max = MIN(MAX_COLOR_PAIRS, SHRT_MAX);
		if (COLORS && ui->curses.color_pairs_max > 1) {
			ui->curses.palette = calloc((size_t)ui->curses.color_pairs_max * 2,
			                           sizeof(*ui->curses.palette));
			if (!ui->curses.palette) {
				endwin();
				delscreen(screen);
				return false;
			}
		}
	}

	return screen != NULL;
}

VIS_INTERNAL void ui_terminal_resume(Ui *term) {}

VIS_INTERNAL void
ui_term_backend_suspend(Ui *ui)
{
	if (ui->curses.change_colors == 1)
		undo_palette();
	curs_set(1);
}

VIS_INTERNAL void
vis_ui_backend_free(Ui *ui)
{
	ui_term_backend_suspend(ui);
	free(ui->curses.palette);
	endwin();
}
