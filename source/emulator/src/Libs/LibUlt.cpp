#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Ult", 1, "Ult", 1, 1);

namespace Ult {

static KYTY_SYSV_ABI int UltInitialize()
{
	PRINT_NAME();
	return OK;
}

static KYTY_SYSV_ABI uint64_t UltUlthreadRuntimeGetWorkAreaSize(uint32_t max_ulthread, uint32_t max_worker_thread)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t max_ulthread       = %" PRIu32 "\n", max_ulthread);
	KYTY_LOG_DEBUG("\t max_worker_thread  = %" PRIu32 "\n", max_worker_thread);
	return 8;
}

static KYTY_SYSV_ABI int UltUlthreadRuntimeOptParamInitialize(void* opt_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t opt_param          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt_param));
	return OK;
}

static KYTY_SYSV_ABI int UltUlthreadRuntimeCreate(void* runtime, const char* name, int32_t max_ulthread,
	                                               int32_t max_worker_thread, void* work_area, void* opt_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t runtime             = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(runtime));
	KYTY_LOG_DEBUG("\t name                = %s\n", name != nullptr ? name : "<null>");
	KYTY_LOG_DEBUG("\t max_ulthread       = %" PRId32 "\n", max_ulthread);
	KYTY_LOG_DEBUG("\t max_worker_thread  = %" PRId32 "\n", max_worker_thread);
	KYTY_LOG_DEBUG("\t work_area           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(work_area));
	KYTY_LOG_DEBUG("\t opt_param           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt_param));
	return OK;
}

static KYTY_SYSV_ABI uint64_t UltWaitingQueueResourcePoolGetWorkAreaSize(uint32_t num_threads, uint32_t num_sync_objects)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t num_threads         = %" PRIu32 "\n", num_threads);
	KYTY_LOG_DEBUG("\t num_sync_objects    = %" PRIu32 "\n", num_sync_objects);
	return 8;
}

static KYTY_SYSV_ABI int UltWaitingQueueResourcePoolCreate(void* pool, const char* name, uint32_t num_threads,
	                                                         uint32_t num_sync_objects, void* work_area, void* opt_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t pool                = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(pool));
	KYTY_LOG_DEBUG("\t name                = %s\n", name != nullptr ? name : "<null>");
	KYTY_LOG_DEBUG("\t num_threads         = %" PRIu32 "\n", num_threads);
	KYTY_LOG_DEBUG("\t num_sync_objects    = %" PRIu32 "\n", num_sync_objects);
	KYTY_LOG_DEBUG("\t work_area           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(work_area));
	KYTY_LOG_DEBUG("\t opt_param           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt_param));
	return OK;
}

static KYTY_SYSV_ABI uint64_t UltQueueDataResourcePoolGetWorkAreaSize(uint32_t num_data, uint64_t data_size,
	                                                                    uint32_t num_queue_objects)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t num_data            = %" PRIu32 "\n", num_data);
	KYTY_LOG_DEBUG("\t data_size           = 0x%016" PRIx64 "\n", data_size);
	KYTY_LOG_DEBUG("\t num_queue_objects   = %" PRIu32 "\n", num_queue_objects);
	return static_cast<uint64_t>(num_data) * data_size * num_queue_objects;
}

static KYTY_SYSV_ABI int UltQueueDataResourcePoolCreate(void* pool, const char* name, uint32_t num_data,
	                                                       uint64_t data_size, uint32_t num_queue_objects,
	                                                       void* waiting_queue, void* work_area, void* opt_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t pool                = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(pool));
	KYTY_LOG_DEBUG("\t name                = %s\n", name != nullptr ? name : "<null>");
	KYTY_LOG_DEBUG("\t num_data            = %" PRIu32 "\n", num_data);
	KYTY_LOG_DEBUG("\t data_size           = 0x%016" PRIx64 "\n", data_size);
	KYTY_LOG_DEBUG("\t num_queue_objects   = %" PRIu32 "\n", num_queue_objects);
	KYTY_LOG_DEBUG("\t waiting_queue       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(waiting_queue));
	KYTY_LOG_DEBUG("\t work_area           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(work_area));
	KYTY_LOG_DEBUG("\t opt_param           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt_param));
	return OK;
}

static KYTY_SYSV_ABI int UltQueueCreate(void* queue, const char* name, uint64_t data_size, void* waiting_queue,
	                                     void* queue_data, void* opt_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t queue               = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(queue));
	KYTY_LOG_DEBUG("\t name                = %s\n", name != nullptr ? name : "<null>");
	KYTY_LOG_DEBUG("\t data_size           = 0x%016" PRIx64 "\n", data_size);
	KYTY_LOG_DEBUG("\t waiting_queue       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(waiting_queue));
	KYTY_LOG_DEBUG("\t queue_data          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(queue_data));
	KYTY_LOG_DEBUG("\t opt_param           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt_param));
	return OK;
}

static KYTY_SYSV_ABI int UltQueuePush(void* queue, const void* data)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t queue               = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(queue));
	KYTY_LOG_DEBUG("\t data                = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	return OK;
}

} // namespace Ult

LIB_DEFINE(InitUlt_1)
{
	LIB_FUNC("hZIg1EWGsHM", Ult::UltInitialize);
	LIB_FUNC("grs2pbc2awM", Ult::UltUlthreadRuntimeGetWorkAreaSize);
	LIB_FUNC("V2u3WLrwh64", Ult::UltUlthreadRuntimeOptParamInitialize);
	LIB_FUNC("jw9FkZBXo-g", Ult::UltUlthreadRuntimeCreate);
	LIB_FUNC("WIWV1Qd7PFU", Ult::UltWaitingQueueResourcePoolGetWorkAreaSize);
	LIB_FUNC("YiHujOG9vXY", Ult::UltWaitingQueueResourcePoolCreate);
	LIB_FUNC("evj9YPkS8s4", Ult::UltQueueDataResourcePoolGetWorkAreaSize);
	LIB_FUNC("TFHm6-N6vks", Ult::UltQueueDataResourcePoolCreate);
	LIB_FUNC("9Y5keOvb6ok", Ult::UltQueueCreate);
	LIB_FUNC("dUwpX3e5NDE", Ult::UltQueuePush);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
