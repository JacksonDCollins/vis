#include "buffer.h"
#include "text.h"
#include "util.h"
#include "view.h"
#include "vis-core.h"
#include "vis.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static DA_COMPARE_FN(ranges_comparator)
{
	const Filerange *r1 = va, *r2 = vb;
	if (!text_range_valid(*r1))
		return text_range_valid(*r2) ? 1 : 0;
	if (!text_range_valid(*r2))
		return -1;
	return (r1->start < r2->start || (r1->start == r2->start && r1->end < r2->end)) ? -1 : 1;
}

void vis_mark_normalize(FilerangeList *ranges)
{
	for (VisDACount i = 0; i < ranges->count; i++)
		if (text_range_size(ranges->data[i]) == 0)
			da_unordered_remove(ranges, i);

	if (ranges->count) {
		da_sort(ranges, ranges_comparator);
		Filerange prev = text_range_empty();
		for (VisDACount i = 0; i < ranges->count; i++) {
			Filerange r = ranges->data[i];
			if (text_range_overlap(prev, r)) {
				prev = text_range_union(prev, r);
				da_ordered_remove(ranges, i);
			} else {
				prev = r;
				i++;
			}
		}
	}
}

static bool vis_mark_equal(FilerangeList a, FilerangeList b)
{
	bool result = a.count == b.count;
	for (VisDACount i = 0; result && i < a.count; i++)
		result = text_range_equal(a.data[i], b.data[i]);
	return result;
}

static SelectionRegionList *mark_from(Vis *vis, enum VisMark id)
{
	if (vis->win) {
		if (id == VIS_MARK_SELECTION)
			return &vis->win->saved_selections;
		File *file = vis->win->file;
		if (id < LENGTH(file->marks))
			return file->marks + id;
	}
	return 0;
}

enum VisMark vis_mark_used(Vis *vis) {
	return vis->action.mark;
}

void vis_mark(Vis *vis, enum VisMark mark) {
	if (mark < LENGTH(vis->win->file->marks))
		vis->action.mark = mark;
}

static FilerangeList mark_get(Vis *vis, Win *win, SelectionRegionList *mark)
{
	FilerangeList result = {0};
	if (mark) {
		da_reserve(vis, &result, mark->count);
		for (VisDACount i = 0; i < mark->count; i++) {
			Filerange r = view_regions_restore(&win->view, mark->data[i]);
			if (text_range_valid(r))
				*da_push(vis, &result) = r;
		}
		vis_mark_normalize(&result);
	}
	return result;
}

FilerangeList vis_mark_get(Vis *vis, Win *win, enum VisMark id)
{
	return mark_get(vis, win, mark_from(vis, id));
}

static void mark_set(Vis *vis, Win *win, SelectionRegionList *mark, FilerangeList ranges)
{
	if (mark) {
		mark->count = 0;
		for (VisDACount i = 0; i < ranges.count; i++) {
			SelectionRegion ss;
			if (view_regions_save(&win->view, ranges.data[i], &ss))
				*da_push(vis, mark) = ss;
		}
	}
}

void vis_mark_set(Vis *vis, Win *win, enum VisMark id, FilerangeList ranges)
{
	mark_set(vis, win, mark_from(vis, id), ranges);
}

void vis_jumplist(Vis *vis, int advance)
{
	Win  *win  = vis->win;
	View *view = &win->view;
	FilerangeList cur = view_selections_get_all(vis, view);

	size_t cursor = win->mark_set_lru_cursor;
	win->mark_set_lru_cursor += advance;
	if (advance < 0)
		cursor = win->mark_set_lru_cursor;
	cursor %= VIS_MARK_SET_LRU_COUNT;

	SelectionRegionList *next = win->mark_set_lru_regions + cursor;
	bool done = false;
	if (next->count) {
		FilerangeList sel = mark_get(vis, win, next);
		done = vis_mark_equal(sel, cur);
		if (advance && !done) {
			/* NOTE: set cached selection */
			vis_mode_switch(vis, win->mark_set_lru_modes[cursor]);
			view_selections_set_all(view, sel, view_selections_primary_get(view)->anchored);
		}
		da_release(&sel);
	}

	if (!advance && !done) {
		/* NOTE: save the current selection */
		mark_set(vis, win, next, cur);
		win->mark_set_lru_modes[cursor] = vis->mode->id;
		win->mark_set_lru_cursor++;
	}

	da_release(&cur);
}

bool vis_jumphist_jump_to(Win *win, JumpHistory jh) {
	if (!win || !jh.file)return false;
	Vis *vis = win->vis;

	File *file = vis->files;
	while (file && file != jh.file) {
		file = file->next;
	}
	if (!file)return false;

	size_t pos = text_mark_get(file->text, jh.cursor);
	if (pos == EPOS)return false;

	if (!vis_buffer_switch(vis, win, file)) return false;

	view_cursors_to(win->view.selection, pos);
	vis_redraw(vis);
	return true;
}

bool vis_check_jumphist_entry_valid(JumpHistory jh) {
	if ( !jh.file || jh.cursor == EMARK) return false;
	size_t pos = text_mark_get(jh.file->text, jh.cursor);
	if (pos == EPOS) return false;

	return true;
}

static bool jumphist_file_eligible(File *file) {
	return file && !file->internal && !file->terminal &&
	       (file->name.length > 0 || text_size(file->text) > 0 || text_modified(file->text));
}

void vis_jumphist_insert(Win *win, int index, Mark cursor, File *file) {
	if (!jumphist_file_eligible(file) || cursor == EMARK) return;
	if (!win || index < 0 || (size_t)index > win->jumphist_count) return;
	assert(win->jumphist_count <= VIS_JUMPHIST_MAX_COUNT);
	assert(win->jumphist_start < VIS_JUMPHIST_MAX_COUNT);

	if (win->jumphist_count == VIS_JUMPHIST_MAX_COUNT) {
		win->jumphist_start = (win->jumphist_start + 1) % VIS_JUMPHIST_MAX_COUNT;
		win->jumphist_count--;
		/* Evicting the oldest entry moves later logical indices left. */
		if (index > 0) index--;
		if (win->jumphist_index > 0)
			win->jumphist_index--;
	}

	for (size_t i = win->jumphist_count; i > (size_t)index; i--) {
		size_t dst = (win->jumphist_start + i) % VIS_JUMPHIST_MAX_COUNT;
		size_t src = (win->jumphist_start + i - 1) % VIS_JUMPHIST_MAX_COUNT;
		win->jumps[dst] = win->jumps[src];
	}
	if (win->jumphist_count > 0 && (size_t)index <= win->jumphist_index)
		win->jumphist_index++;

	size_t slot = (win->jumphist_start + (size_t)index) % VIS_JUMPHIST_MAX_COUNT;
	win->jumps[slot] = (JumpHistory) {
		.file = file,
		.cursor = cursor,
	};
	win->jumphist_count++;
}

void vis_jumphist_append(Win *win, Mark mark, File *file) {
	if (!win) return;
	vis_jumphist_insert(win, win->jumphist_count, mark, file);
}

void vis_jumphist_remove(Win *win, int index) {
	if (!win || index < 0 || index >= win->jumphist_count) return;

	size_t vacant = (win->jumphist_start + win->jumphist_count - 1 )%VIS_JUMPHIST_MAX_COUNT;

	if (index == 0 ) {
		vacant = win->jumphist_start;
		win->jumphist_start = (win->jumphist_start + 1 ) %VIS_JUMPHIST_MAX_COUNT;
	} else {
		for (size_t i = index; i+1 < win->jumphist_count; i++ ) {
			size_t dst = (win->jumphist_start + i ) % VIS_JUMPHIST_MAX_COUNT;
			size_t src = (win->jumphist_start + i + 1) %VIS_JUMPHIST_MAX_COUNT;
			win->jumps[dst] = win->jumps[src];
		}
	}

	win->jumps[vacant] = (JumpHistory){0};
	win->jumphist_count--;

	if (index < win->jumphist_index) win->jumphist_index--;
}

void vis_jumphist_prune_file(Vis *vis, File *file) {
	// Win *win = vis->win;
	for (Win *win = vis->windows; win; win = win->next){
		for (size_t i = 0; i < win->jumphist_count;) {
			size_t slot = (win->jumphist_start + i) % VIS_JUMPHIST_MAX_COUNT;
			JumpHistory jh = win->jumps[slot];
			if (!jh.file || jh.file == file ) {
				vis_jumphist_remove(win, i);
			} else i++;
		}
	}
}

bool vis_jumphist_record(Win *win) {
	if (!win || !jumphist_file_eligible(win->file)) return true;
	Mark cursor = text_mark_set(win->file->text, view_cursor_get(&win->view));
	if (cursor == EMARK) return false;
	File *file = win->file;
	size_t current_pos=text_mark_get(file->text, cursor);
	if (current_pos == EPOS) return false;
	size_t current_line = text_lineno_by_pos(file->text, current_pos);

	for (size_t i = 0; i < win->jumphist_count; /* advance when not removed */) {
	  size_t slot = (win->jumphist_start + i) % VIS_JUMPHIST_MAX_COUNT;
	  JumpHistory jh = win->jumps[slot];
	  	if (jh.cursor == EMARK || !jh.file) {
			i++;
			continue;
		}
		size_t jhpos = text_mark_get(jh.file->text, jh.cursor);
		if (jhpos == EPOS) {
			i++;
			continue;
		}
		size_t jhline = text_lineno_by_pos(jh.file->text, jhpos);
		if (jh.file == file && current_line == jhline)  {
			vis_jumphist_remove(win, i);
		} else {
			i++;
		}
	}

	vis_jumphist_append(win, cursor, file);
	win->jumphist_index = win->jumphist_count;
	return true;
}

void vis_jumphist_advance(Win *win, int advance, bool file) {
	if (!win || advance == 0 || win->jumphist_count == 0) return;
	assert(win->jumphist_count <= VIS_JUMPHIST_MAX_COUNT);
	assert(win->jumphist_start < VIS_JUMPHIST_MAX_COUNT);
	assert(win->jumphist_index <= win->jumphist_count);

	if (win->jumphist_index == win->jumphist_count) {
		if (advance > 0) return;

		if (jumphist_file_eligible(win->file)) {
			Mark mark = text_mark_set(win->file->text,
                                   view_cursor_get(&win->view));
			if (mark == EMARK) return;

			vis_jumphist_append(win, mark, win->file);
			win->jumphist_index = win->jumphist_count - 1;
		}
	}

	int direction = advance > 0 ? 1 : -1;
	int64_t remaining = advance > 0 ? (int64_t)advance : -(int64_t)advance;
	int64_t target = (int64_t)win->jumphist_index;
	File *last_file = win->file;
	while (remaining > 0) {
		target += direction;
		if (target < 0 || target >= (int64_t)win->jumphist_count) return;
		size_t slot = (win->jumphist_start + (size_t)target) % VIS_JUMPHIST_MAX_COUNT;

		if (!vis_check_jumphist_entry_valid(win->jumps[slot])) {
			vis_jumphist_remove(win, (int)target);
			/* Forward removal shifts the next candidate into this slot. */
			if (direction > 0) target--;
			continue;
		}
		if (!file || win->jumps[slot].file != last_file) {
			last_file = win->jumps[slot].file;
			remaining--;
		}
	}

	size_t slot = (win->jumphist_start + (size_t)target) % VIS_JUMPHIST_MAX_COUNT;
	if (vis_jumphist_jump_to(win, win->jumps[slot]))
		win->jumphist_index = (size_t)target;
}

void vis_jumphist_show(Vis *vis) {
	Win *win = vis->win;

	Buffer lines = {0};
	for (size_t i =0; i < win->jumphist_count; i++) {
		size_t slot = (win->jumphist_start + i) % VIS_JUMPHIST_MAX_COUNT;
		JumpHistory jump = win->jumps[slot];
		if (!jump.file) continue;
		size_t pos = text_mark_get(jump.file->text, jump.cursor);
		if (pos == EPOS) continue;
		size_t line = text_lineno_by_pos(jump.file->text, pos);
		int column = text_line_char_get(jump.file->text, pos) + 1;

		if (!vis_buffer_appendf(&lines, "%c [%zu] %.*s %zu:%d\n",
	i == win->jumphist_index ? '*' : ' ',
	i,
	jump.file->name.length ? (int)jump.file->name.length: 9,
	jump.file->name.length ? (char *)jump.file->name.data: "[No Name]",
	line,
	column
					)){
			buffer_release(&lines);
			return;
		}

	}

	vis_buffer_terminate(&lines);
	vis_message_show(vis, buffer_content0(&lines));
	buffer_release(&lines);
}

void vis_jumphist(Vis *vis, enum VisMotion motion)
{

	if (motion == VIS_JUMPHIST_PREV)
		vis_jumphist_advance(vis->win, -1, false);
	else if (motion == VIS_JUMPHIST_NEXT)
		vis_jumphist_advance(vis->win, 1, false);
	else if (motion == VIS_JUMPHIST_SHOW)
		vis_jumphist_show(vis);
	else if (motion == VIS_JUMPHIST_FILE_PREV)
		vis_jumphist_advance(vis->win, -1, true);
	else if (motion == VIS_JUMPHIST_FILE_NEXT)
		vis_jumphist_advance(vis->win, 1, true);
}

enum VisMark vis_mark_from(Vis *vis, char mark) {
	if (mark >= 'a' && mark <= 'z')
		return VIS_MARK_a + mark - 'a';
	for (size_t i = 0; i < LENGTH(vis_marks); i++) {
		if (vis_marks[i].name == mark)
			return i;
	}
	return VIS_MARK_INVALID;
}

char vis_mark_to(Vis *vis, enum VisMark mark) {
	if (VIS_MARK_a <= mark && mark <= VIS_MARK_z)
		return 'a' + mark - VIS_MARK_a;

	if (mark < LENGTH(vis_marks))
		return vis_marks[mark].name;

	return '\0';
}

read_only MarkDef vis_marks[] = {
	[VIS_MARK_DEFAULT]        = { '\'', VIS_HELP("Default mark")    },
	[VIS_MARK_SELECTION]      = { '^',  VIS_HELP("Last selections") },
};
