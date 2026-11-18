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

/* A frame rollback may snapshot and re-run: a live VS battle; in an online session that rolls back in the menus too
 * (syNetPeerSessionRollbackEverywhere), every frame: scene changes wait for confirmed inputs (taskman), and port/rollback
 * never rewinds across one (gPortSceneLoads). */
extern int syNetPeerSessionRollbackEverywhere(void);
extern unsigned int gPortSceneLoads;

int port_rollback_game_safe(void)
{
	return syNetPeerSessionRollbackEverywhere() || port_rollback_game_in_battle();
}

unsigned int port_rollback_scene_loads(void)
{
	return gPortSceneLoads;
}
