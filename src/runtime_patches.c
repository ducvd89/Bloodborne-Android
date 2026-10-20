/* Who wrote the game's code: the notes the port's patchers leave (runtime.h). Written while the game is
 * loaded, one thread; asked on a refused hook, also at load. Consecutive writes of one owner that touch or
 * overlap make one note. */
#include "runtime.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct { uintptr_t start, end; const char *owner; } PatchNote;
static PatchNote *notes;
static size_t note_count, note_capacity;
static pthread_mutex_t notes_lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_code_patch_note(uintptr_t address, size_t size, const char *owner) {
    if (!size || !owner) return;
    pthread_mutex_lock(&notes_lock);
    if (note_count) {
        PatchNote *last = &notes[note_count - 1];
        if (last->owner == owner && address <= last->end && address + size >= last->start) {
            if (address < last->start) last->start = address;
            if (address + size > last->end) last->end = address + size;
            pthread_mutex_unlock(&notes_lock);
            return;
        }
    }
    if (note_count == note_capacity) {
        size_t capacity = note_capacity ? note_capacity * 2 : 256;
        PatchNote *grown = realloc(notes, capacity * sizeof(*notes));
        if (!grown) { pthread_mutex_unlock(&notes_lock); return; }
        notes = grown;
        note_capacity = capacity;
    }
    notes[note_count++] = (PatchNote){address, address + size, owner};
    pthread_mutex_unlock(&notes_lock);
}

const char *runtime_code_patch_owner(uintptr_t address, size_t size) {
    const char *found = NULL;
    pthread_mutex_lock(&notes_lock);
    for (size_t i = 0; i < note_count; ++i) {
        if (notes[i].start < address + size && address < notes[i].end) { found = notes[i].owner; break; }
    }
    pthread_mutex_unlock(&notes_lock);
    return found;
}
