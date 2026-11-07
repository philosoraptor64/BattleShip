/* Game-side queries for port/rollback (compiled with the decomp sources). */
#include <sc/scene.h>

/* A VS battle is live: in its scene, counting down or under way. Rollback
 * only snapshots and resimulates such frames; scene transitions tear the
 * battle's objects down and are never rewound across. */
int port_rollback_game_in_battle(void)
{
	return (gSCManagerSceneData.scene_curr == nSCKindVSBattle) && (gSCManagerBattleState != NULL) &&
	       ((gSCManagerBattleState->game_status == nSCBattleGameStatusWait) ||
	        (gSCManagerBattleState->game_status == nSCBattleGameStatusGo));
}
