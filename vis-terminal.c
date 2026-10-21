/* Persistent terminal buffers. Only PTYs/pipes carry runtime I/O. */
#include "vis-core.h"
#include <pty.h>
#include <vterm.h>

#if !CONFIG_CURSES || !CONFIG_LUA
#error "Terminal buffers require the curses and Lua backends"
#endif

typedef struct VisTerminal {
	Vis *vis;
	File *file;
	struct VisTerminal *next;
	VTerm *vt;
	VTermScreen *screen;
	WINDOW *canvas;
	pid_t pid;
	int master, output, callback, status;
	bool visible, dirty, eof, finished, in_callback;
	const char *error;
	Buffer pending, captured;
} VisTerminal;

static void terminal_output(const char *bytes, size_t len, void *data) {
	VisTerminal *t = data;
	if (len > 65536 || t->pending.length > 65536 - (s64)len ||
	    !buffer_append(&t->pending, bytes, len))
		t->error = "Terminal input queue overflow";
}

static int terminal_property(VTermProp prop, VTermValue *value, void *data) {
	VisTerminal *t = data;
	if (prop == VTERM_PROP_CURSORVISIBLE) t->visible = value->boolean;
	return 1;
}

VIS_INTERNAL bool vis_terminal_running(File *file) {
	return file && file->terminal && file->terminal->pid > 0;
}

VIS_INTERNAL bool vis_terminal_active(Vis *vis) {
	for (VisTerminal *t = vis->ui.terminal; t; t = t->next)
		if (t->pid > 0) return true;
	return false;
}

/* Rebuild a bounded screen snapshot, not an ever-growing undo tree. */
static void terminal_snapshot(VisTerminal *t) {
	if (!t->dirty || !t->file) return;
	Vis *vis = t->vis;
	for (Win *w = vis->windows; w; w = w->next) {
		if (w->file == t->file && (w->view.selection_count > 1 ||
		    (w == vis->win && vis->mode->visual)))
			return; /* keep selected text stable while the process continues */
	}
	Buffer text = {0};
	int rows, cols;
	vterm_get_size(t->vt, &rows, &cols);
	VTermPos cursor;
	vterm_state_get_cursorpos(vterm_obtain_state(t->vt), &cursor);
	size_t cursor_pos = 0, last_used = 0;
	for (int row = 0; row < rows; row++) {
		size_t start = text.length, keep = start;
		for (int col = 0; col < cols; col++) {
			if (row == cursor.row && col == cursor.col) cursor_pos = text.length;
			VTermScreenCell cell = {0};
			vterm_screen_get_cell(t->screen, (VTermPos){row, col}, &cell);
			if (cell.chars[0] == UINT32_MAX) continue;
			char bytes[VTERM_MAX_CHARS_PER_CELL * MB_LEN_MAX];
			size_t len = 0;
			mbstate_t state = {0};
			for (size_t i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
				size_t n = wcrtomb(bytes + len, cell.chars[i], &state);
				if (n == (size_t)-1) break;
				len += n;
			}
			if (!len || cell.attrs.conceal) { bytes[0] = ' '; len = 1; }
			if (!buffer_append(&text, bytes, len)) goto fail;
			if ((len != 1 || bytes[0] != ' ') || (row == cursor.row && col < cursor.col))
				keep = text.length;
		}
		text.length = keep;
		if (!buffer_append(&text, "\n", 1)) goto fail;
		if (keep > start || row == cursor.row) last_used = text.length;
	}
	text.length = last_used;
	if (t->finished) {
		cursor_pos = text.length;
		if (!vis_buffer_appendf(&text, "[Process exited: %d]\n", t->status)) goto fail;
	}
	Text *snapshot = vis_text_load(vis, NULL, vis->load_method);
	if (!snapshot) goto fail;
	if (!text_insert(vis, snapshot, 0, text.data, text.length)) {
		text_free(snapshot);
		goto fail;
	}
	snapshot->readonly = true;
	Text *old = t->file->text;
	t->file->text = snapshot;
	t->file->last_cursor = text_mark_set(snapshot, cursor_pos);
	for (Win *w = vis->windows; w; w = w->next) {
		if (w->file != t->file) continue;
		size_t pos = view_cursor_get(&w->view);
		view_reload(&w->view, snapshot);
		if ((w == vis->win && vis->mode->id == VIS_MODE_TERMINAL) || t->finished)
			pos = cursor_pos;
		view_cursors_to(w->view.selection, MIN(pos, text_size(snapshot)));
		view_draw(&w->view);
	}
	obj_ref_free(vis->lua, old); /* invalidate cached Lua file.lines handles */
	text_free(old);
	buffer_release(&text);
	t->dirty = false;
	return;
fail:
	buffer_release(&text);
	t->error = "Could not allocate terminal screen snapshot";
}

VIS_INTERNAL void vis_terminal_enter(Vis *vis, Mode *old) {
	(void)old;
	Ui *ui = &vis->ui;
	if (!ui->terminal_raw) {
		if (tcgetattr(ui->termkey.fd, &ui->terminal_saved) == -1) {
			vis->win->file->terminal->error = "Could not read terminal input settings";
			return;
		}
		ui->terminal_raw = true;
	}
	struct termios raw = ui->terminal_saved;
	cfmakeraw(&raw);
	raw.c_oflag = ui->terminal_saved.c_oflag;
	if (tcsetattr(ui->termkey.fd, TCSANOW, &raw) == -1)
		vis->win->file->terminal->error = "Could not enable terminal input";
}

VIS_INTERNAL void vis_terminal_leave(Vis *vis, Mode *next) {
	(void)next;
	if (vis->ui.terminal_raw) {
		if (tcsetattr(vis->ui.termkey.fd, TCSANOW, &vis->ui.terminal_saved) == -1)
			vis_info_show(vis, "Could not restore editor input settings");
		vis->ui.terminal_raw = false;
	}
	curs_set(1);
}

VIS_INTERNAL void vis_terminal_input(Vis *vis, const char *str, size_t len) {
	if (!vis->win || !vis_terminal_running(vis->win->file) || len >= VIS_KEY_LENGTH_MAX)
		return;
	VisTerminal *t = vis->win->file->terminal;
	char input[VIS_KEY_LENGTH_MAX];
	memcpy(input, str, len);
	input[len] = '\0';
	TermKeyKey key = {0};
	const char *encoded = input[0] == '<' && len > 1 ? input + 1 : input;
	if (!termkey_strpkey(&vis->ui.termkey, encoded, &key, TERMKEY_FORMAT_VIM)) return;
	VTermModifier mods = VTERM_MOD_NONE;
	if (key.modifiers & TERMKEY_KEYMOD_SHIFT) mods |= VTERM_MOD_SHIFT;
	if (key.modifiers & TERMKEY_KEYMOD_ALT) mods |= VTERM_MOD_ALT;
	if (key.modifiers & TERMKEY_KEYMOD_CTRL) mods |= VTERM_MOD_CTRL;
	if (key.type == TERMKEY_TYPE_UNICODE) {
		vterm_keyboard_unichar(t->vt, key.code.codepoint, mods);
	} else if (key.type == TERMKEY_TYPE_FUNCTION) {
		vterm_keyboard_key(t->vt, VTERM_KEY_FUNCTION(key.code.number), mods);
	} else if (key.type == TERMKEY_TYPE_KEYSYM) {
		VTermKey code = VTERM_KEY_NONE;
		switch (key.code.sym) {
		case TERMKEY_SYM_ENTER: code = VTERM_KEY_ENTER; break;
		case TERMKEY_SYM_TAB: code = VTERM_KEY_TAB; break;
		case TERMKEY_SYM_BACKSPACE: case TERMKEY_SYM_DEL: code = VTERM_KEY_BACKSPACE; break;
		case TERMKEY_SYM_ESCAPE: code = VTERM_KEY_ESCAPE; break;
		case TERMKEY_SYM_UP: code = VTERM_KEY_UP; break;
		case TERMKEY_SYM_DOWN: code = VTERM_KEY_DOWN; break;
		case TERMKEY_SYM_LEFT: code = VTERM_KEY_LEFT; break;
		case TERMKEY_SYM_RIGHT: code = VTERM_KEY_RIGHT; break;
		case TERMKEY_SYM_HOME: code = VTERM_KEY_HOME; break;
		case TERMKEY_SYM_END: code = VTERM_KEY_END; break;
		case TERMKEY_SYM_PAGEUP: code = VTERM_KEY_PAGEUP; break;
		case TERMKEY_SYM_PAGEDOWN: code = VTERM_KEY_PAGEDOWN; break;
		case TERMKEY_SYM_INSERT: code = VTERM_KEY_INS; break;
		case TERMKEY_SYM_DELETE: code = VTERM_KEY_DEL; break;
		case TERMKEY_SYM_SPACE: vterm_keyboard_unichar(t->vt, ' ', mods); break;
		case TERMKEY_SYM_KPENTER: code = VTERM_KEY_KP_ENTER; break;
		case TERMKEY_SYM_KPPLUS: code = VTERM_KEY_KP_PLUS; break;
		case TERMKEY_SYM_KPMINUS: code = VTERM_KEY_KP_MINUS; break;
		case TERMKEY_SYM_KPMULT: code = VTERM_KEY_KP_MULT; break;
		case TERMKEY_SYM_KPDIV: code = VTERM_KEY_KP_DIVIDE; break;
		case TERMKEY_SYM_KPCOMMA: code = VTERM_KEY_KP_COMMA; break;
		case TERMKEY_SYM_KPPERIOD: code = VTERM_KEY_KP_PERIOD; break;
		case TERMKEY_SYM_KPEQUALS: code = VTERM_KEY_KP_EQUAL; break;
		default:
			if (key.code.sym >= TERMKEY_SYM_KP0 && key.code.sym <= TERMKEY_SYM_KP9)
				code = VTERM_KEY_KP_0 + key.code.sym - TERMKEY_SYM_KP0;
		}
		if (code != VTERM_KEY_NONE) vterm_keyboard_key(t->vt, code, mods);
	}
}

static void terminal_stop(VisTerminal *t) {
	if (t->master >= 0) { close(t->master); t->master = -1; }
	if (t->output >= 0) { close(t->output); t->output = -1; }
	if (t->pid > 0) {
		/* WNOWAIT keeps this PID owned until cleanup; never kill a reused PID. */
		kill(-t->pid, SIGKILL);
		kill(t->pid, SIGKILL);
		while (waitpid(t->pid, NULL, 0) == -1 && errno == EINTR) {}
		t->pid = 0;
	}
}

static void terminal_release(VisTerminal *t) {
	if (t->callback != LUA_NOREF) luaL_unref(t->vis->lua, LUA_REGISTRYINDEX, t->callback);
	if (t->canvas) delwin(t->canvas);
	if (t->vt) vterm_free(t->vt);
	buffer_release(&t->pending);
	buffer_release(&t->captured);
	free(t);
}

VIS_INTERNAL void vis_terminal_destroy(Vis *vis, File *file) {
	VisTerminal *t = file->terminal;
	if (!t) return;
	if (vis->win && vis->win->file == file && vis->mode->id == VIS_MODE_TERMINAL)
		vis_mode_switch(vis, VIS_MODE_NORMAL);
	VisTerminal **link = &vis->ui.terminal;
	while (*link && *link != t) link = &(*link)->next;
	if (*link) *link = t->next;
	terminal_stop(t);
	file->terminal = NULL;
	t->file = NULL;
	/* Exit callbacks may delete their own buffer. Defer freeing their context. */
	if (!t->in_callback) terminal_release(t);
}

static bool terminal_capture(VisTerminal *t) {
	if (t->output < 0) return false;
	char bytes[16384];
	ssize_t len = read(t->output, bytes, sizeof(bytes));
	if (len > 0) {
		if (t->captured.length + len > 1024 * 1024 || !buffer_append(&t->captured, bytes, len))
			t->error = "Terminal stdout exceeds 1 MiB or could not be allocated";
		return !t->error;
	}
	if (len < 0 && errno == EINTR) return true;
	if (len < 0 && errno == EAGAIN) return false;
	if (len < 0) t->error = "Terminal stdout read failed";
	close(t->output);
	t->output = -1;
	return false;
}

static void terminal_finish(VisTerminal *t, int status) {
	Vis *vis = t->vis;
	terminal_stop(t);
	t->finished = t->dirty = true;
	t->status = t->error ? -1 : status;
	if (vis->win && vis->win->file == t->file && vis->mode->id == VIS_MODE_TERMINAL)
		vis_mode_switch(vis, VIS_MODE_NORMAL);
	terminal_snapshot(t);
	if (t->error) vis_info_show(vis, "%s", t->error);
	if (t->callback != LUA_NOREF) {
		int ref = t->callback;
		t->callback = LUA_NOREF;
		t->in_callback = true;
		bool discard = vis_lua_terminal_exit(vis, t->file, ref, t->status, &t->captured);
		t->in_callback = false;
		if (!t->file) { terminal_release(t); return; }
		if (discard && t->file->refcount == 0) {
			vis_file_destroy(vis, t->file, true);
			return;
		}
	}
	buffer_release(&t->captured);
	buffer_release(&t->pending);
}

VIS_INTERNAL int vis_terminal_before_tick(Vis *vis, fd_set *reads, fd_set *writes, int maxfd) {
	for (VisTerminal *t = vis->ui.terminal; t; t = t->next) {
		if (t->finished) { terminal_snapshot(t); continue; }
		if (t->pid <= 0) continue;
		if (t->master >= 0 && !t->eof) {
			FD_SET(t->master, reads);
			if (t->pending.length) FD_SET(t->master, writes);
			maxfd = MAX(maxfd, t->master);
		}
		if (t->output >= 0) {
			FD_SET(t->output, reads);
			maxfd = MAX(maxfd, t->output);
		}
	}
	return maxfd;
}

VIS_INTERNAL void vis_terminal_tick(Vis *vis, fd_set *reads, fd_set *writes) {
	for (VisTerminal *t = vis->ui.terminal; t; t = t->next) {
		if (t->finished) { terminal_snapshot(t); continue; }
		Win *display = NULL;
		for (Win *w = vis->windows; w; w = w->next)
			if (w->file == t->file) { display = w; if (w == vis->win) break; }
		if (display) {
			int rows = MAX(1, display->height - !!(display->options & UI_OPTION_STATUSBAR));
			int cols = MAX(1, display->width), oldrows, oldcols;
			vterm_get_size(t->vt, &oldrows, &oldcols);
			if (rows > 1000 || cols > 1000 || rows * cols > 262144) {
				t->error = "Terminal dimensions exceed screen limit";
			} else if (rows != oldrows || cols != oldcols) {
				struct winsize size = {.ws_row = rows, .ws_col = cols};
				if (ioctl(t->master, TIOCSWINSZ, &size) == -1) t->error = "Could not resize terminal PTY";
				vterm_set_size(t->vt, rows, cols);
				t->dirty = true;
			}
		}
		char bytes[16384];
		if (t->master >= 0 && FD_ISSET(t->master, reads)) {
			ssize_t len = read(t->master, bytes, sizeof(bytes));
			if (len > 0) { vterm_input_write(t->vt, bytes, len); t->dirty = true; }
			else if (!len || (len < 0 && errno == EIO)) t->eof = true;
			else if (errno != EAGAIN && errno != EINTR) t->error = "Terminal PTY read failed";
		}
		if (t->master >= 0 && FD_ISSET(t->master, writes) && t->pending.length) {
			ssize_t len = write(t->master, t->pending.data, t->pending.length);
			if (len > 0) buffer_remove(&t->pending, 0, len);
			else if (len < 0 && errno != EAGAIN && errno != EINTR && errno != EIO)
				t->error = "Terminal PTY write failed";
		}
		if (t->output >= 0 && FD_ISSET(t->output, reads)) terminal_capture(t);
		vterm_screen_flush_damage(t->screen);
		if (display) terminal_snapshot(t);
		siginfo_t status = {0};
		if (waitid(P_PID, t->pid, &status, WEXITED | WNOHANG | WNOWAIT) == -1 && errno != EINTR)
			t->error = "Could not wait for terminal process";
		if (status.si_pid || t->error) {
			/* Consume final screen bytes, bounded even if descendants keep writing. */
			for (int i = 0; !t->error && i < 4 && t->master >= 0; i++) {
				ssize_t len = read(t->master, bytes, sizeof(bytes));
				if (len <= 0) break;
				vterm_input_write(t->vt, bytes, len);
			}
			vterm_screen_flush_damage(t->screen);
			/* Bounded drain: descendants cannot keep the callback waiting for EOF. */
			while (!t->error && terminal_capture(t)) {}
			terminal_finish(t, status.si_code == CLD_EXITED ? status.si_status : 128 + status.si_status);
			return; /* callbacks can change the terminal list */
		}
	}
}

VIS_INTERNAL File *vis_terminal_start(Vis *vis, const char *command, bool capture, int callback) {
	if (!vis->win) return NULL;
	ui_draw(vis);
	int rows = MAX(1, vis->win->height - !!(vis->win->options & UI_OPTION_STATUSBAR));
	int cols = MAX(1, vis->win->width);
	if (rows > 1000 || cols > 1000 || rows * cols > 262144) return NULL;
	File *file = vis_file_new(vis, NULL, VisFileNewFlag_Internal | VisFileNewFlag_NoRefcount);
	if (!file) return NULL;
	VisTerminal *t = calloc(1, sizeof(*t));
	if (!t) { vis_file_destroy(vis, file, false); return NULL; }
	t->vis = vis; t->file = file; t->master = t->output = -1; t->callback = LUA_NOREF;
	t->visible = t->dirty = true;
	file->terminal = t;
	file->name = str8("[Terminal]");
	t->next = vis->ui.terminal; vis->ui.terminal = t;
	t->vt = vterm_new(rows, cols);
	if (!t->vt) goto fail;
	vterm_set_utf8(t->vt, 1);
	t->screen = vterm_obtain_screen(t->vt);
	if (!t->screen) goto fail;
	static const VTermScreenCallbacks callbacks = {.settermprop = terminal_property};
	vterm_screen_set_callbacks(t->screen, &callbacks, t);
	vterm_screen_enable_altscreen(t->screen, 1);
	vterm_output_set_callback(t->vt, terminal_output, t);
	vterm_screen_reset(t->screen, 1);
	int fds[2] = {-1, -1};
	if (capture) {
		if (pipe2(fds, O_CLOEXEC) == -1) goto fail;
		t->output = fds[0];
	}
	struct winsize size = {.ws_row = rows, .ws_col = cols};
	struct termios *attrs = vis->ui.termkey.restore_termios_valid ? &vis->ui.termkey.restore_termios : NULL;
	t->pid = forkpty(&t->master, NULL, attrs, &size);
	if (t->pid == 0) {
		if (capture && dup2(fds[1], STDOUT_FILENO) == -1) _exit(126);
		if (close_range(3, ~0U, 0) == -1) _exit(126);
		struct sigaction action = {.sa_handler = SIG_DFL};
		sigemptyset(&action.sa_mask);
		for (int sig = 1; sig < NSIG; sig++) sigaction(sig, &action, NULL);
		sigset_t empty; sigemptyset(&empty);
		if (sigprocmask(SIG_SETMASK, &empty, NULL) == -1) _exit(126);
		if (setenv("TERM", "xterm-256color", 1) == -1) _exit(126);
		unsetenv("LINES"); unsetenv("COLUMNS");
		execl(vis->shell, vis->shell, "-c", command, (char *)NULL);
		_exit(127);
	}
	if (fds[1] >= 0) close(fds[1]);
	if (t->pid < 0 || t->master >= FD_SETSIZE || t->output >= FD_SETSIZE) goto fail;
	if (fcntl(t->master, F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(t->master, F_SETFL, O_NONBLOCK) == -1 ||
	    (capture && fcntl(t->output, F_SETFL, O_NONBLOCK) == -1)) goto fail;
	terminal_snapshot(t);
	if (t->error) goto fail;
	file->internal = false;
	vis_event_emit(vis, VIS_EVENT_FILE_OPEN, file);
	vis_jumphist_record(vis->win);
	if (!vis_buffer_switch(vis, vis->win, file)) goto fail;
	t->callback = callback;
	vis_mode_switch(vis, VIS_MODE_TERMINAL);
	return file;
fail:
	vis_file_destroy(vis, file, false);
	return NULL;
}

static VisCellStyle terminal_style(VisTerminal *t, VTermScreenCell *cell) {
	VisCellStyle style = t->vis->ui.styles[UI_STYLE_DEFAULT];
	style.attributes = (cell->attrs.bold ? VisCellAttribute_Bold : 0) |
	                   (cell->attrs.italic ? VisCellAttribute_Italic : 0) |
	                   (cell->attrs.underline ? VisCellAttribute_Underline : 0) |
	                   (cell->attrs.reverse ? VisCellAttribute_Reverse : 0) |
	                   (cell->attrs.blink ? VisCellAttribute_Blink : 0);
	if (!VTERM_COLOR_IS_DEFAULT_FG(&cell->fg)) {
		unsigned c = VTERM_COLOR_IS_INDEXED(&cell->fg) ? cell->fg.indexed.idx :
			vis_terminal_style_rgb(t->vis, cell->fg.rgb.red, cell->fg.rgb.green, cell->fg.rgb.blue).color.index;
		VisCellStyleFGIndexSet(&style, c);
	}
	if (!VTERM_COLOR_IS_DEFAULT_BG(&cell->bg)) {
		unsigned c = VTERM_COLOR_IS_INDEXED(&cell->bg) ? cell->bg.indexed.idx :
			vis_terminal_style_rgb(t->vis, cell->bg.rgb.red, cell->bg.rgb.green, cell->bg.rgb.blue).color.index;
		VisCellStyleBGIndexSet(&style, c);
	}
	return style;
}

VIS_INTERNAL void vis_terminal_draw(Ui *ui) {
	/* Normal/Visual modes use the actual read-only text view for motions/yanks. */
	VisTerminal *t = ui->terminal;
	if (!t) return;
	Vis *vis = t->vis;
	if (vis->mode->id != VIS_MODE_TERMINAL || !vis->win) return;
	t = vis->win->file->terminal;
	if (!t || t->error) return;
	Win *win = vis->win;
	int rows, cols; vterm_get_size(t->vt, &rows, &cols);
	rows = MIN(rows, win->height - !!(win->options & UI_OPTION_STATUSBAR));
	cols = MIN(cols, win->width);
	if (rows < 1 || cols < 1) return;
	if (!t->canvas) t->canvas = newwin(rows, cols, 0, 0);
	if (!t->canvas || ((getmaxy(t->canvas) != rows || getmaxx(t->canvas) != cols) &&
	                    wresize(t->canvas, rows, cols) == ERR)) {
		t->error = "Could not allocate terminal display"; return;
	}
	werase(t->canvas);
	for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
		VTermScreenCell cell = {0};
		vterm_screen_get_cell(t->screen, (VTermPos){row, col}, &cell);
		if (cell.chars[0] == UINT32_MAX) continue;
		char bytes[VTERM_MAX_CHARS_PER_CELL * MB_LEN_MAX]; size_t len = 0;
		mbstate_t state = {0};
		for (size_t i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
			size_t n = wcrtomb(bytes + len, cell.chars[i], &state);
			if (n == (size_t)-1) break;
			len += n;
		}
		if (!len || cell.attrs.conceal || col + cell.width > cols) { bytes[0] = ' '; len = 1; }
		wattrset(t->canvas, vis_ui_curses_style_to_attr(ui, terminal_style(t, &cell)));
		mvwaddnstr(t->canvas, row, col, bytes, len);
	}
	if (copywin(t->canvas, stdscr, 0, 0, win->y, win->x,
	            win->y + rows - 1, win->x + cols - 1, false) == ERR)
		t->error = "Could not draw terminal inside split";
	VTermPos cursor;
	vterm_state_get_cursorpos(vterm_obtain_state(t->vt), &cursor);
	ui->cur_row = win->y + MIN(MAX(cursor.row, 0), rows - 1);
	ui->cur_col = win->x + MIN(MAX(cursor.col, 0), cols - 1);
	curs_set(t->visible ? 1 : 0);
}
