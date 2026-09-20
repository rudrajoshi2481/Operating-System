#pragma once

#include <stdint.h>

/* gate.c — the agent gate (Step 13). The only action space an external
 * model gets: act <verb> <args> over the host channel. Every action is
 * audited to the object store; instrument execution requires an
 * approval that a human grants on the serial console. */
int  gate_act(const char *line, char *out, uint32_t cap);
void gate_approve(uint32_t reqid);      /* serial shell: approve req */
int  gate_fmt_audit(char *buf, uint32_t cap);   /* sys://audit */
int  gate_fmt_pending(char *buf, uint32_t cap); /* sys://approvals */
