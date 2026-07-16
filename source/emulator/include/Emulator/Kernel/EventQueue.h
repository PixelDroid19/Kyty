#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::EventQueue {

constexpr int16_t KERNEL_EVFILT_TIMER     = -7;
constexpr int16_t KERNEL_EVFILT_READ      = -1;
constexpr int16_t KERNEL_EVFILT_WRITE     = -2;
constexpr int16_t KERNEL_EVFILT_USER      = -11;
constexpr int16_t KERNEL_EVFILT_FILE      = -4;
constexpr int16_t KERNEL_EVFILT_GRAPHICS  = -14;
constexpr int16_t KERNEL_EVFILT_VIDEO_OUT = -13;
constexpr int16_t KERNEL_EVFILT_HRTIMER   = -15;
// PS5 Ampr completion filter. Public headers stop at GPU_DBGGC_EV (-22); Ampr is
// a later Gen5 filter. Value used for Add/Trigger matching; confirm if a guest
// ever asserts KernelEvent.filter for Ampr completions.
constexpr int16_t KERNEL_EVFILT_AMPR = -23;

class KernelEqueuePrivate;
struct KernelEqueueEvent;

using KernelEqueue = KernelEqueuePrivate*;

using trigger_func_t = void (*)(KernelEqueueEvent* event, void* trigger_data);
using reset_func_t   = void (*)(KernelEqueueEvent* event);
using delete_func_t  = void (*)(KernelEqueue eq, KernelEqueueEvent* event);

struct KernelEvent
{
	uintptr_t ident  = 0;
	int16_t   filter = 0;
	uint16_t  flags  = 0;
	uint32_t  fflags = 0;
	intptr_t  data   = 0;
	void*     udata  = nullptr;
};

struct KernelFilter
{
	void*          data              = nullptr;
	trigger_func_t trigger_func      = nullptr;
	reset_func_t   reset_func        = nullptr;
	delete_func_t  delete_event_func = nullptr;
};

struct KernelEqueueEvent
{
	bool         triggered = false;
	KernelEvent  event;
	KernelFilter filter;
};

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event);
int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter, void* trigger_data);
int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter);

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name);
int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq);
int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo);

// sceKernelAddAmprEvent — NID bBfz7kMF2Ho.
// Observed SysV ABI: (eq, 0, 0, ident, udata).
int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, uint64_t reserved0, uint64_t reserved1, uintptr_t ident, void* udata);
// sceKernelDeleteAmprEvent — NID bMmid3pfyjo.
int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, uintptr_t ident);

intptr_t KYTY_SYSV_ABI  KernelGetEventData(const KernelEvent* ev);
intptr_t KYTY_SYSV_ABI  KernelGetEventFflags(const KernelEvent* ev);
int KYTY_SYSV_ABI       KernelGetEventFilter(const KernelEvent* ev);
uintptr_t KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev);
void* KYTY_SYSV_ABI     KernelGetEventUserData(const KernelEvent* ev);
int KYTY_SYSV_ABI       KernelGetEventError(const KernelEvent* ev);

} // namespace Kyty::Libs::LibKernel::EventQueue

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_ */
