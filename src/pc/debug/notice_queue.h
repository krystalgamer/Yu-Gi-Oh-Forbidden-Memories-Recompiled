#ifndef MEMORIES_PC_DEBUG_NOTICE_QUEUE_H
#define MEMORIES_PC_DEBUG_NOTICE_QUEUE_H
/* The recorder's notices for the player (recorder.c, notify): a few, in the
 * order they came, each taken once, the menu showing one at a time. It used to
 * be one slot, the last notice winning: "Replay not saved" was lost to the
 * "Recording not started" of the same poll, and to the "Replay saved" of
 * another pack that ended with it. Header only, so that a test can include it.
 * When it is full the oldest of the notices that matter least goes (a success
 * before a failure), and the caller says so on stderr. */
#include <stdio.h>
#include <string.h>

#define NOTICE_QUEUE_MAX 8

typedef struct {
    char title[64];
    char text[700];
    int important; /* a failure, or something the player must know: the last to be dropped */
} QueuedNotice;

typedef struct {
    QueuedNotice items[NOTICE_QUEUE_MAX];
    unsigned first, count;
} NoticeQueue;

static inline QueuedNotice *NoticeQueue_At(NoticeQueue *queue, unsigned index)
{
    return &queue->items[(queue->first + index) % NOTICE_QUEUE_MAX];
}

/* Appends a notice. 1 when the queue was full and an older one was dropped to make room. */
static inline int NoticeQueue_Push(NoticeQueue *queue, const char *title, const char *text, int important)
{
    int dropped = 0;
    QueuedNotice *at;
    if (queue->count == NOTICE_QUEUE_MAX) {
        unsigned drop = 0, i, j;
        for (i = 0; i < queue->count; i++) {
            if (!NoticeQueue_At(queue, i)->important) {
                drop = i;
                break;
            }
        }
        for (j = drop; j + 1 < queue->count; j++) *NoticeQueue_At(queue, j) = *NoticeQueue_At(queue, j + 1);
        queue->count--;
        dropped = 1;
    }
    at = NoticeQueue_At(queue, queue->count++);
    snprintf(at->title, sizeof(at->title), "%s", title);
    snprintf(at->text, sizeof(at->text), "%s", text);
    at->important = important;
    return dropped;
}

/* The oldest notice, taken: 1 with it, 0 when there is none. */
static inline int NoticeQueue_Take(NoticeQueue *queue, char *title, size_t title_size, char *text, size_t text_size)
{
    const QueuedNotice *first;
    if (!queue->count) return 0;
    first = NoticeQueue_At(queue, 0);
    snprintf(title, title_size, "%s", first->title);
    snprintf(text, text_size, "%s", first->text);
    queue->first = (queue->first + 1) % NOTICE_QUEUE_MAX;
    queue->count--;
    return 1;
}

#endif
