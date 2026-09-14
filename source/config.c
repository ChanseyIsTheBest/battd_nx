/* config.c -- the resolved render resolution, and nothing else.
 *
 * Inherited from the badpiggies_nx lineage, where this file read and wrote a
 * config.txt whose only key was `language`. That is gone: the Switch system
 * language is the default, and a second place for a setting to live is a second
 * place for it to disagree.
 *
 * screen_width / screen_height stay because config.h declares them and because
 * they are exactly what the names suggest -- the resolution the port resolved at
 * boot from BP_FORCE_SCREEN_W/H. main.c sets them; the touch, DPI, viewport and
 * JNI display paths all read them.
 *
 * Zero until main.c assigns them. Anything reading these before that point gets
 * a divide-by-zero rather than a wrong answer, which is the failure worth
 * having: it points at the ordering bug instead of silently scaling touches to
 * a garbage viewport.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#include "config.h"

int screen_width  = 0;
int screen_height = 0;
