/* bp_config.h -- config.txt, read at boot. MIT. */
#ifndef BP_CONFIG_H_USER
#define BP_CONFIG_H_USER
/* The game's portrait resolution: width = the "p" number (720..1080), height =
 * width * 16 / 9. Defaults 720 x 1280 until bp_config_load() runs. */
extern int bp_res_w, bp_res_h;
extern int bp_portrait_rot;       /* 1 = 90 CW (right Joy-Con up), 2 = 90 CCW (left Joy-Con up) */
void bp_config_load(void);        /* reads <root>/config.txt (template on first boot) */
void bp_config_sync_prefs(void);  /* keep Unity's saved screen size in step (after prefs load) */
#endif
