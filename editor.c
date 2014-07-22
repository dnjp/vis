#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "editor.h"

#define MAX(a, b)  ((a) < (b) ? (b) : (a))

#define BUFFER_SIZE (1 << 20)

/* Buffer holding the file content, either readonly mmap(2)-ed from the original
 * file or heap allocated to store the modifications.
 */
typedef struct Buffer Buffer;
struct Buffer {
	size_t size;            /* maximal capacity */
	size_t pos;             /* current insertion position */ // TODO: rename to len
	char *content;          /* actual data */
	Buffer *next;           /* next junk */
};

/* A piece holds a reference (but doesn't itself store) a certain amount of data.
 * All active pieces chained together form the whole content of the document.
 * At the beginning there exists only one piece, spanning the whole document.
 * Upon insertion/delition new pieces will be created to represent the changes.
 * Generally pieces are never destroyed, but kept around to peform undo/redo operations.
 */
typedef struct Piece Piece;
struct Piece {
	Editor *editor;                   /* editor to which this piece belongs */
	Piece *prev, *next;               /* pointers to the logical predecessor/successor */
	Piece *global_prev, *global_next; /* double linked list in order of allocation, used to free individual pieces */
	char *content;                    /* pointer into a Buffer holding the data */
	size_t len;                       /* the lenght in number of bytes starting from content */
	// size_t line_count;
	int index;                        /* unique index identifiying the piece */
};

typedef struct {
	Piece *piece;
	size_t off;
} Location;

/* A Span holds a certain range of pieces. Changes to the document are allways
 * performed by swapping out an existing span with a new one.
 */
typedef struct {
	Piece *start, *end;     /* start/end of the span */
	size_t len;             /* the sum of the lenghts of the pieces which form this span */
	// size_t line_count;
} Span;

/* A Change keeps all needed information to redo/undo an insertion/deletion. */
typedef struct Change Change;
struct Change {
	Span old;               /* all pieces which are being modified/swapped out by the change */
	Span new;               /* all pieces which are introduced/swapped in by the change */
	Change *next;
};

/* An Action is a list of Changes which are used to undo/redo all modifications
 * since the last snapshot operation. Actions are kept in an undo and a redo stack.
 */
typedef struct Action Action;
struct Action {
	Change *change;         /* the most recent change */
	Action *next;           /* next action in the undo/redo stack */
	time_t time;            /* when the first change of this action was performed */
};

/* The main struct holding all information of a given file */
struct Editor {
	Buffer buf;             /* original mmap(2)-ed file content at the time of load operation */
	Buffer *buffers;        /* all buffers which have been allocated to hold insertion data */
	Piece *pieces;		/* all pieces which have been allocated, used to free them */
	Piece *cache;           /* most recently modified piece */
	int piece_count;	/* number of pieces allocated, only used for debuging purposes */
	Piece begin, end;       /* sentinel nodes which always exists but don't hold any data */
	Action *redo, *undo;    /* two stacks holding all actions performed to the file */
	Action *current_action; /* action holding all file changes until a snapshot is performed */
	Action *saved_action;   /* the last action at the time of the save operation */
	size_t size;            /* current file content size in bytes */
	const char *filename;   /* filename of which data was loaded */
	struct stat info;	/* stat as proped on load time */
	int fd;                 /* the file descriptor of the original mmap-ed data */
};

/* buffer management */
static Buffer *buffer_alloc(Editor *ed, size_t size);
static void buffer_free(Buffer *buf);
static bool buffer_capacity(Buffer *buf, size_t len);
static char *buffer_append(Buffer *buf, char *content, size_t len);
static bool buffer_insert(Buffer *buf, size_t pos, char *content, size_t len);
static bool buffer_delete(Buffer *buf, size_t pos, size_t len);
static char *buffer_store(Editor *ed, char *content, size_t len);
/* cache layer */
static void cache_piece(Editor *ed, Piece *p);
static bool cache_contains(Editor *ed, Piece *p);
static bool cache_insert(Editor *ed, Piece *p, size_t off, char *text, size_t len);
static bool cache_delete(Editor *ed, Piece *p, size_t off, size_t len);
/* piece management */
static Piece *piece_alloc(Editor *ed);
static void piece_free(Piece *p);
static void piece_init(Piece *p, Piece *prev, Piece *next, char *content, size_t len);
static Location piece_get(Editor *ed, size_t pos);
/* span management */
static void span_init(Span *span, Piece *start, Piece *end);
static void span_swap(Editor *ed, Span *old, Span *new);
/* change management */
static Change *change_alloc(Editor *ed);
static void change_free(Change *c);
/* action management */
static Action *action_alloc(Editor *ed);
static void action_free(Action *a);
static void action_push(Action **stack, Action *action);
static Action *action_pop(Action **stack);

/* allocate a new buffer of MAX(size, BUFFER_SIZE) bytes */
static Buffer *buffer_alloc(Editor *ed, size_t size) {
	Buffer *buf = calloc(1, sizeof(Buffer));
	if (!buf)
		return NULL;
	if (BUFFER_SIZE > size)
		size = BUFFER_SIZE;
	if (!(buf->content = malloc(size))) {
		free(buf);
		return NULL;
	}
	buf->size = size;
	buf->next = ed->buffers;
	ed->buffers = buf;
	return buf;
}

static void buffer_free(Buffer *buf) {
	if (!buf)
		return;
	free(buf->content);
	free(buf);
}

/* check whether buffer has enough free space to store len bytes */
static bool buffer_capacity(Buffer *buf, size_t len) {
	return buf->size - buf->pos >= len;
}

/* append data to buffer, assumes there is enough space available */
static char *buffer_append(Buffer *buf, char *content, size_t len) {
	char *dest = memcpy(buf->content + buf->pos, content, len);
	buf->pos += len;
	return dest;
}

/* stores the given data in a buffer, allocates a new one if necessary. returns
 * a pointer to the storage location or NULL if allocation failed. */
static char *buffer_store(Editor *ed, char *content, size_t len) {
	Buffer *buf = ed->buffers;
	if ((!buf || !buffer_capacity(buf, len)) && !(buf = buffer_alloc(ed, len)))
		return NULL;
	return buffer_append(buf, content, len);
}

/* insert data into buffer at an arbitrary position, this should only be used with
 * data of the most recently created piece. */
static bool buffer_insert(Buffer *buf, size_t pos, char *content, size_t len) {
	if (pos > buf->pos || !buffer_capacity(buf, len))
		return false;
	if (buf->pos == pos)
		return buffer_append(buf, content, len);
	char *insert = buf->content + pos;
	memmove(insert + len, insert, buf->pos - pos);
	memcpy(insert, content, len);
	buf->pos += len;
	return true;
}

/* delete data from a buffer at an arbitrary position, this should only be used with
 * data of the most recently created piece. */
static bool buffer_delete(Buffer *buf, size_t pos, size_t len) {
	if (pos + len > buf->pos)
		return false;
	if (buf->pos == pos) {
		buf->pos -= len;
		return true;
	}
	char *delete = buf->content + pos;
	memmove(delete, delete + len, buf->pos - pos - len);
	buf->pos -= len;
	return true;
}

/* cache the given piece if it is the most recently changed one */
static void cache_piece(Editor *ed, Piece *p) {
	Buffer *buf = ed->buffers;
	if (!buf || p->content < buf->content || p->content + p->len != buf->content + buf->pos)
		return;
	ed->cache = p;
}

/* check whether the given piece was the most recently modified one */
static bool cache_contains(Editor *ed, Piece *p) {
	Buffer *buf = ed->buffers;
	Action *a = ed->current_action;
	if (!buf || !ed->cache || ed->cache != p || !a || !a->change)
		return false;

	Piece *start = a->change->new.start;
	Piece *end = a->change->new.start;
	bool found = false;
	for (Piece *cur = start; !found; cur = cur->next) {
		if (cur == p)
			found = true;
		if (cur == end)
			break;
	}

	return found && p->content + p->len == buf->content + buf->pos;
}

/* try to insert a junk of data at a given piece offset. the insertion is only
 * performed if the piece is the most recenetly changed one. the legnth of the
 * piece, the span containing it and the whole text is adjusted accordingly */
static bool cache_insert(Editor *ed, Piece *p, size_t off, char *text, size_t len) {
	if (!cache_contains(ed, p))
		return false;
	Buffer *buf = ed->buffers;
	size_t bufpos = p->content + off - buf->content;
	if (!buffer_insert(buf, bufpos, text, len))
		return false;
	p->len += len;
	ed->current_action->change->new.len += len;
	ed->size += len;
	return true;
}

/* try to delete a junk of data at a given piece offset. the deletion is only
 * performed if the piece is the most recenetly changed one and the whole
 * affected range lies within it. the legnth of the piece, the span containing it
 * and the whole text is adjusted accordingly */
static bool cache_delete(Editor *ed, Piece *p, size_t off, size_t len) {
	if (!cache_contains(ed, p))
		return false;
	Buffer *buf = ed->buffers;
	size_t bufpos = p->content + off - buf->content;
	if (off + len > p->len || !buffer_delete(buf, bufpos, len))
		return false;
	p->len -= len;
	ed->current_action->change->new.len -= len;
	ed->size -= len;
	return true;
}

/* initialize a span and calculate its length */
static void span_init(Span *span, Piece *start, Piece *end) {
	size_t len = 0;
	span->start = start;
	span->end = end;
	for (Piece *p = start; p; p = p->next) {
		len += p->len;
		if (p == end)
			break;
	}
	span->len = len;
}

/* swap out an old span and replace it with a new one.
 *
 *  - if old is an empty span do not remove anything, just insert the new one
 *  - if new is an empty span do not insert anything, just remove the old one
 *
 * adjusts the document size accordingly.
 */
static void span_swap(Editor *ed, Span *old, Span *new) {
	/* TODO use a balanced search tree to keep the pieces
		instead of a doubly linked list.
	 */
	if (old->len == 0 && new->len == 0) {
		return;
	} else if (old->len == 0) {
		/* insert new span */
		new->start->prev->next = new->start;
		new->end->next->prev = new->end;
	} else if (new->len == 0) {
		/* delete old span */
		old->start->prev->next = old->end->next;
		old->end->next->prev = old->start->prev;
	} else {
		/* replace old with new */
		old->start->prev->next = new->start;
		old->end->next->prev = new->end;
	}
	ed->size -= old->len;
	ed->size += new->len;
}

static void action_push(Action **stack, Action *action) {
	action->next = *stack;
	*stack = action;
}

static Action *action_pop(Action **stack) {
	Action *action = *stack;
	if (action)
		*stack = action->next;
	return action;
}

/* allocate a new action, empty the redo stack and push the new action onto
 * the undo stack. all further changes will be associated with this action. */
static Action *action_alloc(Editor *ed) {
	Action *old, *new = calloc(1, sizeof(Action));
	if (!new)
		return NULL;
	new->time = time(NULL);
	/* throw a away all old redo operations */
	while ((old = action_pop(&ed->redo)))
		action_free(old);
	ed->current_action = new;
	action_push(&ed->undo, new);
	return new;
}

static void action_free(Action *a) {
	if (!a)
		return;
	for (Change *next, *c = a->change; c; c = next) {
		next = c->next;
		change_free(c);
	}
	free(a);
}

static Piece *piece_alloc(Editor *ed) {
	Piece *p = calloc(1, sizeof(Piece));
	if (!p)
		return NULL;
	p->editor = ed;
	p->index = ++ed->piece_count;
	p->global_next = ed->pieces;
	if (ed->pieces)
		ed->pieces->global_prev = p;
	ed->pieces = p;
	return p;
}

static void piece_free(Piece *p) {
	if (!p)
		return;
	if (p->global_prev)
		p->global_prev->global_next = p->global_next;
	if (p->global_next)
		p->global_next->global_prev = p->global_prev;
	if (p->editor->pieces == p)
		p->editor->pieces = p->global_next;
	if (p->editor->cache == p)
		p->editor->cache = NULL;
	free(p);
}

static void piece_init(Piece *p, Piece *prev, Piece *next, char *content, size_t len) {
	p->prev = prev;
	p->next = next;
	p->content = content;
	p->len = len;
}

/* returns the piece holding the text at byte offset pos.
 * if pos is zero, then the begin sentinel piece is returned. */
static Location piece_get(Editor *ed, size_t pos) {
	Location loc = {};
	// TODO: handle position at end of file: pos+1
	size_t cur = 0;
	for (Piece *p = &ed->begin; p->next; p = p->next) {
		if (cur <= pos && pos <= cur + p->len) {
			loc.piece = p;
			loc.off = pos - cur;
			return loc;
		}
		cur += p->len;
	}

	return loc;
}

/* allocate a new change, associate it with current action or a newly
 * allocated one if none exists. */
static Change *change_alloc(Editor *ed) {
	Action *a = ed->current_action;
	if (!a) {
		a = action_alloc(ed);
		if (!a)
			return NULL;
	}
	Change *c = calloc(1, sizeof(Change));
	if (!c)
		return NULL;
	c->next = a->change;
	a->change = c;
	return c;
}

static void change_free(Change *c) {
	/* only free the new part of the span, the old one is still in use */
	piece_free(c->new.start);
	if (c->new.start != c->new.end)
		piece_free(c->new.end);
	free(c);
}

/* When inserting new data there are 2 cases to consider.
 *
 *  - in the first the insertion point falls into the middle of an exisiting
 *    piece which is replaced by three new pieces:
 *
 *      /-+ --> +---------------+ --> +-\
 *      | |     | existing text |     | |
 *      \-+ <-- +---------------+ <-- +-/
 *                         ^
 *                         Insertion point for "demo "
 *
 *      /-+ --> +---------+ --> +-----+ --> +-----+ --> +-\
 *      | |     | existing|     |demo |     |text |     | |
 *      \-+ <-- +---------+ <-- +-----+ <-- +-----+ <-- +-/
 *
 *  - the second case deals with an insertion point at a piece boundry:
 *
 *      /-+ --> +---------------+ --> +-\
 *      | |     | existing text |     | |
 *      \-+ <-- +---------------+ <-- +-/
 *            ^
 *            Insertion point for "short"
 *
 *      /-+ --> +-----+ --> +---------------+ --> +-\
 *      | |     |short|     | existing text |     | |
 *      \-+ <-- +-----+ <-- +---------------+ <-- +-/
 */

bool editor_insert(Editor *ed, size_t pos, char *text) {
	size_t len = strlen(text); // TODO

	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	size_t off = loc.off;
	if (cache_insert(ed, p, off, text, len))
		return true;

	Change *c = change_alloc(ed);
	if (!c)
		return false;

	if (!(text = buffer_store(ed, text, len)))
		return false;

	Piece *new = NULL;

	if (off == p->len) {
		/* insert between two existing pieces, hence there is nothing to
		 * remove, just add a new piece holding the extra text */
		if (!(new = piece_alloc(ed)))
			return false;
		piece_init(new, p, p->next, text, len);
		span_init(&c->new, new, new);
		span_init(&c->old, NULL, NULL);
	} else {
		/* insert into middle of an existing piece, therfore split the old
		 * piece. that is we have 3 new pieces one containing the content
		 * before the insertion point then one holding the newly inserted
		 * text and one holding the content after the insertion point.
		 */
		Piece *before = piece_alloc(ed);
		new = piece_alloc(ed);
		Piece *after = piece_alloc(ed);
		if (!before || !new || !after)
			return false;
		// TODO: check index calculation
		piece_init(before, p->prev, new, p->content, off);
		piece_init(new, before, after, text, len);
		piece_init(after, new, p->next, p->content + off, p->len - off);

		span_init(&c->new, before, after);
		span_init(&c->old, p, p);
	}

	cache_piece(ed, new);
	span_swap(ed, &c->old, &c->new);
	return true;
}

/* undo all changes of the last action, return whether changes existed */
bool editor_undo(Editor *ed) {
	Action *a = action_pop(&ed->undo);
	if (!a)
		return false;
	for (Change *c = a->change; c; c = c->next) {
		span_swap(ed, &c->new, &c->old);
	}

	action_push(&ed->redo, a);
	return true;
}

/* redo all changes of the last action, return whether changes existed */
bool editor_redo(Editor *ed) {
	Action *a = action_pop(&ed->redo);
	if (!a)
		return false;
	for (Change *c = a->change; c; c = c->next) {
		span_swap(ed, &c->old, &c->new);
	}

	action_push(&ed->undo, a);
	return true;
}

bool copy_content(void *data, size_t pos, const char *content, size_t len) {
	char **p = (char **)data;
	memcpy(*p, content, len);
	*p += len;
	return true;
}

/* save current content to given filename. the data is first saved to
 * a file called `.filename.tmp` and then atomically moved to its final
 * (possibly alredy existing) destination using rename(2).
 */
int editor_save(Editor *ed, const char *filename) {
	size_t len = strlen(filename) + 10;
	char tmpname[len];
	snprintf(tmpname, len, ".%s.tmp", filename);
	// TODO file ownership, permissions etc
	/* O_RDWR is needed because otherwise we can't map with MAP_SHARED */
	int fd = open(tmpname, O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return -1;
	if (ftruncate(fd, ed->size) == -1)
		goto err;
	if (ed->size > 0) {
		void *buf = mmap(NULL, ed->size, PROT_WRITE, MAP_SHARED, fd, 0);
		if (buf == MAP_FAILED)
			goto err;

		void *cur = buf;
		editor_iterate(ed, &cur, 0, copy_content);

		if (munmap(buf, ed->size) == -1)
			goto err;
	}
	if (close(fd) == -1)
		return -1;
	if (rename(tmpname, filename) == -1)
		return -1;
	ed->saved_action = ed->undo;
	editor_snapshot(ed);
err:
	close(fd);
	return -1;
}

/* load the given file as starting point for further editing operations.
 * to start with an empty document, pass NULL as filename. */
Editor *editor_load(const char *filename) {
	Editor *ed = calloc(1, sizeof(Editor));
	if (!ed)
		return NULL;
	ed->begin.index = 1;
	ed->end.index = 2;
	ed->piece_count = 2;
	piece_init(&ed->begin, NULL, &ed->end, NULL, 0);
	piece_init(&ed->end, &ed->begin, NULL, NULL, 0);
	if (filename) {
		ed->filename = filename;
		ed->fd = open(filename, O_RDONLY);
		if (ed->fd == -1)
			goto out;
		if (fstat(ed->fd, &ed->info) == -1)
			goto out;
		if (!S_ISREG(ed->info.st_mode))
			goto out;
		// XXX: use lseek(fd, 0, SEEK_END); instead?
		ed->buf.size = ed->info.st_size;
		ed->buf.content = mmap(NULL, ed->info.st_size, PROT_READ, MAP_SHARED, ed->fd, 0);
		if (ed->buf.content == MAP_FAILED)
			goto out;

		Piece *p = piece_alloc(ed);
		if (!p)
			goto out;
		piece_init(&ed->begin, NULL, p, NULL, 0);
		piece_init(p, &ed->begin, &ed->end, ed->buf.content, ed->buf.size);
		piece_init(&ed->end, p, NULL, NULL, 0);
		ed->size = ed->buf.size;
	}
	return ed;
out:
	if (ed->fd > 2)
		close(ed->fd);
	editor_free(ed);
	return NULL;
}

static void print_piece(Piece *p) {
	fprintf(stderr, "index: %d\tnext: %d\tprev: %d\t len: %d\t content: %p\n", p->index,
		p->next ? p->next->index : -1,
		p->prev ? p->prev->index : -1,
		p->len, p->content);
	fflush(stderr);
	write(1, p->content, p->len);
	write(1, "\n", 1);
}

void editor_debug(Editor *ed) {
	for (Piece *p = &ed->begin; p; p = p->next) {
		print_piece(p);
	}
}

void editor_iterate(Editor *ed, void *data, size_t pos, iterator_callback_t callback) {
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	if (!p)
		return;
	size_t len = p->len - loc.off;
	char *content = p->content + loc.off;
	while (p && callback(data, pos, content, len)) {
		pos += len;
		p = p->next;
		if (!p)
			return;
		content = p->content;
		len = p->len;
	}
}

/* A delete operation can either start/stop midway through a piece or at
 * a boundry. In the former case a new piece is created to represent the
 * remaining text before/after the modification point.
 *
 *      /-+ --> +---------+ --> +-----+ --> +-----+ --> +-\
 *      | |     | existing|     |demo |     |text |     | |
 *      \-+ <-- +---------+ <-- +-----+ <-- +-----+ <-- +-/
 *                   ^                         ^
 *                   |------ delete range -----|
 *
 *      /-+ --> +----+ --> +--+ --> +-\
 *      | |     | exi|     |t |     | |
 *      \-+ <-- +----+ <-- +--+ <-- +-/
 */
bool editor_delete(Editor *ed, size_t pos, size_t len) {
	if (len == 0)
		return true;
	if (pos + len > ed->size)
		return false;
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	size_t off = loc.off;
	if (cache_delete(ed, p, off, len))
		return true;
	size_t cur; // how much has already been deleted
	bool midway_start = false, midway_end = false;
	Change *c = change_alloc(ed);
	if (!c)
		return false;
	Piece *before, *after; // unmodified pieces before / after deletion point
	Piece *start, *end; // span which is removed
	if (off == p->len) {
		/* deletion starts at a piece boundry */
		cur = 0;
		before = p;
		start = p->next;
	} else {
		/* deletion starts midway through a piece */
		midway_start = true;
		cur = p->len - off;
		start = p;
		before = piece_alloc(ed);
	}
	/* skip all pieces which fall into deletion range */
	while (cur < len) {
		p = p->next;
		cur += p->len;
	}

	if (cur == len) {
		/* deletion stops at a piece boundry */
		end = p;
		after = p->next;
	} else { // cur > len
		/* deletion stops midway through a piece */
		midway_end = true;
		end = p;
		after = piece_alloc(ed);
		piece_init(after, before, p->next, p->content + p->len - (cur - len), cur - len);
	}

	if (midway_start) {
		/* we finally know which piece follows our newly allocated before piece */
		piece_init(before, start->prev, after, start->content, off);
	}

	Piece *new_start = NULL, *new_end = NULL;
	if (midway_start) {
		new_start = before;
		if (!midway_end)
			new_end = before;
	}
	if (midway_end) {
		if (!midway_start)
			new_start = after;
		new_end = after;
	}

	span_init(&c->new, new_start, new_end);
	span_init(&c->old, start, end);
	span_swap(ed, &c->old, &c->new);
	return true;
}

bool editor_replace(Editor *ed, size_t pos, char *c) {
	// TODO argument validation: pos etc.
	size_t len = strlen(c);
	editor_delete(ed, pos, len);
	editor_insert(ed, pos, c);
	return true;
}

/* preserve the current text content such that it can be restored by
 * means of undo/redo operations */
void editor_snapshot(Editor *ed) {
	ed->current_action = NULL;
	ed->cache = NULL;
}

void editor_free(Editor *ed) {
	if (!ed)
		return;

	Action *a;
	while ((a = action_pop(&ed->undo)))
		action_free(a);
	while ((a = action_pop(&ed->redo)))
		action_free(a);

	for (Piece *next, *p = ed->pieces; p; p = next) {
		next = p->global_next;
		piece_free(p);
	}

	for (Buffer *next, *buf = ed->buffers; buf; buf = next) {
		next = buf->next;
		buffer_free(buf);
	}

	if (ed->buf.content)
		munmap(ed->buf.content, ed->buf.size);

	free(ed);
}

bool editor_modified(Editor *ed) {
	return ed->saved_action != ed->undo;
}

Iterator editor_iterator_get(Editor *ed, size_t pos) {
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	if (p == &ed->begin)
		p = p->next;
	return (Iterator){
		.piece = p,
		.text = p ? p->content + loc.off : NULL,
		.len = p ? p->len - loc.off : 0,
	};
}

void editor_iterator_next(Iterator *it) {
	Piece *p = it->piece ? it->piece->next : NULL;
	*it = (Iterator){
		.piece = p,
		.text = p ? p->content : NULL,
		.len = p ? p->len : 0,
	};
}

void editor_iterator_prev(Iterator *it) {
	Piece *p = it->piece ? it->piece->prev : NULL;
	*it = (Iterator){
		.piece = p,
		.text = p ? p->content : NULL,
		.len = p ? p->len : 0,
	};
}

bool editor_iterator_valid(const Iterator *it) {
	/* filter out sentinel nodes */
	return it->piece && it->piece->editor;
}
