/* editbox.h -- the Switch software keyboard (swkbd) behind Unity's soft input.
 * From daggerfall_nx / pvz_fusion_en_nx's editbox (MIT), extended: Unity's keyboard
 * type, password flag, placeholder and character limit reach swkbd, and the result
 * is delivered at a frame boundary, the way Android's UI thread does it. */
#ifndef __EDITBOX_H__
#define __EDITBOX_H__

/* Deferred (UnityPlayer.showSoftInput): record the request and return at once;
 * editbox_pump() shows swkbd and delivers the result between frames. */
void editbox_request(const char *initial, int unity_type, int secure, int multiline,
                     const char *placeholder, int char_limit);
/* Once per frame, from the main loop, outside any engine call. */
void editbox_pump(void);

/* Synchronous (String-returning keyboard calls): show now, result in editbox_text(). */
void editbox_show(const char *initial, int maxlen);

int  editbox_is_open(void);          /* a keyboard is pending or on screen */
const char *editbox_text(void);      /* last entered text (stable pointer) */
int  editbox_cancelled(void);        /* last keyboard was cancelled */
void editbox_close(void);            /* hideSoftInput: drop a request not yet shown */
void editbox_mark_engine_driven(void);
int  editbox_engine_drives(void);

#endif
