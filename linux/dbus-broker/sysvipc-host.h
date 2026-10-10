#ifndef MATON_SYSVIPC_HOST_H
#define MATON_SYSVIPC_HOST_H
/* Start the per-app SysV IPC supervisor and its "<bus_path>-sysvipc" socket.
 * Non-fatal: on failure SysV calls keep failing with ENOSYS. */
int maton_sysvipc_host_start(const char *bus_path);
void maton_sysvipc_host_stop(void);
#endif
