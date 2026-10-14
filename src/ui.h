#ifndef SPARSEGET_UI_H
#define SPARSEGET_UI_H
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

void ui_init(void);
int ui_enabled(void);
void ui_end_progress(void);
void ui_error(const char *message);
void ui_created(const char *path, off_t size);
void ui_written(const char *path, off_t offset, size_t bytes);
void ui_status(const char *path, off_t size, uintmax_t blocks,
               uintmax_t allocated, double saving, int supported, off_t holes);
void ui_start(const char *path, off_t offset, off_t size);
void ui_progress(off_t written, off_t size, off_t saved);
void ui_paused(off_t saved, off_t size);
void ui_complete(off_t saved, off_t size);
void ui_help(void);
#endif
