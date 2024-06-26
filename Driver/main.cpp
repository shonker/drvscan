#pragma warning (disable: 4201)
#pragma warning (disable: 4996)

#include <ntifs.h>
#include <intrin.h>
#include "ia32.hpp"

#define POOLTAG (DWORD)'ACEC'
#define LOG(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[driver.sys] " __VA_ARGS__)
typedef int BOOL;
typedef unsigned short WORD;
typedef ULONG_PTR QWORD;
typedef ULONG DWORD;
typedef unsigned char BYTE;

#define IOCTL_READMEMORY 0xECAC00
#define IOCTL_IO_READ 0xECAC02
#define IOCTL_IO_WRITE 0xECAC12
#define IOCTL_REQUEST_MMAP 0xECAC04
#define IOCTL_REQUEST_PAGES 0xECAC06
#define IOCTL_READMEMORY_PROCESS 0xECAC08
#define IOCTL_GET_PHYSICAL 0xECAC10
#define IOCTL_GET_PCITSC 0xECAC14

#define OVERHEAD_MEASURE_LOOPS	1000000

static DWORD get_tsc_overhead(void);

#pragma pack(push, 1)
typedef struct {
	PVOID address;
	PVOID buffer;
	ULONG_PTR length;
} DRIVER_READMEMORY;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
	PVOID buffer;
	QWORD buffer_size;
} DRIVER_REQUEST_MAP;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
	PVOID src;
	PVOID dst;
	ULONG_PTR length;
	ULONG pid;
} DRIVER_READMEMORY_PROCESS;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
	PVOID InOutPhysical;
} DRIVER_GET_PHYSICAL;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
	BYTE     bus, slot, func, offset;
	DWORD    loops;
	QWORD    *tsc;
	DWORD    *tsc_overhead;
} DRIVER_PCITSC ;
#pragma pack(pop)

typedef struct {
	PVOID page_address_buffer;
	PVOID page_count_buffer;
	DWORD *total_count;
} EFI_MEMORY_PAGES ;

extern "C" NTSTATUS NTAPI MmCopyVirtualMemory
(
	PEPROCESS SourceProcess,
	PVOID SourceAddress,
	PEPROCESS TargetProcess,
	PVOID TargetAddress,
	SIZE_T BufferSize,
	KPROCESSOR_MODE PreviousMode,
	PSIZE_T ReturnSize
);


BOOL get_efi_memory_pages(EFI_MEMORY_PAGES *info);

//
// global variables
//
PVOID           gEfiMemoryMap;
DWORD           gEfiMemoryMapSize;

QWORD           efi_pages[100];
QWORD           efi_num_pages[100];
DWORD           efi_page_count;

PDEVICE_OBJECT  gDeviceObject;
UNICODE_STRING  gDeviceName;
UNICODE_STRING  gDosDeviceName;

DWORD           tsc_overhead;

WORD read_pci_u16(BYTE bus, BYTE slot, BYTE func, BYTE offset)
{
	__outdword(0xCF8, 0x80000000 | bus << 16 | slot << 11 | func <<  8 | offset);
	return (__indword(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF;
}

void get_pci_latency(DWORD loops, BYTE bus, BYTE slot, BYTE func, BYTE offset , QWORD *tsc);

NTSTATUS dummy_io(PDEVICE_OBJECT DriverObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(DriverObject);
	irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return irp->IoStatus.Status;
}

NTSTATUS IoControl(PDEVICE_OBJECT DriverObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(DriverObject);

	//
	// irp->IoStatus.Information : WE NEED CHANGE THIS DEPENDING ON CONTEXT
	//

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
	VOID *buffer = (VOID*)irp->AssociatedIrp.SystemBuffer;

	if (!stack)
		goto E0;

	ULONG ioctl_code = stack->Parameters.DeviceIoControl.IoControlCode;

	if (ioctl_code == IOCTL_READMEMORY)
	{
		DRIVER_READMEMORY *mem = (DRIVER_READMEMORY*)buffer;



		PVOID virtual_buffer = ExAllocatePoolWithTag(NonPagedPoolNx, mem->length, POOLTAG);


		__try {
			// MM_COPY_ADDRESS virtual_address;
			// virtual_address.VirtualAddress = mem->address;
			// irp->IoStatus.Status = MmCopyMemory( virtual_buffer, virtual_address, mem->length, MM_COPY_MEMORY_VIRTUAL, &mem->length);


			memcpy( virtual_buffer, mem->address, mem->length );
			irp->IoStatus.Status = STATUS_SUCCESS;

		} __except (1)
		{
			//
			// do nothing
			//
			irp->IoStatus.Status = STATUS_INVALID_ADDRESS;
		}

		if (irp->IoStatus.Status == STATUS_SUCCESS)
		{
			memcpy(mem->buffer, virtual_buffer, mem->length);
		}

		ExFreePoolWithTag(virtual_buffer, POOLTAG);

		irp->IoStatus.Information = sizeof(DRIVER_READMEMORY);
	}

	else if (ioctl_code == IOCTL_IO_READ)
	{
		DRIVER_READMEMORY *mem = (DRIVER_READMEMORY*)buffer;
		PVOID io = MmMapIoSpace(*(PHYSICAL_ADDRESS*)&mem->address, mem->length, MmNonCached);
		if (io)
		{
			memcpy(mem->buffer, io, mem->length);
			MmUnmapIoSpace(io, mem->length);
			irp->IoStatus.Status = STATUS_SUCCESS;
		} else {
			irp->IoStatus.Status = STATUS_INVALID_ADDRESS;
		}
		irp->IoStatus.Information = sizeof(DRIVER_READMEMORY);
	}

	else if (ioctl_code == IOCTL_IO_WRITE)
	{
		DRIVER_READMEMORY *mem = (DRIVER_READMEMORY*)buffer;
		PVOID io = MmMapIoSpace(*(PHYSICAL_ADDRESS*)&mem->address, mem->length, MmNonCached);
		if (io)
		{
			memcpy(io, mem->buffer, mem->length);
			MmUnmapIoSpace(io, mem->length);
			irp->IoStatus.Status = STATUS_SUCCESS;
		} else {
			irp->IoStatus.Status = STATUS_INVALID_ADDRESS;
		}
		irp->IoStatus.Information = sizeof(DRIVER_READMEMORY);
	}

	else if (ioctl_code == IOCTL_REQUEST_MMAP)
	{
		DRIVER_REQUEST_MAP *mem = (DRIVER_REQUEST_MAP*)buffer;
		PVOID address=0;
		QWORD size = gEfiMemoryMapSize;

		irp->IoStatus.Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &address, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (NT_SUCCESS(irp->IoStatus.Status))
		{
			memcpy(address, gEfiMemoryMap, gEfiMemoryMapSize);
			*(PVOID*)mem->buffer      = address;
			*(QWORD*)mem->buffer_size = gEfiMemoryMapSize;
		}
		else
		{
			*(PVOID*)mem->buffer      = 0;
			*(QWORD*)mem->buffer_size = 0;
		}
		irp->IoStatus.Information = sizeof(DRIVER_REQUEST_MAP);
	}

	else if (ioctl_code == IOCTL_REQUEST_PAGES)
	{
		DRIVER_REQUEST_MAP *mem = (DRIVER_REQUEST_MAP*)buffer;
		PVOID address = 0;
		QWORD size    = (efi_page_count * 16) + 4;
		if (size)
		{
			irp->IoStatus.Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &address, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (NT_SUCCESS(irp->IoStatus.Status))
			{
				*(DWORD*)((QWORD)address + 0x00) = efi_page_count;
				for (DWORD i = 0; i < efi_page_count; i++)
				{
					QWORD temp = (QWORD)((QWORD)address + 4 + (i * 16));
					*(QWORD*)((QWORD)temp + 0x00) = efi_pages[i];
					*(QWORD*)((QWORD)temp + 0x08) = efi_num_pages[i];
				}
				*(PVOID*)mem->buffer      = address;
				*(QWORD*)mem->buffer_size = size;
			}
			else
			{
				*(PVOID*)mem->buffer      = 0;
				*(QWORD*)mem->buffer_size = 0;
			}
		}
		else
		{
			irp->IoStatus.Status = STATUS_INVALID_ADDRESS;
		}
		irp->IoStatus.Information = sizeof(DRIVER_REQUEST_MAP);
	}

	else if (ioctl_code == IOCTL_READMEMORY_PROCESS)
	{
		DRIVER_READMEMORY_PROCESS *mem = (DRIVER_READMEMORY_PROCESS*)buffer;

		__try {
			PEPROCESS eprocess;
			irp->IoStatus.Status = PsLookupProcessByProcessId((HANDLE)mem->pid, &eprocess);
			if (irp->IoStatus.Status == 0)
			{
				irp->IoStatus.Status = MmCopyVirtualMemory( eprocess, mem->src, PsGetCurrentProcess(), mem->dst, mem->length, KernelMode, &mem->length);
			}
		} __except (1)
		{
			//
			// do nothing
			//
			irp->IoStatus.Status = STATUS_INVALID_ADDRESS;
		}

		irp->IoStatus.Information = sizeof(DRIVER_READMEMORY_PROCESS);
	}

	else if (ioctl_code == IOCTL_GET_PHYSICAL)
	{
		DRIVER_GET_PHYSICAL *mem = (DRIVER_GET_PHYSICAL*)buffer;

		QWORD physical = *(QWORD*)mem->InOutPhysical;
		*(QWORD*)mem->InOutPhysical = (QWORD)MmGetPhysicalAddress(  (PVOID)physical  ).QuadPart;

		irp->IoStatus.Status = STATUS_SUCCESS;

		irp->IoStatus.Information = sizeof(DRIVER_GET_PHYSICAL);
	}

	else if (ioctl_code == IOCTL_GET_PCITSC)
	{
		DRIVER_PCITSC *mem = (DRIVER_PCITSC*)buffer;
		get_pci_latency(mem->loops, mem->bus, mem->slot, mem->func, mem->offset, mem->tsc);
		*(DWORD*)mem->tsc_overhead = tsc_overhead;
		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = sizeof(DRIVER_PCITSC);
	}
E0:
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

extern "C" VOID DriverUnload(
	_In_ struct _DRIVER_OBJECT* DriverObject
)
{
	UNREFERENCED_PARAMETER(DriverObject);
	if (gEfiMemoryMap)
	{
		ExFreePoolWithTag(gEfiMemoryMap, POOLTAG);
		IoDeleteSymbolicLink(&gDosDeviceName);
		IoDeleteDevice(gDeviceObject);
	}
}

extern "C" __declspec(dllimport) LIST_ENTRY * PsLoadedModuleList;
QWORD get_ntoskrnl_entry(void)
{
	typedef struct _KLDR_DATA_TABLE_ENTRY
	{
		struct _LIST_ENTRY InLoadOrderLinks;                                    //0x0
		VOID* ExceptionTable;                                                   //0x10
		ULONG ExceptionTableSize;                                               //0x18
		VOID* GpValue;                                                          //0x20
		struct _NON_PAGED_DEBUG_INFO* NonPagedDebugInfo;                        //0x28
		VOID* DllBase;                                                          //0x30
		VOID* EntryPoint;                                                       //0x38
	} KLDR_DATA_TABLE_ENTRY;

	KLDR_DATA_TABLE_ENTRY* module_entry =
		CONTAINING_RECORD(PsLoadedModuleList, KLDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
	return (QWORD)module_entry->EntryPoint;
}

extern "C" NTSTATUS DriverEntry(
	_In_ PDRIVER_OBJECT  DriverObject,
	_In_ PUNICODE_STRING RegistryPath
)
{
	UNREFERENCED_PARAMETER(RegistryPath);


	//
	// used for timing check https://github.com/andre-richter/pcie-lat/blob/master/pcie-lat.c
	//
	tsc_overhead = get_tsc_overhead();

	//
	// resolve KeLoaderBlock address
	//
	QWORD KeLoaderBlock = get_ntoskrnl_entry();
	KeLoaderBlock += 0x0C;
	KeLoaderBlock = (KeLoaderBlock + 7) + *(int*)(KeLoaderBlock + 3);
	KeLoaderBlock = *(QWORD*)(KeLoaderBlock);
	if (KeLoaderBlock == 0)
	{
		LOG("Error: Launch driver as boot start\n");

		return STATUS_FAILED_DRIVER_ENTRY;
	}


	//
	// setup driver comms
	//
	RtlInitUnicodeString(&gDeviceName, L"\\Device\\drvscan");
	if (!NT_SUCCESS(IoCreateDevice(DriverObject, 0, &gDeviceName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &gDeviceObject)))
	{
		//
		// driver is already installed
		//
		return STATUS_ENTRYPOINT_NOT_FOUND;
	}


	RtlInitUnicodeString(&gDosDeviceName, L"\\DosDevices\\drvscan");
	IoCreateSymbolicLink(&gDosDeviceName, &gDeviceName);
	SetFlag(gDeviceObject->Flags, DO_BUFFERED_IO);
	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
	{
		DriverObject->MajorFunction[i] = dummy_io;
	}
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IoControl;
	DriverObject->DriverUnload = DriverUnload;
	ClearFlag(gDeviceObject->Flags, DO_DEVICE_INITIALIZING);
	

	//
	// copy all efi memory pages from page tables
	//
	efi_page_count = 100;
	EFI_MEMORY_PAGES info{};
	info.page_address_buffer = (PVOID)efi_pages;
	info.page_count_buffer = (PVOID)efi_num_pages;
	info.total_count = &efi_page_count;
	if (!get_efi_memory_pages(&info))
	{
		return STATUS_FAILED_DRIVER_ENTRY;
	}


	//
	// copy efi memory map for later use
	//
	QWORD FirmwareInformation        = KeLoaderBlock + 0x108;
	QWORD EfiInformation             = FirmwareInformation + 0x08;
	QWORD EfiMemoryMap               = *(QWORD*)(EfiInformation + 0x28);
	DWORD EfiMemoryMapSize           = *(DWORD*)(EfiInformation + 0x30);


	gEfiMemoryMap     = ExAllocatePool2(POOL_FLAG_NON_PAGED, EfiMemoryMapSize, POOLTAG);
	gEfiMemoryMapSize = EfiMemoryMapSize;
	memcpy(gEfiMemoryMap, (const void *)EfiMemoryMap, EfiMemoryMapSize);

	return STATUS_SUCCESS;
}

BOOL get_efi_memory_pages(EFI_MEMORY_PAGES *info)
{
	cr3 kernel_cr3;
	kernel_cr3.flags = __readcr3();
	
	DWORD index = 0;

	QWORD physical_address = kernel_cr3.address_of_page_directory << PAGE_SHIFT;

	pml4e_64* pml4 = (pml4e_64*)(MmGetVirtualForPhysical(*(PHYSICAL_ADDRESS*)&physical_address));

	if (!MmIsAddressValid(pml4) || !pml4)
		return 0;

	
	for (int pml4_index = 256; pml4_index < 512; pml4_index++)
	{

		physical_address = pml4[pml4_index].page_frame_number << PAGE_SHIFT;
		if (!pml4[pml4_index].present)
		{
			continue;
		}
	
		pdpte_64* pdpt = (pdpte_64*)(MmGetVirtualForPhysical(*(PHYSICAL_ADDRESS*)&physical_address));
		if (!MmIsAddressValid(pdpt) || !pdpt)
			continue;

		for (int pdpt_index = 0; pdpt_index < 512; pdpt_index++)
		{
			physical_address = pdpt[pdpt_index].page_frame_number << PAGE_SHIFT;
			if (!pdpt[pdpt_index].present)
			{
				continue;
			}

			pde_64* pde = (pde_64*)(MmGetVirtualForPhysical(*(PHYSICAL_ADDRESS*)&physical_address));
			if (!MmIsAddressValid(pde) || !pde)
				continue;

			if (pdpt[pdpt_index].large_page)
			{
				//
				// we dont need add 1gb pages
				//
				continue;
			}



			DWORD page_count=0;
			QWORD previous_address=0;
			for (int pde_index = 0; pde_index < 512; pde_index++) {
				physical_address = pde[pde_index].page_frame_number << PAGE_SHIFT;

				if (!pde[pde_index].present)
				{

					if (page_count > 2 && *info->total_count > index)
					{
						*(QWORD*)((QWORD)info->page_address_buffer + (index * 8)) = previous_address - (page_count * 0x1000);
						*(QWORD*)((QWORD)info->page_count_buffer + (index * 8)) = page_count + 1;
						index++;
					}
					page_count = 0;
					previous_address = 0;
					continue;
				}

				pte_64* pte = (pte_64*)(MmGetVirtualForPhysical(*(PHYSICAL_ADDRESS*)&physical_address));
				if (!MmIsAddressValid(pte) || !pte)
				{
					if (page_count > 2 && *info->total_count > index)
					{
						*(QWORD*)((QWORD)info->page_address_buffer + (index * 8)) = previous_address - (page_count * 0x1000);
						*(QWORD*)((QWORD)info->page_count_buffer + (index * 8)) = page_count + 1;
						index++;
					}
					page_count = 0;
					previous_address = 0;
					continue;
				}

				if (pde[pde_index].large_page)
				{
					//
					// we dont need add 2mb pages
					//
					if (page_count > 2 && *info->total_count > index)
					{
						*(QWORD*)((QWORD)info->page_address_buffer + (index * 8)) = previous_address - (page_count * 0x1000);
						*(QWORD*)((QWORD)info->page_count_buffer + (index * 8)) = page_count + 1;
						index++;
					}
					page_count = 0;
					previous_address = physical_address;	
					continue;
				}
				
				for (int pte_index = 0; pte_index < 512; pte_index++)
				{
					physical_address = pte[pte_index].page_frame_number << PAGE_SHIFT;
					if (!pte[pte_index].present)
					{
						if (page_count > 2)
							goto add_page;
						continue;
					}
					
					if (pte[pte_index].execute_disable)
					{
						if (page_count > 2)
							goto add_page;
						continue;
					}
					
					if (PAGE_ALIGN(MmGetVirtualForPhysical(*(PHYSICAL_ADDRESS*)&physical_address)) != 0)
					{
						if (page_count > 2)
							goto add_page;
						continue;
					}

					if ((physical_address - previous_address) == 0x1000)
					{
						page_count++;
					}
					else
					{
					add_page:
						if (page_count > 2 && *info->total_count > index)
						{
							*(QWORD*)((QWORD)info->page_address_buffer + (index * 8)) = previous_address - (page_count * 0x1000);
							*(QWORD*)((QWORD)info->page_count_buffer + (index * 8)) = page_count + 1;
							index++;
						}
						page_count = 0;
					}
					previous_address = physical_address;
				}
			}
		}
	}
	*info->total_count = index;
	return index != 0;
}

inline void get_tsc_top(DWORD *high, DWORD *low)
{
	int buffer[4]{};
	__cpuid(buffer, 0);

	QWORD tsc  = __rdtsc();

	*high = (DWORD)(tsc >> 32);
	*low  = (DWORD)(tsc & 0xFFFFFFFF);
}

inline void get_tsc_bottom(DWORD *high, DWORD *low)
{
	DWORD aux;
	QWORD tsc  = __rdtscp((unsigned int*)&aux);

	*high = (unsigned int)(tsc >> 32);
	*low = (unsigned int)(tsc & 0xFFFFFFFF);

	int buffer[4]{};
	__cpuid(buffer, 0);
}

static DWORD get_tsc_overhead(void)
{
	QWORD sum;
	DWORD tsc_high_before, tsc_high_after;
	DWORD tsc_low_before, tsc_low_after;
	DWORD i;

	get_tsc_top(&tsc_high_before, &tsc_low_before);
	get_tsc_bottom(&tsc_high_after, &tsc_low_after);
	get_tsc_top(&tsc_high_before, &tsc_low_before);
	get_tsc_bottom(&tsc_high_after, &tsc_low_after);

	sum = 0;
	for (i = 0; i < OVERHEAD_MEASURE_LOOPS; i++) {
		KIRQL old;
		KeRaiseIrql(DISPATCH_LEVEL, &old);

		get_tsc_top(&tsc_high_before, &tsc_low_before);
		get_tsc_bottom(&tsc_high_after, &tsc_low_after);

		KeLowerIrql(old);

		/* Calculate delta; lower 32 Bit should be enough here */
	        sum += tsc_low_after - tsc_low_before;
	}
	return (DWORD)(sum / OVERHEAD_MEASURE_LOOPS);
}

void get_pci_latency(DWORD loops, BYTE bus, BYTE slot, BYTE func, BYTE offset , QWORD *tsc)
{
	QWORD sum;
	DWORD tsc_high_before, tsc_high_after;
	DWORD tsc_low_before, tsc_low_after;
	QWORD tsc_start, tsc_end, tsc_diff;
	DWORD i;

	/*
	 * "Warmup" of the benchmarking code.
	 * This will put instructions into cache.
	 */
	get_tsc_top(&tsc_high_before, &tsc_low_before);
	get_tsc_bottom(&tsc_high_after, &tsc_low_after);
	get_tsc_top(&tsc_high_before, &tsc_low_before);
	get_tsc_bottom(&tsc_high_after, &tsc_low_after);

        /* Main latency measurement loop */
	sum = 0;
	for (i = 0; i < loops; i++) {

		KIRQL old;
		KeRaiseIrql(DISPATCH_LEVEL, &old);

		get_tsc_top(&tsc_high_before, &tsc_low_before);

		/*** Function to measure execution time for ***/
		read_pci_u16(bus, slot, func, offset);
		/***************************************/

		get_tsc_bottom(&tsc_high_after, &tsc_low_after);

		KeLowerIrql(old);

		/* Calculate delta */
		tsc_start = ((QWORD) tsc_high_before << 32) | tsc_low_before;
		tsc_end = ((QWORD) tsc_high_after << 32) | tsc_low_after;
	        tsc_diff = tsc_end  - tsc_start;

		sum += tsc_diff;
	}

	*tsc = (sum / loops);
}

