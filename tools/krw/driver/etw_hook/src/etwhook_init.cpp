#include <etwhook_init.hpp>
#include <kstl/ksystem_info.hpp>
#include <etwhook_utils.hpp>
#include <kstl/kpe_parse.hpp>


#pragma warning(disable : 4201)

#define EtwpStartTrace		1
#define EtwpStopTrace		2
#define EtwpQueryTrace		3
#define EtwpUpdateTrace		4
#define EtwpFlushTrace		5

#define WNODE_FLAG_TRACED_GUID			0x00020000  // denotes a trace
#define EVENT_TRACE_BUFFERING_MODE      0x00000400  // Buffering mode only
#define EVENT_TRACE_FLAG_SYSTEMCALL     0x00000080  // system calls

typedef struct _WNODE_HEADER
{
	ULONG BufferSize;        // Size of entire buffer inclusive of this ULONG
	ULONG ProviderId;    // Provider Id of driver returning this buffer
	union
	{
		ULONG64 HistoricalContext;  // Logger use
		struct
		{
			ULONG Version;           // Reserved
			ULONG Linkage;           // Linkage field reserved for WMI
		} DUMMYSTRUCTNAME;
	} DUMMYUNIONNAME;

	union
	{
		ULONG CountLost;         // Reserved
		HANDLE KernelHandle;     // Kernel handle for data block
		LARGE_INTEGER TimeStamp; // Timestamp as returned in units of 100ns
								 // since 1/1/1601
	} DUMMYUNIONNAME2;
	GUID Guid;                  // Guid for data block returned with results
	ULONG ClientContext;
	ULONG Flags;             // Flags, see below
} WNODE_HEADER, * PWNODE_HEADER;

typedef struct _EVENT_TRACE_PROPERTIES {
	WNODE_HEADER	Wnode;
	ULONG			BufferSize;
	ULONG			MinimumBuffers;
	ULONG			MaximumBuffers;
	ULONG			MaximumFileSize;
	ULONG			LogFileMode;
	ULONG			FlushTimer;
	ULONG			EnableFlags;
	LONG			AgeLimit;
	ULONG			NumberOfBuffers;
	ULONG			FreeBuffers;
	ULONG			EventsLost;
	ULONG			BuffersWritten;
	ULONG			LogBuffersLost;
	ULONG			RealTimeBuffersLost;
	HANDLE			LoggerThreadId;
	ULONG			LogFileNameOffset;
	ULONG			LoggerNameOffset;
} EVENT_TRACE_PROPERTIES, * PEVENT_TRACE_PROPERTIES;

/* 54dea73a-ed1f-42a4-af713e63d056f174 */
const GUID CkclSessionGuid = { 0x54dea73a, 0xed1f, 0x42a4, { 0xaf, 0x71, 0x3e, 0x63, 0xd0, 0x56, 0xf1, 0x74 } };

/*这个是Nt kernel Logger 的 guid 其实设置谁都一样！*/
const GUID session_guid = { 0x9E814AAD, 0x3204, 0x11D2, { 0x9A, 0x82, 0x0, 0x60, 0x8, 0xA8, 0x69, 0x39 } };

typedef struct _CKCL_TRACE_PROPERIES : EVENT_TRACE_PROPERTIES
{
	ULONG64					Unknown[3];
	UNICODE_STRING			ProviderName;
} CKCL_TRACE_PROPERTIES, * PCKCL_TRACE_PROPERTIES;

EXTERN_C
NTSYSCALLAPI
NTSTATUS
NTAPI
ZwTraceControl(
	_In_ ULONG FunctionCode,
	_In_reads_bytes_opt_(InBufferLen) PVOID InBuffer,
	_In_ ULONG InBufferLen,
	_Out_writes_bytes_opt_(OutBufferLen) PVOID OutBuffer,
	_In_ ULONG OutBufferLen,
	_Out_ PULONG ReturnLength
);

EXTERN_C
NTSYSCALLAPI
NTSTATUS
NTAPI
ZwSetSystemInformation(ULONG info_class,void* buf,ULONG length);

typedef enum _EVENT_TRACE_INFORMATION_CLASS {
	EventTraceKernelVersionInformation,
	EventTraceGroupMaskInformation,
	EventTracePerformanceInformation,
	EventTraceTimeProfileInformation,
	EventTraceSessionSecurityInformation,
	EventTraceSpinlockInformation,
	EventTraceStackTracingInformation,
	EventTraceExecutiveResourceInformation,
	EventTraceHeapTracingInformation,
	EventTraceHeapSummaryTracingInformation,
	EventTracePoolTagFilterInformation,
	EventTracePebsTracingInformation,
	EventTraceProfileConfigInformation,
	EventTraceProfileSourceListInformation,
	EventTraceProfileEventListInformation,
	EventTraceProfileCounterListInformation,
	EventTraceStackCachingInformation,
	EventTraceObjectTypeFilterInformation,
	MaxEventTraceInfoClass
} EVENT_TRACE_INFORMATION_CLASS;

typedef struct _EVENT_TRACE_PROFILE_COUNTER_INFORMATION
{
	EVENT_TRACE_INFORMATION_CLASS EventTraceInformationClass;
	HANDLE TraceHandle;
	ULONG ProfileSource[1];
} EVENT_TRACE_PROFILE_COUNTER_INFORMATION, * PEVENT_TRACE_PROFILE_COUNTER_INFORMATION;

typedef struct _EVENT_TRACE_SYSTEM_EVENT_INFORMATION
{
	EVENT_TRACE_INFORMATION_CLASS EventTraceInformationClass;
	HANDLE TraceHandle;
	ULONG HookId[1];
} EVENT_TRACE_SYSTEM_EVENT_INFORMATION, * PEVENT_TRACE_SYSTEM_EVENT_INFORMATION;

const ULONG SystemPerformanceTraceInformation = 31;



EtwInitilizer::EtwInitilizer() : __is_open(false), __logger_id(0)
{
	VM_BEGIN();

	UNICODE_STRING func_name = {};

	// ntoskrnl exports HalPrivateDispatchTable as the table base (not a pointer-to-pointer).
	// MmGetSystemRoutineAddress returns &HalPrivateDispatchTable[0]. HalCollectPmcCounters = [73].
	RtlInitUnicodeString(&func_name, L"HalPrivateDispatchTable");
	HalPrivateDispatchTable = reinterpret_cast<UINT_PTR*>(MmGetSystemRoutineAddress(&func_name));
	if (HalPrivateDispatchTable == nullptr) {
		LOG_ERROR("failed to get HalPrivateDispatchTable");
	} else {
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] HalPrivateDispatchTable=%p\n", HalPrivateDispatchTable);
	}
	VM_END();
}


EtwInitilizer::~EtwInitilizer()
{
	/*如果开启着，那么需要关闭*/
	if (__is_open) end_syscall_trace();
}

NTSTATUS EtwInitilizer::start_syscall_trace()
{
	VM_BEGIN();
	auto status = STATUS_UNSUCCESSFUL;
	auto ckcl_property = (PCKCL_TRACE_PROPERTIES)(nullptr);
	auto returnlen = 0ul;

	do {

		ckcl_property = kalloc<CKCL_TRACE_PROPERTIES>(NonPagedPool,PAGE_SIZE);
		if (!ckcl_property) {
			LOG_ERROR("failed to alloc memory for etw property!\r\n");
			status = STATUS_MEMORY_NOT_ALLOCATED;
			break;
		}

		memset(ckcl_property, 0, PAGE_SIZE);
		ckcl_property->Wnode.BufferSize = PAGE_SIZE;
		ckcl_property->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
		//ckcl_property->ProviderName = RTL_CONSTANT_STRING(L"NT Kernel Logger");
		//ckcl_property->Wnode.Guid = session_guid;
		ckcl_property->ProviderName = RTL_CONSTANT_STRING(L"Circular Kernel Context Logger");
		ckcl_property->Wnode.Guid = CkclSessionGuid;
		ckcl_property->Wnode.ClientContext = 1;
		ckcl_property->BufferSize = sizeof(ULONG);
		ckcl_property->MinimumBuffers = ckcl_property->MaximumBuffers = 2;
		ckcl_property->LogFileMode = EVENT_TRACE_BUFFERING_MODE;
		
		//enable kernel logger etw trace
		status = ZwTraceControl(EtwpStartTrace, ckcl_property, PAGE_SIZE, ckcl_property, PAGE_SIZE, &returnlen);
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] EtwpStartTrace status=%08X\n", status);

		//sometimes may return value is STATUS_OBJECT_NAME_COLLISION
		if (!NT_SUCCESS(status) && status!= STATUS_OBJECT_NAME_COLLISION) {
			LOG_ERROR("failed to enable kernel loggger etw trace,errcode=%x\r\n", status);
			break;
		}
		if (status == STATUS_OBJECT_NAME_COLLISION) {
			status = ZwTraceControl(EtwpQueryTrace, ckcl_property, PAGE_SIZE, ckcl_property, PAGE_SIZE,
			                        &returnlen);
			DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			           "[XCatKrw] EtwpQueryTrace status=%08X hist=%llx\n", status,
			           (unsigned long long)ckcl_property->Wnode.HistoricalContext);
			if (!NT_SUCCESS(status)) {
				LOG_ERROR("failed to query ckcl etw,errcode=%x\r\n", status);
				break;
			}
		}

		//start syscall etw
		ckcl_property->EnableFlags = EVENT_TRACE_FLAG_SYSTEMCALL;

		status=  ZwTraceControl(EtwpUpdateTrace, ckcl_property, PAGE_SIZE, ckcl_property, PAGE_SIZE, &returnlen);
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] EtwpUpdateTrace(SYSCALL) status=%08X hist=%llx\n", status,
		           (unsigned long long)ckcl_property->Wnode.HistoricalContext);
		// Already-enabled is fine (reload after soft-fail left CKCL up).
		if (!NT_SUCCESS(status) && status != STATUS_WMI_ALREADY_ENABLED) {
			LOG_ERROR("failed to enable syscall etw,errcode=%x\r\n", status);
			end_syscall_trace();
			break;
		}
		status = STATUS_SUCCESS;

		// Prefer logger id from WNODE after Start/Update (avoids fragile EtwpDebuggerData walk).
		__logger_id = static_cast<ULONG>(ckcl_property->Wnode.HistoricalContext & 0xFFFFull);
		if (__logger_id == 0) {
			__logger_id = static_cast<ULONG>(ckcl_property->Wnode.HistoricalContext);
		}
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] CKCL logger_id=%lu\n", __logger_id);

	} while (false);

	//clean up

	if (ckcl_property) ExFreePool(ckcl_property);
	
	//if fail
	//to do
	
	//if success clean
	if (NT_SUCCESS(status)) __is_open = true;
	
	return status;
	VM_END();
}

NTSTATUS EtwInitilizer::end_syscall_trace()
{
	VM_BEGIN();
	auto status = STATUS_UNSUCCESSFUL;
	auto ckcl_property = (PCKCL_TRACE_PROPERTIES)(nullptr);
	auto returnlen = 0ul;

	do {

		ckcl_property = kalloc<CKCL_TRACE_PROPERTIES>(NonPagedPool, PAGE_SIZE);
		if (!ckcl_property) {
			LOG_ERROR("failed to alloc memory for etw property!\r\n");
			status = STATUS_MEMORY_NOT_ALLOCATED;
			break;
		}

		

		memset(ckcl_property, 0, PAGE_SIZE);
		ckcl_property->Wnode.BufferSize = PAGE_SIZE;
		ckcl_property->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
		//ckcl_property->ProviderName = RTL_CONSTANT_STRING(L"NT Kernel Logger");
		//ckcl_property->Wnode.Guid = session_guid;
		ckcl_property->ProviderName = RTL_CONSTANT_STRING(L"Circular Kernel Context Logger");
		ckcl_property->Wnode.Guid = CkclSessionGuid;
		ckcl_property->Wnode.ClientContext = 1;
		ckcl_property->BufferSize = sizeof(ULONG);
		ckcl_property->MinimumBuffers = ckcl_property->MaximumBuffers = 2;
		ckcl_property->LogFileMode = EVENT_TRACE_BUFFERING_MODE;

		//enable kernel logger etw trace
		status = ZwTraceControl(EtwpStopTrace, ckcl_property, PAGE_SIZE, ckcl_property, PAGE_SIZE, &returnlen);

		if (!NT_SUCCESS(status)) {
			LOG_ERROR("failed to stop kernel loggger etw trace,errcode=%x\r\n", status);
			break;
		}


	} while (false);

	//clean up
	if (ckcl_property) ExFreePool(ckcl_property);
	
	if (NT_SUCCESS(status)) __is_open = false;

	return status;
	VM_END();
}

/*其实这个要调用ZwSetSystemInfomation，但是没有找到合适的文档化和文章，故只能手动逆向windows，最终得出结果*/
NTSTATUS EtwInitilizer::open_pmc_counter()
{
	VM_BEGIN();
	auto status = STATUS_SUCCESS;
	auto pmc_count_info = (PEVENT_TRACE_PROFILE_COUNTER_INFORMATION)(nullptr);
	auto pmc_event_info=(PEVENT_TRACE_SYSTEM_EVENT_INFORMATION)(nullptr);
	constexpr auto syscall_hookid = 0xf33ul;



	if (!__is_open) return STATUS_FLT_NOT_INITIALIZED;

	do {

		ULONG logger_id = __logger_id;

		/*Fallback: KdDebuggerData64.EtwpDebuggerData walk (legacy InfinityHook).*/
		if (logger_id == 0) {
			auto EtwpDebuggerData = reinterpret_cast<ULONG***>(
			    kstd::SysInfoManager::getInstance()->getSysInfo()->EtwpDebuggerData);

			if (!EtwpDebuggerData || !MmIsAddressValid(EtwpDebuggerData)) {
				status = STATUS_NOT_SUPPORTED;
				LOG_ERROR("failed to get EtwpDebuggerData!\r\n");
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
				           "[XCatKrw] EtwpDebuggerData invalid %p\n", EtwpDebuggerData);
				break;
			}

			logger_id = EtwpDebuggerData[2][2][0];
			DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			           "[XCatKrw] EtwpDebuggerData=%p logger_id=%lu\n", EtwpDebuggerData, logger_id);
		}

		if (logger_id == 0) {
			status = STATUS_NOT_SUPPORTED;
			DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[XCatKrw] logger_id is 0\n");
			break;
		}

		pmc_count_info = kalloc<EVENT_TRACE_PROFILE_COUNTER_INFORMATION>(NonPagedPool);
		if (!pmc_count_info) {
			LOG_ERROR("failed to alloc memory for pmc_count!\r\n");
			status = STATUS_MEMORY_NOT_ALLOCATED;
			break;
		}
		//先设置PMC Count 我们只关心一个hookid 那就是syscall的hookid 0xf33 profile source 随便设置
		pmc_count_info->EventTraceInformationClass = EventTraceProfileCounterListInformation;
		pmc_count_info->TraceHandle = ULongToHandle(logger_id)/*这个其实就是loggerid*/;
		pmc_count_info->ProfileSource[0] = 1;/*随便填写*/

		auto EtwpMaxPmcCounter=get_EtwpMaxPmcCounter();

		auto org = (unsigned char)0;

		if (MmIsAddressValid(EtwpMaxPmcCounter)) {

			org = *EtwpMaxPmcCounter;

			if (org <= 1) *EtwpMaxPmcCounter = 2;

		}

		status=ZwSetSystemInformation(SystemPerformanceTraceInformation, pmc_count_info, sizeof EVENT_TRACE_PROFILE_COUNTER_INFORMATION);
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] PMC counter list status=%08X\n", status);
		if (!NT_SUCCESS(status) && status != STATUS_WMI_ALREADY_ENABLED) {
			LOG_ERROR("failed to configure pmc counter,errcode=%x\r\n", status);
			break;
		}
		status = STATUS_SUCCESS;


		if (MmIsAddressValid(EtwpMaxPmcCounter)) {
			if (org <= 1) *EtwpMaxPmcCounter = org;
		}

		//然后设置PMC Event hookid只需要一个就行
		pmc_event_info = kalloc<EVENT_TRACE_SYSTEM_EVENT_INFORMATION>(NonPagedPool);
		if (!pmc_event_info) {
			LOG_ERROR("failed to alloc memory for pmc_event_info!\r\n");
			status = STATUS_MEMORY_NOT_ALLOCATED;
			break;
		}

		pmc_event_info->EventTraceInformationClass = EventTraceProfileEventListInformation;
		pmc_event_info->TraceHandle = ULongToHandle(logger_id);
		pmc_event_info->HookId[0] = syscall_hookid;/*必须0xf33*/


		status = ZwSetSystemInformation(SystemPerformanceTraceInformation, pmc_event_info, sizeof EVENT_TRACE_SYSTEM_EVENT_INFORMATION);
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		           "[XCatKrw] PMC event list status=%08X\n", status);
		if (!NT_SUCCESS(status) && status != STATUS_WMI_ALREADY_ENABLED) {
			LOG_ERROR("failed to configure pmc event,errcode=%x\r\n", status);
			break;
		}
		status = STATUS_SUCCESS;

		

	} while (false);

	//clean up
	if (pmc_count_info) ExFreePool(pmc_count_info);
	if (pmc_event_info) ExFreePool(pmc_event_info);

	return status;
	VM_END();
}

unsigned char* EtwInitilizer::get_EtwpMaxPmcCounter()
{
	VM_BEGIN();
	auto ret = nullptr;
	auto nt_img = (void*)nullptr;
	auto nt_size = 0ul;

	//PAGE:00000001409DB8DE 44 3B 05 57 57 37 00                          cmp     r8d, cs:EtwpMaxPmcCounter
	//PAGE : 00000001409DB8E5 0F 87 EC 00 00 00                           ja      loc_1409DB9D7
	//PAGE : 00000001409DB8EB 83 B9 2C 01 00 00 01                        cmp     dword ptr[rcx + 12Ch], 1
	//PAGE:00000001409DB8F2 0F 84 DF 00 00 00                             jz      loc_1409DB9D7
	//PAGE : 00000001409DB8F8 48 83 B9 F8 03 00 00 00                     cmp     qword ptr[rcx + 3F8h], 0
	//PAGE:00000001409DB900 75 0D                                         jnz     short loc_1409DB90F

	//windows 18362开始有的
	if (kstd::SysInfoManager::getInstance()->getBuildNumber() < 18362) return ret;

	nt_img = find_module_base(L"ntoskrnl.exe", &nt_size);

	kstd::ParsePE ntos(nt_img, nt_size);

	auto p = reinterpret_cast<unsigned char*>(ntos.patternFindSections((unsigned long long)nt_img, \
		"\x44\x3b\x05\x00\x00\x00\x00\x0f\x87\x00\x00\x00\x00\x83\xb9\x00\x00\x00\x00\x01\x0f\x84\x00\x00\x00\x00\x48\x83\xb9\x00\x00\x00\x00\x00\x75\x00", \
		"xxx????xx????xx????xxx????xxx????xx?", "PAGE"));


	if (MmIsAddressValid(p)) {
		auto offset = *reinterpret_cast<PLONG>(p + 3);
		return p + 7 + offset;
	}
	else return nullptr;
	

	VM_END();
}

#pragma warning(default : 4201)