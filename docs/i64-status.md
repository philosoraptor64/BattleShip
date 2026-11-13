# Super Smash Bros. in i64: status

ROM: Super Smash Bros. (USA) v1.0, SHA-1 e2929e10fccc0aa84e5776227e798abc07cedabf  
Route: A, BattleShip (decomp + libultraship)  
Owner: MacBook agent  
Process and gate definitions: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence |
|---|---|---|
| 0 Intake | done |  |
| 1 Code map | n/a | existing port |
| 2 Builds | done |  |
| 3 Boots and plays | done |  |
| 4 Correctness | partial | to verify (owner) |
| 5 Determinism | done | SyncTest 0 mismatches |
| 6 i64 module | done | coroutine pool from the arena (9b8622f) |
| 7 Online (lockstep) | done | sessions from the title screen |
| 8 Rollback | done | rollback in VS |
| 9 Multiplayer features | partial | 1P modes in a session / observation mode open |
| 10 iPhone | done | phones, cellular test |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; owners please correct).

Known issues / open:
- to verify (owner)
