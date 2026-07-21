#ifndef ATTACK_MOVE_H
#define ATTACK_MOVE_H

// Master switch for the "attack while walking/running" feature.
// Set to 0 to compile it out entirely (the call site in main.c is guarded too).
#define GOEMON_ATTACK_MOVE 1

#if GOEMON_ATTACK_MOVE
void attack_move_tick(void);
#endif

#endif // ATTACK_MOVE_H
