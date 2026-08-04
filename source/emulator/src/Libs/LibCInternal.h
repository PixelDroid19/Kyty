#ifndef EMULATOR_SRC_LIBS_LIBCINTERNAL_H_
#define EMULATOR_SRC_LIBS_LIBCINTERNAL_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/MSpace.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Libs.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

class VaList;

namespace LibC {

// Shared internals between the libc HLE module and the LibcInternal/
// LibcInternalExt modules. Kept in a private header so the per-module
// translation units stay decoupled from each other.

extern uint32_t g_need_flag;

using execute_once_callback_t = KYTY_SYSV_ABI int (*)(void*, void*, void**);

int  c_thread_sync_result(int result);
void collect_host_malloc_stats(Core::MSpaceSize* out);

int  KYTY_SYSV_ABI c_cnd_init(LibKernel::PthreadCond* cond);
int  KYTY_SYSV_ABI c_cnd_init_with_name(LibKernel::PthreadCond* cond, const char* name);
int  KYTY_SYSV_ABI c_cnd_init_with_default_name_override(LibKernel::PthreadCond* cond, const char* name);
int  KYTY_SYSV_ABI c_cnd_broadcast(LibKernel::PthreadCond* cond);
int  KYTY_SYSV_ABI c_cnd_signal(LibKernel::PthreadCond* cond);
int  KYTY_SYSV_ABI c_cnd_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex);
int  KYTY_SYSV_ABI c_cnd_timedwait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex,
                                   const LibKernel::KernelTimespec* abstime);
void KYTY_SYSV_ABI c_cnd_destroy(LibKernel::PthreadCond* cond);
void KYTY_SYSV_ABI c_cnd_register_at_thread_exit(LibKernel::PthreadCond* condition,
                                                 LibKernel::PthreadMutex* mutex, int* completed);
void KYTY_SYSV_ABI c_cnd_unregister_at_thread_exit(LibKernel::PthreadMutex* mutex);
void KYTY_SYSV_ABI c_cnd_do_broadcast_at_thread_exit();

int  KYTY_SYSV_ABI c_mtx_init(LibKernel::PthreadMutex* mutex, int type);
int  KYTY_SYSV_ABI c_mtx_init_with_name(LibKernel::PthreadMutex* mutex, int type, const char* name);
int  KYTY_SYSV_ABI c_mtx_init_with_default_name_override(LibKernel::PthreadMutex* mutex, int type, const char* name);
void KYTY_SYSV_ABI c_mtx_destroy(LibKernel::PthreadMutex* mutex);
int  KYTY_SYSV_ABI c_mtx_lock(LibKernel::PthreadMutex* mutex);
int  KYTY_SYSV_ABI c_mtx_trylock(LibKernel::PthreadMutex* mutex);
int  KYTY_SYSV_ABI c_mtx_timedlock(LibKernel::PthreadMutex* mutex, const LibKernel::KernelTimespec* abstime);
int  KYTY_SYSV_ABI c_mtx_unlock(LibKernel::PthreadMutex* mutex);
int  KYTY_SYSV_ABI c_mtx_current_owns(LibKernel::PthreadMutex* mutex);

int  KYTY_SYSV_ABI c_execute_once(int* flag, execute_once_callback_t callback, void* context);
int  KYTY_SYSV_ABI c_vsnprintf(char* s, size_t n, const char* fmt, VaList* ap);
int  cxa_atexit(void (*func)(void*), void* arg, void* d);
void cxa_finalize(void* d);
int  KYTY_SYSV_ABI c_cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso_handle);

// Math (double) family — pure wrappers over the host libm, defined in
// LibCMath.cpp.
KYTY_SYSV_ABI double c_sin(double x);
KYTY_SYSV_ABI double c_cos(double x);
KYTY_SYSV_ABI double c_tan(double x);
KYTY_SYSV_ABI double c_asin(double x);
KYTY_SYSV_ABI double c_acos(double x);
KYTY_SYSV_ABI double c_atan(double x);
KYTY_SYSV_ABI double c_atan2(double y, double x);
KYTY_SYSV_ABI double c_exp(double x);
KYTY_SYSV_ABI double c_log(double x);
KYTY_SYSV_ABI double c_pow(double x, double y);
KYTY_SYSV_ABI double c_powidf2(double x, int y);
KYTY_SYSV_ABI double c_fmod(double x, double y);
KYTY_SYSV_ABI double c_ceil(double x);
KYTY_SYSV_ABI double c_floor(double x);
KYTY_SYSV_ABI double c_round(double x);
KYTY_SYSV_ABI double c_sqrt(double x);
KYTY_SYSV_ABI double c_fabs(double x);
KYTY_SYSV_ABI double c_modf(double x, double* ip);
KYTY_SYSV_ABI double c_ldexp(double x, int e);
KYTY_SYSV_ABI double c_frexp(double x, int* e);
KYTY_SYSV_ABI void c_sincos(double x, double* s, double* c);
KYTY_SYSV_ABI float c_powf(float x, float y);
KYTY_SYSV_ABI int c_isnanf(float x);
KYTY_SYSV_ABI int c_isfinite(double x);
KYTY_SYSV_ABI int c_isnan(double x);
KYTY_SYSV_ABI int c_isinf(double x);
KYTY_SYSV_ABI float c_sinf(float x);
KYTY_SYSV_ABI float c_cosf(float x);
KYTY_SYSV_ABI float c_tanf(float x);
KYTY_SYSV_ABI float c_atanf(float x);
KYTY_SYSV_ABI float c_asinf(float x);
KYTY_SYSV_ABI float c_acosf(float x);
KYTY_SYSV_ABI float c_atan2f(float y, float x);
KYTY_SYSV_ABI float c_fmodf(float x, float y);
KYTY_SYSV_ABI float c_hypotf(float x, float y);
KYTY_SYSV_ABI float c_truncf(float x);
KYTY_SYSV_ABI float c_roundf(float x);
KYTY_SYSV_ABI float c_log10f(float x);
KYTY_SYSV_ABI float c_logf(float x);
KYTY_SYSV_ABI float c_sqrtf(float x);
KYTY_SYSV_ABI float c_fabsf(float x);
KYTY_SYSV_ABI float c_floorf(float x);
KYTY_SYSV_ABI float c_ceilf(float x);
KYTY_SYSV_ABI float c_log2f(float x);
KYTY_SYSV_ABI float c_exp2f(float x);
KYTY_SYSV_ABI float c_expf(float x);
KYTY_SYSV_ABI float c_ldexpf(float x, int e);
KYTY_SYSV_ABI void c_sincosf(float x, float* s, float* c);

} // namespace LibC

namespace LibcInternalExt {

void InitLibcInternalExt_1(Loader::SymbolDatabase* s);

} // namespace LibcInternalExt

namespace LibcInternal {

void InitLibcInternal_1(Loader::SymbolDatabase* s);

int  KYTY_SYSV_ABI fflush(FILE* stream);
void* KYTY_SYSV_ABI LibcMspaceCreate(const char* name, void* base, size_t capacity, uint32_t flag);
void* KYTY_SYSV_ABI LibcMspaceMalloc(void* msp, size_t size);
void* KYTY_SYSV_ABI LibcMspaceMemalign(void* msp, size_t align, size_t size);
size_t KYTY_SYSV_ABI LibcMspaceMallocUsableSize(const void* ptr);
void* KYTY_SYSV_ABI LibcMspaceCalloc(void* msp, size_t nelem, size_t size);
int  KYTY_SYSV_ABI LibcMspaceMallocStatsFast(void* msp, void* stats);
int  KYTY_SYSV_ABI LibcMallocStatsFast(void* stats);
void KYTY_SYSV_ABI LibcMspaceFree(void* msp, void* ptr);

} // namespace LibcInternal

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_LIBS_LIBCINTERNAL_H_ */
