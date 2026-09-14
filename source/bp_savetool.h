/* bp_savetool.h -- apply save.txt to Profile.Save at boot. MIT. */
#ifndef BP_SAVETOOL_H
#define BP_SAVETOOL_H
/* Reads <root>/save.txt (writing a fully commented template the first time) and
 * writes every uncommented value into each Profile.Save under <root>/files,
 * before the engine starts. */
void bp_savetool_run(void);
#endif
