/* hooks.h -- patches to libGame.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include "so_util.h"

// code patches, before so_finalize
void patch_game(so_module *game);
// data writes, after so_finalize
void patch_game_data(so_module *game);

void input_init(void);
void input_update(void);

#endif
