#include "../client.h"
#include "clkm.h"

#define IOCTL_READMEMORY         0xECAC00
#define IOCTL_IO_READ            0xECAC02
#define IOCTL_IO_WRITE           0xECAC12
#define IOCTL_REQUEST_MMAP       0xECAC04
#define IOCTL_REQUEST_PAGES      0xECAC06
#define IOCTL_READMEMORY_PROCESS 0xECAC08
#define IOCTL_GET_PHYSICAL       0xECAC10

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

BOOL cl::clkm::initialize(void)
{
	if (hDriver != 0)
	{
		return 1;
	}

	hDriver = CreateFileA("\\\\.\\drvscan", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

	if (hDriver == INVALID_HANDLE_VALUE)
	{
		hDriver = 0;
	}

	return hDriver != 0;
}

BOOL cl::clkm::read_virtual(DWORD pid, QWORD address, PVOID buffer, QWORD length)
{
	if (!cl::initialize())
	{
		return 0;
	}

	if (pid == 4 || pid == 0)
	{
		DRIVER_READMEMORY io{};
		io.address = (PVOID)address;

		PVOID tmp_buffer = (PVOID)malloc(length);

		io.buffer = tmp_buffer;
		io.length = length;

		BOOL status = DeviceIoControl(hDriver, IOCTL_READMEMORY, &io, sizeof(io), &io, sizeof(io), 0, 0);

		if (status)
		{
			memcpy(buffer, tmp_buffer, length);
		}
		else
		{
			memset(buffer, 0, length);
		}

		free(tmp_buffer);

		return status;
	}

	DRIVER_READMEMORY_PROCESS io{};
	io.src = (PVOID)address;

	PVOID tmp_buffer = (PVOID)malloc(length);

	io.dst = tmp_buffer;
	io.length = length;
	io.pid = pid;

	BOOL status = DeviceIoControl(hDriver, IOCTL_READMEMORY_PROCESS, &io, sizeof(io), &io, sizeof(io), 0, 0);

	if (status)
	{
		memcpy(buffer, tmp_buffer, length);
	}
	else
	{
		memset(buffer, 0, length);
	}

	free(tmp_buffer);

	return status;
}

BOOL cl::clkm::read_mmio(QWORD address, PVOID buffer, QWORD length)
{
	DRIVER_READMEMORY io;
	io.address = (PVOID)address;
	io.buffer = buffer;
	io.length = length;
	return DeviceIoControl(hDriver, IOCTL_IO_READ, &io, sizeof(io), &io, sizeof(io), 0, 0);
}

BOOL cl::clkm::write_mmio(QWORD address, PVOID buffer, QWORD length)
{
	DRIVER_READMEMORY io;
	io.address = (PVOID)address;
	io.buffer = buffer;
	io.length = length;
	return DeviceIoControl(hDriver, IOCTL_IO_WRITE, &io, sizeof(io), &io, sizeof(io), 0, 0);
}

QWORD cl::clkm::get_physical_address(QWORD virtual_address)
{
	DRIVER_GET_PHYSICAL io{};
	io.InOutPhysical = (PVOID)&virtual_address;
	if (!DeviceIoControl(hDriver, IOCTL_GET_PHYSICAL, &io, sizeof(io), &io, sizeof(io), 0, 0))
		return 0;
	return virtual_address;
}

PVOID cl::clkm::__get_memory_map(QWORD* size)
{
	PVOID buffer = 0;
	QWORD buffer_size = 0;
	DRIVER_REQUEST_MAP io{};

	io.buffer = (PVOID)&buffer;
	io.buffer_size = (QWORD)&buffer_size;

	if (!DeviceIoControl(hDriver, IOCTL_REQUEST_MMAP, &io, sizeof(io), &io, sizeof(io), 0, 0))
	{
		return 0;
	}

	*size = buffer_size;

	return buffer;
}

PVOID cl::clkm::__get_memory_pages(QWORD* size)
{
	if (!cl::initialize())
	{
		return 0;
	}

	PVOID buffer = 0;
	QWORD buffer_size = 0;
	DRIVER_REQUEST_MAP io{};

	io.buffer = (PVOID)&buffer;
	io.buffer_size = (QWORD)&buffer_size;

	if (!DeviceIoControl(hDriver, IOCTL_REQUEST_PAGES, &io, sizeof(io), &io, sizeof(io), 0, 0))
	{
		return 0;
	}

	*size = buffer_size;

	return buffer;
}

#pragma pack(push, 1)
typedef struct {
	BYTE     bus, slot, func;
	BYTE     offset;
	DWORD    loops;
	QWORD    *tsc;
	DWORD    *tsc_overhead;
} DRIVER_PCITSC ;
#pragma pack(pop)

void cl::clkm::get_pci_latency(BYTE bus, BYTE slot, BYTE func, BYTE offset, DWORD loops, DRIVER_TSC *out)
{
	if (!cl::initialize())
	{
		return;
	}

	DWORD overhead=0;
	QWORD tscqw=0;
	DRIVER_PCITSC tsc{};

	tsc.bus = bus;
	tsc.slot = slot;
	tsc.func = func;
	tsc.offset = offset;
	tsc.loops = loops;
	tsc.tsc_overhead = &overhead;
	tsc.tsc = &tscqw;
	DeviceIoControl(hDriver, 0xECAC14, &tsc, sizeof(DRIVER_PCITSC), &tsc, sizeof(DRIVER_PCITSC), 0, 0);
	out->tsc = tscqw;
	out->tsc_overhead = overhead;
}

