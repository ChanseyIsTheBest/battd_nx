/* bp_vsync.h -- the Android vsync counter, pumped. See bp_vsync.c. MIT. */
#ifndef BP_VSYNC_H
#define BP_VSYNC_H
#include <stdint.h>
int      bp_vsync_init(void);   /* after libunity is mapped; 0 on success */
void     bp_vsync_tick(void);   /* one vsync; called by the pump thread */
int      bp_vsync_start(void);  /* start the pump -- BEFORE recreateGfxState */
void     bp_vsync_stop(void);
uint64_t bp_vsync_frames(void);
#endif
