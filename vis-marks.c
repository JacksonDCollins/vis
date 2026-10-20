#include "buffer.h"
#include "text.h"
#include "util.h"
#include "view.h"
#include "vis-core.h"
#include "vis.h"
#include <stddef.h>
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

void vis_jumphist_advance(Vis *vis, int advance) {

}

void vis_jumphist_insert(Vis *vis, int index) {
	Win *win = vis->win;
	if (!win || index < 0 || (size_t)index > win->jumphist_count) return;
	assert(win->jumphist_count <= VIS_JUMPHIST_MAX_COUNT);
	assert(win->jumphist_start < VIS_JUMPHIST_MAX_COUNT);

	Mark cursor = text_mark_set(win->file->text, view_cursor_get(&win->view));
	if (cursor == EMARK) return;

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
		.file = win->file,
		.cursor = cursor,
	};
	win->jumphist_count++;
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
		int column = text_line_char_get(jump.file->text, pos);

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
		vis_jumphist_advance(vis, -1);
	else if (motion == VIS_JUMPHIST_NEXT)
		vis_jumphist_advance(vis, 1);
	else if (motion == VIS_JUMPHIST_SHOW)
		vis_jumphist_show(vis);

	// Win *win = vis->win;
	// Text *txt = win->file->text;
	//
	// Buffer jumps = {0};
	// for (size_t i = 0; i < VIS_MARK_SET_LRU_COUNT; i++) {
	// 	size_t cursor = (win->mark_set_lru_cursor + i) % VIS_MARK_SET_LRU_COUNT;
	// 	SelectionRegionList *next = win->mark_set_lru_regions + cursor;
	// 	if (next->count) {
	// 		for (VisDACount j =0; j< next->count; j++) {
	//
	// 			size_t pos = text_mark_get(txt, next->data[j].cursor);
	// 			if (pos == EPOS) continue;
	// 			size_t line = text_lineno_by_pos(txt,pos);
	// 			int column = text_line_char_get(txt,pos) + 1;
	// 			char file_name[256]	= {0};
	// 			snprintf(file_name, sizeof(file_name), "%.*s", win->file->name.length ? (int)win->file->name.length : 9, (int)win->file->name.length ? (char *)win->file->name.data : "[No Name]");
	// 			if (!vis_buffer_appendf(&jumps, "%s [%zu] %zu:%d\n",
	// 										file_name, i, line, column
	// 						)){
	// 				buffer_release(&jumps);
	// 				return;
	// 			}
	// 		}
	// 	}
	// }
	// vis_buffer_terminate(&jumps);
	// vis_message_show(vis, buffer_content0(&jumps));
	// buffer_release(&jumps);
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
