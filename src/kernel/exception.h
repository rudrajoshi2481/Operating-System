#pragma once

struct trap_frame;

void sync_exception(struct trap_frame *frame);
void fatal_exception(struct trap_frame *frame, int kind);
