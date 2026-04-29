/* ────────────────────────────────────────────────────────────────
 *  syscalls_stubs.c — stubs propios para evitar las warnings de
 *  libc_nano/libnosys ("_close is not implemented..." etc).
 *
 *  Las versiones de libnosys llevan secciones .gnu.warning que el
 *  linker imprime aunque estén GC-ed. Proveyendo aquí nuestras propias
 *  implementaciones (no-op, retornan -1) sustituimos las de libnosys
 *  y desaparecen las warnings — sin cambiar el comportamiento real
 *  (estas syscalls no se usan en el firmware del Daisy).
 * ──────────────────────────────────────────────────────────────── */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#undef errno
extern int errno;

int _close(int file)               { (void)file; errno = ENOSYS; return -1; }
int _fstat(int file, struct stat* st) { (void)file; if (st) st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)              { (void)file; return 1; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char* ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
int _write(int file, char* ptr, int len) { (void)file; (void)ptr; return len; }
int _kill(int pid, int sig)        { (void)pid; (void)sig; errno = ENOSYS; return -1; }
int _getpid(void)                  { return 1; }
