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
| 4 Correctness | partial | user ran 1P mode, menus, VS and results on the iPhone (2026-11-07); white results screen fixed (Metal color mask) |
| 5 Determinism | done | SyncTest 0 mismatches |
| 6 i64 module | done | coroutine pool from the arena, released at stop (9b8622f; peak 10 of 128 slots in a VS battle); address space flat per run; heap during play 77 MB after the LUS texture-buffer fix (e014299) |
| 7 Online (lockstep) | done | sessions from the title screen |
| 8 Rollback | done | rollback in VS |
| 9 Multiplayer features | partial | 1P modes in a session / observation mode open |
| 10 iPhone | done | phones, cellular test |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; corrected by the MacBook agent, owner).

Known issues / open:
- 1P / Training / Bonus in a session: the friend should watch (observation mode, not built)
- the voice button covers the battle timer's last digit
- footprint left after a run ~115-140 MB on the Mac, grows ~10 MB per run: not attributed yet
