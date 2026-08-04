#ifndef EMULATOR_SRC_LIBS_LIBCINTERNAL_H_
#define EMULATOR_SRC_LIBS_LIBCINTERNAL_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/MSpace.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Libs.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

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

// Host/application-heap allocation shims. Their ownership ledger lives in
// LibCAlloc.cpp so allocation policy is isolated from the ABI registration
// unit and can be validated independently.
void* KYTY_SYSV_ABI c_malloc(size_t size);
char* KYTY_SYSV_ABI c_strdup(const char* source);
void* KYTY_SYSV_ABI c_calloc(size_t count, size_t size);
void* KYTY_SYSV_ABI c_memalign(size_t alignment, size_t size);
void* KYTY_SYSV_ABI c_realloc(void* ptr, size_t size);
void  KYTY_SYSV_ABI c_free(void* ptr);
void* KYTY_SYSV_ABI c_aligned_alloc(size_t alignment, size_t size);
int   KYTY_SYSV_ABI c_posix_memalign(void** memptr, size_t alignment, size_t size);

// Shared by the C++ new/delete adapters that remain in LibC.cpp.
void* allocate_with_owner(size_t size);
bool  free_by_owner(void* ptr);

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

// stdio guest shims — host FILE* remains opaque to the guest and mounted paths
// are translated by the isolated implementation in LibCStdIo.cpp.
FILE*  KYTY_SYSV_ABI c_fopen(const char* path, const char* mode);
int    KYTY_SYSV_ABI c_fclose(FILE* stream);
size_t KYTY_SYSV_ABI c_fread(void* ptr, size_t size, size_t count, FILE* stream);
char*  KYTY_SYSV_ABI c_fgets(char* buffer, int size, FILE* stream);
size_t KYTY_SYSV_ABI c_fwrite(const void* ptr, size_t size, size_t count, FILE* stream);
int    KYTY_SYSV_ABI c_setvbuf(FILE* stream, char* buffer, int mode, size_t size);
int    KYTY_SYSV_ABI c_fseek(FILE* stream, long offset, int origin);
long   KYTY_SYSV_ABI c_ftell(FILE* stream);
int    KYTY_SYSV_ABI c_feof(FILE* stream);
int    KYTY_SYSV_ABI c_ferror(FILE* stream);
int    KYTY_SYSV_ABI c_fileno(FILE* stream);
int    KYTY_SYSV_ABI c_fputc(int character, FILE* stream);
int    KYTY_SYSV_ABI c_remove(const char* path);

// Memory, string and guest UTF-16 shims. Their implementations live in
// LibCString.cpp so the main libc registration unit does not own the entire
// byte/string ABI surface.
void*              KYTY_SYSV_ABI c_memcpy(void* dst, const void* src, size_t size);
int                KYTY_SYSV_ABI c_memcpy_s(void* dst, size_t dst_size, const void* src, size_t size);
int                KYTY_SYSV_ABI c_memmove_s(void* dst, size_t dst_size, const void* src, size_t size);
void*              KYTY_SYSV_ABI c_memmove(void* dst, const void* src, size_t size);
void*              KYTY_SYSV_ABI c_memset(void* dst, int value, size_t size);
int                KYTY_SYSV_ABI c_memset_s(void* dst, size_t dst_size, int value, size_t size);
int                KYTY_SYSV_ABI c_memcmp(const void* lhs, const void* rhs, size_t size);
void*              KYTY_SYSV_ABI c_memchr(const void* data, int value, size_t size);
size_t             KYTY_SYSV_ABI c_strlen(const char* value);
size_t             KYTY_SYSV_ABI c_wcslen(const uint16_t* value);
uint16_t*          KYTY_SYSV_ABI c_wcsncpy(uint16_t* dst, const uint16_t* src, size_t count);
int                KYTY_SYSV_ABI c_Iswctype(uint32_t character, int character_class);
int                KYTY_SYSV_ABI c_Wctombx(char* dst, uint32_t character, std::mbstate_t* state, const void* cvtvec);
int                KYTY_SYSV_ABI c_Mbtowcx(uint16_t* dst, const char* src, size_t count, std::mbstate_t* state, const void* cvtvec);
char*              KYTY_SYSV_ABI c_strcpy(char* dst, const char* src);
wchar_t*           KYTY_SYSV_ABI c_wmemchr(const wchar_t* src, wchar_t value, size_t count);
int                KYTY_SYSV_ABI c_wmemcmp(const wchar_t* lhs, const wchar_t* rhs, size_t count);
int                KYTY_SYSV_ABI c_wmemcmp16(const char16_t* lhs, const char16_t* rhs, size_t count);
char16_t*          KYTY_SYSV_ABI c_wmemcpy16(char16_t* dst, const char16_t* src, size_t count);
wchar_t*           KYTY_SYSV_ABI c_wmemcpy(wchar_t* dst, const wchar_t* src, size_t count);
wchar_t*           KYTY_SYSV_ABI c_wmemmove(wchar_t* dst, const wchar_t* src, size_t count);
wchar_t*           KYTY_SYSV_ABI c_wmemset(wchar_t* dst, wchar_t value, size_t count);
int                KYTY_SYSV_ABI c_strcpy_s(char* dst, size_t dst_size, const char* src);
char*              KYTY_SYSV_ABI c_strncpy(char* dst, const char* src, size_t count);
int                KYTY_SYSV_ABI c_strcmp(const char* lhs, const char* rhs);
int                KYTY_SYSV_ABI c_strncmp(const char* lhs, const char* rhs, size_t count);
int                KYTY_SYSV_ABI c_strcasecmp(const char* lhs, const char* rhs);
int                KYTY_SYSV_ABI c_strncasecmp(const char* lhs, const char* rhs, size_t count);
char*              KYTY_SYSV_ABI c_strcat(char* dst, const char* src);
char*              KYTY_SYSV_ABI c_strncat(char* dst, const char* src, size_t count);
char*              KYTY_SYSV_ABI c_strpbrk(const char* value, const char* accept);
char*              KYTY_SYSV_ABI c_strchr(const char* value, int character);
char*              KYTY_SYSV_ABI c_strrchr(const char* value, int character);
char*              KYTY_SYSV_ABI c_strstr(const char* value, const char* needle);
char*              KYTY_SYSV_ABI c_getenv(const char* name);
char*              KYTY_SYSV_ABI c_setlocale(int category, const char* locale);
unsigned __int128  KYTY_SYSV_ABI c_udivti3(unsigned __int128 numerator, unsigned __int128 denominator);
uint16_t*          KYTY_SYSV_ABI c_wcsstr(const uint16_t* value, const uint16_t* needle);
int                KYTY_SYSV_ABI c_wcsncmp(const uint16_t* lhs, const uint16_t* rhs, size_t count);
size_t             KYTY_SYSV_ABI c_strnlen(const char* value, size_t count);

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
