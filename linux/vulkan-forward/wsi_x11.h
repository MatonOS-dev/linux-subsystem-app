#ifndef MATON_WSI_X11_H
#define MATON_WSI_X11_H
#include <stdint.h>
struct mv_x11;
struct mv_x11_buffer;
int mv_x11_visual_supported(void *,uint32_t);
struct mv_x11 *mv_x11_create(void *connection,uint32_t window);
void *mv_x11_connection(void *display);
void mv_x11_set_mailbox(struct mv_x11 *,int);
void mv_x11_set_extent(struct mv_x11 *,uint32_t,uint32_t);
int mv_x11_suboptimal(struct mv_x11 *);
int mv_x11_has_alpha(struct mv_x11 *);
void mv_x11_destroy(struct mv_x11 *);
int mv_x11_extent(struct mv_x11 *,uint32_t *,uint32_t *);
extern int mv_x11_last_error;
/* Optional mirror for diagnostics (logcat); set by the ICD, NULL in tests. */
extern void (*mv_x11_log_hook)(const char *,unsigned long);
struct mv_x11_buffer *mv_x11_buffer_create(struct mv_x11 *,int,uint32_t,uint32_t,uint32_t);
void mv_x11_buffer_destroy(struct mv_x11_buffer *);
/* Connected socket carrying the AHB for a DRI3 stand-in fd, or -1. */
int mv_x11_ahb_lookup(int standin);
int mv_x11_ahb_register(int standin,int transport);
void mv_x11_ahb_unregister(int fd);
int mv_x11_present(struct mv_x11_buffer *,int);
int mv_x11_released(struct mv_x11_buffer *);
/* >0: progress, 0: timeout, <0: connection lost */
int mv_x11_dispatch(struct mv_x11 *,int);
#endif
