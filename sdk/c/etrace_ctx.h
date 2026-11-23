/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ETRACE_CTX_H
#define ETRACE_CTX_H

#include <stdint.h>

#define ETRACE_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/* Publishes the active trace context of the calling thread so the eTrace
 * agent can attribute off-CPU time to it. Passing a zero trace id clears it.
 * Safe to call from any thread; never blocks. */
ETRACE_API void etrace_set_context(const uint8_t trace_id[16], const uint8_t span_id[8]);

ETRACE_API void etrace_clear_context(void);

/* Uprobe attach point used by the agent. Not intended to be called directly. */
ETRACE_API void etrace_register_tls(void *tls);

#ifdef __cplusplus
}
#endif

#endif
