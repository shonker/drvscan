#define _CRT_SECURE_NO_WARNINGS
#include "client.h"

#define DEBUG
#define LOG(...) printf("[drvscan] "  __VA_ARGS__)
#ifdef DEBUG
#define DEBUG_LOG(...) printf("[drvscan] " __VA_ARGS__)
#else
#define DEBUG_LOG(...) // __VA_ARGS__
#endif

inline void FontColor(int color=0x07) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color); }

#define LOG_RED(...) \
printf("[drvscan] "); \
FontColor(4); \
printf(__VA_ARGS__); \
FontColor(7); \

#define LOG_YELLOW(...) \
printf("[drvscan] "); \
FontColor(14); \
printf(__VA_ARGS__); \
FontColor(7); \

#define PRINT_RED(...) \
FontColor(4); \
printf(__VA_ARGS__); \
FontColor(7); \

#define PRINT_GREEN(...) \
FontColor(2); \
printf(__VA_ARGS__); \
FontColor(7); \

static void scan_efi(BOOL dump);
static BOOL dump_module_to_file(std::vector<FILE_INFO> modules, DWORD pid, FILE_INFO file);
static void scan_image(std::vector<FILE_INFO> modules, DWORD pid, FILE_INFO file, BOOL use_cache);
static void scan_pci(BOOL disable, BOOL dump_cfg, BOOL dump_bar);

int main(int argc, char **argv)
{
	//
	// reset font
	//
	FontColor(7);

	if (!cl::initialize())
	{
		LOG("driver is not running\n");
		printf("Press any key to continue . . .");
		return getchar();
	}

	if (argc < 2)
	{
		LOG("--help\n");
		return getchar();
	}
	
	DWORD scan = 0, pid = 4, savecache = 0, scanpci = 0, block=0, cfg=0, bar=0, use_cache = 0, scanefi = 0, dump = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--help"))
		{
			printf(
				"\n\n"

				"--scan                 scan target process memory changes\n"
				"    --pid              target process id\n"
				"    --usecache         (optional) we use local cache instead of original PE files\n"
				"    --savecache        (optional) dump target process modules to disk\n\n"
				"--scanefi              scan abnormals from efi memory map\n"
				"    --dump             (optional) dump found abnormal to disk\n\n"
				"--scanpci              scan pci cards from the system\n"
				"    --block            block illegal cards\n"
				"    --cfg              print out every card cfg space\n"
				"    --bar              print out every card bar space\n\n\n"
			);

			printf("\nExample (verifying modules integrity by using cache):\n"
				"1.                     load malware\n"
				"1.                     drvscan.exe --scan --savecache --pid 4\n"
				"2.                     reboot the computer\n"
				"3.                     load windows without malware\n"
				"4.                     drvscan.exe --scan --usecache --pid 4\n"
				"all malware patches should be now visible\n\n"
			);
		}

		else if (!strcmp(argv[i], "--scan"))
		{
			scan = 1;
		}

		else if (!strcmp(argv[i], "--pid"))
		{
			pid = atoi(argv[i + 1]);
		}

		else if (!strcmp(argv[i], "--savecache"))
		{
			savecache = 1;
		}

		else if (!strcmp(argv[i], "--scanpci"))
		{
			scanpci = 1;
		}

		else if (!strcmp(argv[i], "--block"))
		{
			block = 1;
		}

		else if (!strcmp(argv[i], "--cfg"))
		{
			cfg = 1;
		}

		else if (!strcmp(argv[i], "--bar"))
		{
			bar = 1;
		}

		else if (!strcmp(argv[i], "--scanefi"))
		{
			scanefi = 1;
		}

		else if (!strcmp(argv[i], "--dump"))
		{
			dump = 1;
		}

		else if (!strcmp(argv[i], "--usecache"))
		{
			use_cache = 1;
		}
	}

	if (scanpci)
	{
		LOG("scanning PCIe devices\n");
		scan_pci(block, cfg, bar);
		LOG("scan is complete\n");
	}

	if (scan)
	{
		std::vector<FILE_INFO> modules;

		if (pid == 4)
		{
			modules = get_kernel_modules();
		}
		else
		{
			modules = get_user_modules(pid);
		}

		LOG("scanning modules\n");
		for (auto mod : modules)
		{
			if (savecache)
			{
				dump_module_to_file(modules, pid, mod);
			}
			else
			{
				scan_image(modules, pid, mod, use_cache);
			}
		}
		LOG("scan is complete\n");
	}

	if (scanefi)
	{
		LOG("scanning EFI\n");
		scan_efi(dump);
		LOG("scan is complete\n");
	}

	//
	// add watermark
	//
	PRINT_GREEN("\nbuild date: %s, %s\n", __DATE__, __TIME__);

	return 0;
}

typedef struct _DEVICE_INFO {
	unsigned char  bus, slot, func, cfg[0x200];

	QWORD physical_address;

	std::vector<struct _DEVICE_INFO> childrens;
} DEVICE_INFO;

typedef struct _PCIE_DEVICE_INFO {
	unsigned char blk;
	unsigned char info;
	DEVICE_INFO   d; // device
	DEVICE_INFO   p; // parent
} PCIE_DEVICE_INFO;

const char *blkinfo(unsigned char info)
{
	switch (info)
	{
	case 1:  return "pcileech";
	case 2:  return "bus master off";
	case 3:  return "xilinx";
	case 4:  return "invalid bridge";
	case 5:  return "hidden device";
	case 6:  return "invalid pm cap";
	case 7:  return "invalid msi cap";
	case 8:  return "invalid pcie cap";
	case 9:  return "invalid multi func device";
	case 10: return "invalid header type 0";
	case 11: return "invalid header type 1";
	case 12: return "invalid header type";
	case 13: return "invalid config"; // just general msg
	case 14: return "invalid device type"; // just general msg
	case 15: return "port/device mismatch";
	case 16: return "driverless card";
	case 17: return "invalid network adapter";
	case 18: return "no network connections";
	case 19: return "driverless card with bus master";
	}
	return "OK";
}

void PrintPcieConfiguration(unsigned char *cfg, int size)
{
	printf("\n>    00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n\n");
	int line_counter=0;
	for (int i = 0; i < size; i++)
	{
		if (line_counter == 0)
		{
			if (i < 0xFF)
				printf("%02X   ", i);
			else
				printf("%02X  ", i);
		}
		line_counter++;
		printf("%02X ", cfg[i]);
		if (line_counter == 16)
		{
			printf("\n");
			line_counter=0;
		}	
	}
	printf("\n");
}

void PrintPcieBarSpace(DWORD bar)
{
	int line_counter=0;
	int row_max_count=0;
	for (int i = 0; i < 0x1000; i+=4)
	{
		unsigned int cfg = cl::io::read<unsigned int>(bar + i);
		line_counter++;
		printf("%08X,", cfg);
		if (line_counter == 4)
		{
			printf("\n");
			line_counter=0;
		}
		row_max_count++;

		if (row_max_count == (16*4))
		{
			printf("\n");
			row_max_count=0;
		}
	}
	printf("\n");
}

DWORD get_port_type(unsigned char *cfg)
{
	PVOID pcie = pci::get_pcie(cfg);
	if (pcie == 0)
	{
		return 0;
	}
	return pci::pcie::cap::pcie_cap_device_port_type(pcie);
}

BOOL is_xilinx(unsigned char *cfg)
{
	unsigned char *a0 = cfg + *(BYTE*)(cfg + 0x34);
	if (a0[1] == 0)
		return 0;

	a0 = cfg + a0[1];
	if (a0[1] == 0)
		return 0;
	DWORD a1 = *(DWORD*)(cfg + a0[1] + 0x0C);
	return (GET_BITS(a1, 14, 12) + GET_BITS(a1, 17, 15) + (GET_BIT(a1, 10) | GET_BIT(a1, 11))) == 15;
}

void validate_device_config(PCIE_DEVICE_INFO &device)
{
	using namespace pci;

	DEVICE_INFO &dev = device.d;
	DEVICE_INFO &port = device.p;


	PVOID pcie = get_pcie(dev.cfg);
	if (pcie != 0)
	{
		//
		// compare data between device data and port
		//
		if (link::status::link_speed(get_link(dev.cfg)) > link::status::link_speed(get_link(port.cfg)))
		{
			device.blk = 2; device.info = 15;
			return;
		}

		if (link::status::link_width(get_link(dev.cfg)) > link::status::link_width(get_link(port.cfg)))
		{
			device.blk = 2; device.info = 15;
			return;
		}

		//
		// end point device never should be bridge without devices
		//
		if (pcie::cap::pcie_cap_device_port_type(pcie) >= PciExpressRootPort)
		{
			device.blk = 2; device.info = 14;
			return;
		}
	}

	//
	// bus master is disabled, we can safely disable them
	//
	if (!GET_BIT(*(WORD*)(dev.cfg + 0x04), 2))
	{
		BOOL allow=0;
		//
		// verify if any multifunc devices got bus master enabled
		//
		if (GET_BIT(header_type(dev.cfg), 7))
		{
			for (auto &entry : dev.childrens)
			{
				if (GET_BIT(*(WORD*)(entry.cfg + 0x04), 2))
				{
					allow = 1;
					break;
				}
			}
		}

		if (allow)
		{
			return;
		}

		device.blk = 1; device.info = 2;

		return;
	}

	//
	// invalid cfg
	//
	if (capabilities_ptr(dev.cfg) == 0)
	{
		device.blk = 2; device.info = 13;
		return;
	}

	//
	// can be just used to identify xilinx FPGA
	//
	/*
	if (is_xilinx(device.cfg))
	{
		device.blk = 2; device.info = 3;
		return;
	}
	*/


	//
	// hidden device, LUL.
	//
	if (device_id(dev.cfg) == 0xFFFF && vendor_id(dev.cfg) == 0xFFFF)
	{
		device.blk  = 2; device.info = 5;
		return;
	}


	//
	// invalid VID/PID
	//
	if (device_id(dev.cfg) == 0x0000 && vendor_id(dev.cfg) == 0x0000)
	{
		device.blk  = 2; device.info = 5;
		return;
	}

	/*
	not every device got PM cap
	PVOID pm = get_pm(dev.cfg);

	if (pm == 0)
	{
		device.blk = 2; device.info = 6;
		return;
	}
	*/
	
	/*
	not every device got MSI cap
	PVOID msi = get_msi(dev.cfg);
	if (msi == 0)
	{
		device.blk = 1; device.info = 7;
		return;
	}
	*/
	if (pcie == 0)
	{
		device.blk = 2; device.info = 8;
		return;
	}

	//
	// invalid port device test
	//
	for (auto& children : dev.childrens)
	{
		if (device_id(children.cfg) == 0xFFFF && vendor_id(children.cfg) == 0xFFFF)
		{
			device.blk = 2; device.info = 9;
			return;
		}
		/*
		????
		if (device_id(children.cfg) == 0x0000 && vendor_id(children.cfg) == 0x0000)
		{
			device.blk = 2; device.info = 9;
			return;
		}
		*/
	}
	
	//
	// 1432
	// Header Type: bit 7 (0x80) indicates whether it is a multi-function device,
	// while interesting values of the remaining bits are: 00 = general device, 01 = PCI-to-PCI bridge.
	// src: https://www.khoury.northeastern.edu/~pjd/cs7680/homework/pci-enumeration.html
	//
	if (GET_BIT(header_type(dev.cfg), 7))
	{
		//
		// check if we have any children devices
		//
		if (!dev.childrens.size())
		{
			device.blk = 2; device.info = 9;
			return;
		}
	}

	//
	// header type 1 (bridge)
	//
	if (GET_BITS(header_type(dev.cfg), 6, 0) == 1)
	{
		//
		// Type 1 Configuration Space Header is used for Root Port and Upstream Port/Downstream Port of PCIe Switch.
		//
		if (
			pcie::cap::pcie_cap_device_port_type(pcie) != PciExpressDownstreamSwitchPort &&
			pcie::cap::pcie_cap_device_port_type(pcie) != PciExpressUpstreamSwitchPort   &&
			pcie::cap::pcie_cap_device_port_type(pcie) != PciExpressRootPort)
		{
			device.blk = 2; device.info = 11;
			return;
		}
	}
	//
	// header type 0
	//
	else if (GET_BITS(header_type(dev.cfg), 6, 0) == 0)
	{
		//
		// Type 0 Configuration Space Hader is used for Endpoint Device
		//
		if (pcie::cap::pcie_cap_device_port_type(pcie) >= PciExpressRootPort)
		{
			device.blk = 2; device.info = 10;
			return;
		}
	}
	//
	// invalid device
	//
	else
	{
		device.blk = 2; device.info = 12;
		return;
	}
}

void validate_network_adapters(PCIE_DEVICE_INFO &device, PNP_ADAPTER &pnp_adapter)
{
	using namespace pci;

	BOOL  found       = 0;
	BOOL  status      = 0;

	QWORD table       = wmi::open_table("SELECT * FROM Win32_NetworkAdapter where PNPDeviceID is not NULL");
	QWORD table_entry = wmi::next_entry(table, 0);
	while (table_entry)
	{
		std::string pnp_id = wmi::get_string(table_entry, "PNPDeviceID");
		if (pnp_id.size() && !_strcmpi(pnp_id.c_str(), pnp_adapter.pnp_id.c_str()))
		{
			found  = 1;
			status = wmi::get_bool(table_entry, "NetEnabled");
			break;
		}
		table_entry = wmi::next_entry(table, table_entry);
	}
	wmi::close_table(table);


	if (found == 0)
	{
		//
		// sus
		//
		device.info = 17;
		device.blk  = 2;
		return;
	}

	if (status == 0)
	{
		device.info = 18;
		device.blk  = 1;
		return;
	}
}

void validate_device_features(PCIE_DEVICE_INFO &device, std::vector<PNP_ADAPTER> &pnp_adapters)
{
	using namespace pci;

	//
	// check if device is backed by driver
	//
	BOOL found = 0;
	PNP_ADAPTER pnp_adapter;

	for (auto& pnp : pnp_adapters)
	{
		DEVICE_INFO& dev = device.d;

		if (pnp.bus == dev.bus &&
			pnp.slot == dev.slot &&
			pnp.func == dev.func
			)
		{
			found = 1;
			pnp_adapter = pnp;
			break;
		}
	}

	if (!found)
	{
		//
		// bus master was forcefully enabled(?)
		//
		if (GET_BIT(command(device.d.cfg), 2))
		{
			device.blk = 2;
			device.info = 19;
		}
		else
		{
			device.blk = 1;
			device.info = 16;
		}
		return;
	}

	//
	// check network card features
	//
	if (class_code(device.d.cfg) == 0x020000 || class_code(device.d.cfg) == 0x028000)
	{
		validate_network_adapters(device, pnp_adapter);
	}
}

PCSTR get_port_type_str(unsigned char *cfg)
{
	switch (get_port_type(cfg))
	{
		case PciExpressEndpoint: return "PciExpressEndpoint";
		case PciExpressLegacyEndpoint: return "PciExpressLegacyEndpoint";
		case 2: return "NVME";
		case PciExpressRootPort: return "PciExpressRootPort";
		case PciExpressUpstreamSwitchPort: return "PciExpressUpstreamSwitchPort";
		case PciExpressDownstreamSwitchPort: return "PciExpressDownstreamSwitchPort";
		case PciExpressToPciXBridge: return "PciExpressToPciXBridge";
		case PciXToExpressBridge: return "PciXToExpressBridge";
		case PciExpressRootComplexIntegratedEndpoint: return "PciExpressRootComplexIntegratedEndpoint";
		case PciExpressRootComplexEventCollector: return "PciExpressRootComplexEventCollector";
	}
	return "";
}

inline void print_device_info(PCIE_DEVICE_INFO entry)
{
	DEVICE_INFO &parent = entry.p;
	DEVICE_INFO &dev = entry.d;

	//
	// print parent information (bridge)
	//
	printf("[%s] [%02d:%02d:%02d] [%04X:%04X] (%s)\n",
		get_port_type_str(parent.cfg), parent.bus, parent.slot, parent.func, *(WORD*)(parent.cfg), *(WORD*)(parent.cfg + 0x02), blkinfo(entry.info));

	//
	// print device PCIe device information
	//
	printf("	[%s] [%02d:%02d:%02d] [%04X:%04X]\n",
		get_port_type_str(dev.cfg), dev.bus, dev.slot, dev.func, *(WORD*)(dev.cfg), *(WORD*)(dev.cfg + 0x02));

	//
	// print children PCIe device information
	//
	for (auto &child : dev.childrens)
	{
		printf("	[%s] [%02d:%02d:%02d] [%04X:%04X]\n",
			get_port_type_str(child.cfg), child.bus, child.slot, child.func, *(WORD*)(child.cfg), *(WORD*)(child.cfg + 0x02));
	}
}

void test_devices(std::vector<PCIE_DEVICE_INFO> &devices, BOOL disable)
{
	std::vector<PNP_ADAPTER> pnp_adapters = get_pnp_adapters();

	//
	// test shadow cfg (pcileech-fpga 4.11 and lower)
	//
	for (auto &entry : devices)
	{
		DEVICE_INFO &dev = entry.d;

		DWORD tick = GetTickCount();
		cl::pci::write<WORD>(dev.bus, dev.slot, 0xA0, *(WORD*)(dev.cfg + 0xA0));
		tick = GetTickCount() - tick;
		if (tick > 100)
			continue;

		tick = GetTickCount();
		cl::pci::write<WORD>(dev.bus, dev.slot, 0xA8, *(WORD*)(dev.cfg + 0xA8));
		tick = GetTickCount() - tick;
		if (tick > 100)
		{
			entry.blk = 2;
			entry.info = 1;
			break;
		}
	}
	
	//
	// check configuration space
	//
	for (auto &entry : devices)
	{
		//
		// device was already blocked
		//
		if (entry.blk)
		{
			continue;
		}

		validate_device_config(entry);
	}

	//
	// check device features
	//
	for (auto &entry : devices)
	{
		//
		// device was already blocked
		//
		if (entry.blk)
		{
			continue;
		}
		validate_device_features(entry, pnp_adapters);
	}

	for (auto &entry : devices)
	{
		if (!entry.blk)
		{
			print_device_info(entry);
			printf("\n");
		}
	}

	std::vector<DEVICE_INFO> blocked_devices;

	for (auto &entry : devices)
	{
		if (entry.blk == 1)
		{
			FontColor(14);
			print_device_info(entry);
			FontColor(7);
			printf("\n");
			if (disable)
			{
				//
				// check if bus master is enabled from bridge
				//
				WORD command = pci::command(entry.p.cfg);
				if (GET_BIT(command, 2))
				{
					//
					// disable bus master from bridge
					//
					command &= ~(1 << 2);
					cl::io::write<WORD>(entry.p.physical_address + 0x04, command);

					blocked_devices.push_back(entry.p);
				}
			}
		}
	}

	for (auto &entry : devices)
	{
		if (entry.blk == 2)
		{
			FontColor(4);
			print_device_info(entry);
			FontColor(7);
			printf("\n");
			if (disable)
			{
				//
				// check if bus master is enabled from bridge
				//
				WORD command = pci::command(entry.p.cfg);
				if (GET_BIT(command, 2))
				{
					//
					// disable bus master from bridge
					//
					command &= ~(1 << 2);
					cl::io::write<WORD>(entry.p.physical_address + 0x04, command);

					blocked_devices.push_back(entry.p);
				}
			}
		}
	}

	if (blocked_devices.size())
	{
		LOG("Press any key to unblock devices . . .\n");
		getchar();

		for (auto &entry : blocked_devices)
		{
			cl::io::write<WORD>(entry.physical_address + 0x04, pci::command(entry.cfg));
		}
	}
}

std::vector<PCIE_DEVICE_INFO> get_root_bridge_devices(void)
{
	std::vector<PCIE_DEVICE_INFO> devices;
	unsigned char bus = 0;
	for (unsigned char slot = 0; slot < 32; slot++)
	{
		QWORD physical_address = cl::pci::get_physical_address(bus, slot);
		if (physical_address == 0)
		{
			goto E0;
		}

		for (unsigned char func = 0; func < 8; func++)
		{
			QWORD entry = physical_address + (func << 12l);

			DWORD class_code = 0;
			((unsigned char*)&class_code)[0] = cl::io::read<BYTE>(entry + 0x09 + 0);
			((unsigned char*)&class_code)[1] = cl::io::read<BYTE>(entry + 0x09 + 1);
			((unsigned char*)&class_code)[2] = cl::io::read<BYTE>(entry + 0x09 + 2);
				
			int invalid_cnt = 0;
			for (int i = 0; i < 8; i++)
			{
				if (cl::io::read<BYTE>(entry + 0x04 + i) == 0xFF)
				{
					invalid_cnt++;
				}
			}

			if (invalid_cnt == 8)
			{
				if (func == 0)
				{
					break;
				}
				continue;
			}

			if (class_code != 0x060400)
			{
				continue;
			}

			PCIE_DEVICE_INFO device{};
			device.d.bus = bus;
			device.d.slot = slot;
			device.d.func = func;
			device.d.physical_address = entry;

			//
			// do not even ask... intel driver problem
			//
			for (int i = 0; i < 0x200; i++)
			{
				*(BYTE*)((char*)device.d.cfg + i) = cl::io::read<BYTE>(entry + i);
			}
			devices.push_back(device);
		}
	}
E0:
	return devices;
}

std::vector<DEVICE_INFO> get_devices_by_bus(unsigned char bus)
{
	std::vector<DEVICE_INFO> devices;
	for (unsigned char slot = 0; slot < 32; slot++)
	{
		QWORD physical_address = cl::pci::get_physical_address(bus, slot);
		if (physical_address == 0)
		{
			goto E0;
		}

		for (unsigned char func = 0; func < 8; func++)
		{
			QWORD entry = physical_address + (func << 12l);
				
			int invalid_cnt = 0;
			for (int i = 0; i < 8; i++)
			{
				if (cl::io::read<BYTE>(entry + 0x04 + i) == 0xFF)
				{
					invalid_cnt++;
				}
			}

			if (invalid_cnt == 8)
			{
				if (func == 0)
				{
					break;
				}
				continue;
			}

			DEVICE_INFO device;
			device.bus = bus;
			device.slot = slot;
			device.func = func;
			device.physical_address = entry;

			//
			// do not even ask... intel driver problem
			//
			for (int i = 0; i < 0x200; i++)
			{
				*(BYTE*)((char*)device.cfg + i) = cl::io::read<BYTE>(entry + i);
			}
			devices.push_back(device);
		}
	}
E0:
	return devices;
}

static BOOL is_bridge_device(PCIE_DEVICE_INFO& dev)
{
	using namespace pci;

	//
	// validate if its real bridge
	//
	if (class_code(dev.d.cfg) != 0x060400)
	{
		return 0;
	}

	//
	// type0 endpoint device
	//
	if (GET_BITS(header_type(dev.d.cfg), 6, 0) == 0)
	{
		return 0;
	}

	return 1;
}

std::vector<PCIE_DEVICE_INFO> get_inner_devices(std::vector<PCIE_DEVICE_INFO> &devices)
{
	using namespace pci;


	std::vector<PCIE_DEVICE_INFO> devs;


	for (auto &entry : devices)
	{
		if (!is_bridge_device(entry))
		{
			continue;
		}

		BYTE max_bus = type1::subordinate_bus_number(entry.d.cfg);
		auto bridge_devices = get_devices_by_bus(type1::secondary_bus_number(entry.d.cfg));

		for (auto &bridge : bridge_devices)
		{
			if (bridge.bus > max_bus)
			{
				continue;
			}
			devs.push_back({0, 0, bridge, entry.d});
		}
	}
	return devs;
}

std::vector<PCIE_DEVICE_INFO> get_endpoint_device_list(void)
{
	using namespace pci;

	std::vector<PCIE_DEVICE_INFO> root_devices = get_root_bridge_devices();


	std::vector<PCIE_DEVICE_INFO> endpoint_devices;
	while (1)
	{
		std::vector<PCIE_DEVICE_INFO> fake_bridges;
		for (auto &dev : root_devices)
		{
			if (!is_bridge_device(dev))
			{
				if (dev.d.func != 0)
				{
					endpoint_devices[endpoint_devices.size() - 1].d.childrens.push_back(dev.d);
				}
				else
				{
					endpoint_devices.push_back(dev);
				}
			}
			else
			{
				fake_bridges.push_back(dev);
			}
		}
		root_devices = get_inner_devices(root_devices);
		if (!root_devices.size())
		{
			//
			// add fake bridges too
			//
			for (auto &dev : fake_bridges)
			{
				if (dev.d.func != 0)
				{
					endpoint_devices[endpoint_devices.size() - 1].d.childrens.push_back(dev.d);
				}
				else
				{
					endpoint_devices.push_back(dev);
				}
			}
			break;
		}
	}
	return endpoint_devices;
}

void filter_pci_cfg(unsigned char *cfg)
{
	using namespace pci;

	printf(
		"\n[General information]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("CFG_VEND_ID | CFG_DEV_ID 			%04X %04X\n", vendor_id(cfg), device_id(cfg));
	printf("CFG_SUBSYS_VEND_ID | CFG_SUBSYS_ID 		%04X %04X\n", subsys_vendor_id(cfg), subsys_id(cfg));
	printf("CFG_REV_ID 					%ld\n", revision_id(cfg));
	printf("HEADER_TYPE 					0x%lx\n", header_type(cfg));
	printf("BAR0 						%lx\n", bar(cfg)[0]);
	printf("CLASS_CODE 					%06X\n", class_code(cfg));
	printf("CAPABILITIES_PTR | PM_BASE_PTR 			0x%x\n", capabilities_ptr(cfg));
	printf("INTERRUPT_LINE                                  %lx\n", interrupt_line(cfg));
	printf("INTERRUPT_PIN                                   %lx\n", interrupt_pin(cfg));
	printf("---------------------------------------------------------------------\n");

	PVOID pm = get_pm(cfg);

	if (pm != 0)
	{	
		printf(
			"\n[PM Cap]\n"
			"---------------------------------------------------------------------\n"
		);
		printf("PM_CAP_ON 					%d\n", pm::cap::pm_cap_on(pm));
		printf("PM_CAP_NEXTPTR | MSI_BASE_PTR 			0x%x\n", pm::cap::pm_cap_next_ptr(pm));
		printf("PM_CAP_ID 					%d\n",pm::cap::pm_cap_id(pm));
		printf("PM_CAP_PME_CLOCK 				%d\n", pm::cap::pm_cap_pme_clock(pm));
		printf("PM_CAP_DSI 					%d\n", pm::cap::pm_cap_dsi(pm));
		printf("PM_CAP_AUXCURRENT 				%d\n", pm::cap::pm_cap_auxcurrent(pm));
		printf("PM_CAP_D1SUPPORT PM_CAP_D2SUPPORT 		%d %d\n", pm::cap::pm_cap_d1support(pm), pm::cap::pm_cap_d2support(pm));
		printf("PM_CAP_PMESUPPORT 				0x0%x\n", pm::cap::pm_cap_pmesupport(pm));
		printf("PM_CAP_RSVD_04 					%ld\n", pm::cap::pm_cap_rsvd_04(pm));
		printf("PM_CAP_VERSION 					%ld\n", pm::cap::pm_cap_version(pm));
		printf("---------------------------------------------------------------------\n");

		printf(
			"\n[PMCSR]\n"
			"---------------------------------------------------------------------\n"
		);
		printf("PM_CSR_NOSOFTRST 				%ld\n", pm::csr::pm_csr_nosoftrst(pm));
		printf("PM_CSR_BPCCEN    				%ld\n", pm::csr::pm_csr_bpccen(pm));
		printf("PM_CSR_B2B3S     				%ld\n", pm::csr::pm_csr_b2b3s(pm));
		printf("PMCSR PWR STATE 				%ld\n", pm::csr::pm_csr_power_state(pm));
		printf("PMCSR PMESTATUS 				%ld\n", pm::csr::pm_csr_pme_status(pm));
		printf("PMCSR DATA SCALE 				%ld\n", pm::csr::pm_csr_data_scale(pm));
		printf("PMCSR DATA SELECT 				%ld\n", pm::csr::pm_csr_pme_status(pm));
		printf("PMCSR PME ENABLE 				%ld\n", pm::csr::pm_csr_pme_enabled(pm));
		printf("PMCSR reserved 					%ld\n", pm::csr::pm_csr_reserved(pm));
		printf("PMCSR dynamic data 				%ld\n", pm::csr::pm_csr_dynamic_data(pm));
		printf("---------------------------------------------------------------------\n");
	}
	PVOID msi = get_msi(cfg);

	if (msi != 0)
	{
		printf(
			"\n[MSI CAP]\n"
			"---------------------------------------------------------------------\n"
		);
		printf("MSI_CAP_ON 					%d\n", msi::cap::msi_cap_on(msi));
		printf("MSI_CAP_NEXTPTR | PCIE_BASE_PTR 		0x%x\n", msi::cap::msi_cap_nextptr(msi));
		printf("MSI_CAP_ID 					0x0%lx\n", msi::cap::msi_cap_id(msi));
		printf("MSI_CAP_MULTIMSGCAP 				%ld\n", msi::cap::msi_cap_multimsgcap(msi));
		printf("MSI_CAP_MULTIMSG_EXTENSION 			%ld\n", msi::cap::msi_cap_multimsg_extension(msi));
		printf("MSI_CAP_64_BIT_ADDR_CAPABLE 			%ld\n", msi::cap::msi_cap_64_bit_addr_capable(msi));
		printf("MSI_CAP_PER_VECTOR_MASKING_CAPABLE 		%ld\n", msi::cap::msi_cap_per_vector_masking_capable(msi));
		printf("---------------------------------------------------------------------\n");
	}
	PVOID pcie = get_pcie(cfg);

	if (pcie == 0)
	{
		return;
	}
	
	printf(
		"\n[PE CAP]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("PCIE_CAP_ON 					%d\n", pcie::cap::pcie_cap_on(pcie));
	printf("PCIE_CAP_NEXTPTR               			0x%lx\n", pcie::cap::pcie_cap_nextptr(pcie));
	printf("PCIE_CAP_CAPABILITY_ID               		0x%lx\n", pcie::cap::pcie_cap_capability_id(pcie));
	printf("PCIE_CAP_CAPABILITY_VERSION 			0x%lx\n", pcie::cap::pcie_cap_capability_version(pcie));
	printf("PCIE_CAP_DEVICE_PORT_TYPE 			0x%lx\n", pcie::cap::pcie_cap_device_port_type(pcie));
	printf("PCIE_CAP_SLOT_IMPLEMENTED  			0x%lx\n", pcie::cap::pcie_cap_slot_implemented(pcie));
	printf("---------------------------------------------------------------------\n");

	PVOID dev = get_dev(cfg);

	if (dev == 0)
	{
		return;
	}

	
	printf(
		"\n[PCI Express Device Capabilities]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("DEV_CAP_MAX_PAYLOAD_SUPPORTED 			%d\n", dev::cap::dev_cap_max_payload_supported(dev));
	printf("DEV_CAP_PHANTOM_FUNCTIONS_SUPPORT 		%ld\n", dev::cap::dev_cap_phantom_functions_support(dev));
	printf("DEV_CAP_EXT_TAG_SUPPORTED 			%ld\n", dev::cap::dev_cap_ext_tag_supported(dev));
	printf("DEV_CAP_ENDPOINT_L0S_LATENCY 			%ld\n", dev::cap::dev_cap_endpoint_l0s_latency(dev));
	printf("DEV_CAP_ENDPOINT_L1_LATENCY 			%ld\n", dev::cap::dev_cap_endpoint_l1_latency(dev));
	printf("DEV_CAP_ROLE_BASED_ERROR 			%ld\n", dev::cap::dev_cap_role_based_error(dev));
	printf("DEV_CAP_ENABLE_SLOT_PWR_LIMIT_VALUE 		%ld\n", dev::cap::dev_cap_enable_slot_pwr_limit_value(dev));
	printf("DEV_CAP_ENABLE_SLOT_PWR_LIMIT_SCALE 		%ld\n", dev::cap::dev_cap_enable_slot_pwr_limit_scale(dev));
	printf("DEV_CAP_FUNCTION_LEVEL_RESET_CAPABLE 		%ld\n", dev::cap::dev_cap_function_level_reset_capable(dev));
	printf("---------------------------------------------------------------------\n");


	printf(
		"\n[Device Control]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("Correctable Error Reporting Enable 		%ld\n", dev::ctrl::dev_ctrl_corr_err_reporting(dev));
	printf("Non-Fatal Error Reporting Enable 		%ld\n", dev::ctrl::dev_ctrl_non_fatal_reporting(dev));
	printf("Fatal Error Reporting Enable 			%ld\n", dev::ctrl::dev_ctrl_fatal_err_reporting(dev));
	printf("Unsupported Request Reporting Enable 		%ld\n", dev::ctrl::dev_ctrl_ur_reporting(dev));
	printf("Enable Relaxed Ordering 			%ld\n", dev::ctrl::dev_ctrl_relaxed_ordering(dev));
	printf("Max_Payload_Size 				%ld\n", dev::ctrl::dev_ctrl_max_payload_size(dev));
	printf("DEV_CONTROL_EXT_TAG_DEFAULT 			%ld\n", dev::ctrl::dev_ctrl_ext_tag_default(dev));
	printf("Phantom Functions Enable 			%ld\n", dev::ctrl::dev_ctrl_phantom_func_enable(dev));
	printf("Auxiliary Power PM Enable 			%ld\n", dev::ctrl::dev_ctrl_aux_power_enable(dev));
	printf("Enable No Snoop 				%ld\n", dev::ctrl::dev_ctrl_enable_no_snoop(dev));
	printf("Max_Read_Request_Size 				%ld\n", dev::ctrl::dev_ctrl_max_read_request_size(dev));
	printf("Configuration retry status enable 		%ld\n", dev::ctrl::dev_ctrl_cfg_retry_status_enable(dev));
	printf("---------------------------------------------------------------------\n");
		
	PVOID link = get_link(cfg);

	if (link == 0)
	{
		return;
	}

	printf(
		"\n[PCI Express Link Capabilities]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("LINK_CAP_MAX_LINK_SPEED 			%ld\n", link::cap::link_cap_max_link_speed(link));
	printf("LINK_CAP_MAX_LINK_WIDTH 			%ld\n", link::cap::link_cap_max_link_width(link));
	printf("LINK_CAP_ASPM_SUPPORT 				%d\n",  link::cap::link_cap_aspm_support(link));
	printf("LINK_CAP_L0S_EXIT_LATENCY 			%ld\n", link::cap::link_cap_l0s_exit_latency(link));
	printf("LINK_CAP_L1_EXIT_LATENCY 			%ld\n", link::cap::link_cap_l1_exit_latency(link));
	printf("LINK_CAP_CLOCK_POWER_MANAGEMENT 		%ld\n", link::cap::link_cap_clock_power_management(link));
	printf("LINK_CAP_ASPM_OPTIONALITY 			%ld\n", link::cap::link_cap_aspm_optionality(link));
	printf("LINK_CAP_RSVD_23 				%ld\n", link::cap::link_cap_rsvd_23(link));
	printf("---------------------------------------------------------------------\n");



	printf(
		"\n[Link Control]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("LINK_CONTROL_RCB  				%ld\n", link::ctrl::link_control_rcb(link));
	printf("---------------------------------------------------------------------\n");



	printf(
		"\n[Link Status]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("LINK_STATUS_SLOT_CLOCK_CONFIG	 		%ld\n", link::status::link_status_slot_clock_config(link));
	printf("---------------------------------------------------------------------\n");


	printf(
		"\n[PCI Express Device Capabilities 2]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("CPL_TIMEOUT_RANGES_SUPPORTED 			%ld\n", dev::cap2::cpl_timeout_disable_supported(dev));
	printf("CPL_TIMEOUT_DISABLE_SUPPORTED 			%ld\n", dev::cap2::cpl_timeout_disable_supported(dev));
	printf("---------------------------------------------------------------------\n");


	printf(
		"\n[Device Control 2]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("Completion Timeout value 			%ld\n", dev::ctrl2::completiontimeoutvalue(dev));
	printf("Completion Timeout disable 			%ld\n", dev::ctrl2::completiontimeoutdisable(dev));
	printf("---------------------------------------------------------------------\n");



	printf(
		"\n[PCI Express Link Capabilities 2]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("Link speeds supported 				%ld\n", link::cap2::linkspeedssupported(link));
	printf("---------------------------------------------------------------------\n");



	printf(
		"\n[Link Control 2]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("LINK_CTRL2_TARGET_LINK_SPEED 			%d\n",  link::ctrl2::link_ctrl2_target_link_speed(link));
	printf("LINK_CTRL2_HW_AUTONOMOUS_SPEED_DISABLE 		%ld\n", link::ctrl2::link_ctrl2_hw_autonomous_speed_disable(link));
	printf("LINK_CTRL2_DEEMPHASIS 				%ld\n", link::ctrl2::link_ctrl2_deemphasis(link));
	printf("Enter Compliance 				%ld\n", link::ctrl2::entercompliance(link));
	printf("Transmit Margin 				%ld\n", link::ctrl2::transmitmargin(link));
	printf("Enter Modified Compliance 			%ld\n", link::ctrl2::entermodifiedcompliance(link));
	printf("Compliance SOS 					%d\n",  link::ctrl2::compliancesos(link));
	printf("---------------------------------------------------------------------\n");


	printf(
		"\n[Link Status 2]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("Compliance Preset/De-emphasis 			%ld\n", link::status2::deemphasis(link));
	printf("Current De-emphasis Level 			%ld\n", link::status2::deemphasislvl(link));
	printf("Equalization Complete 				%ld\n", link::status2::equalizationcomplete(link));
	printf("Equalization Phase 1 Successful 		%ld\n", link::status2::equalizationphase1successful(link));
	printf("Equalization Phase 2 Successful 		%ld\n", link::status2::equalizationphase2successful(link));
	printf("Equalization Phase 3 Successful 		%ld\n", link::status2::equalizationphase3successful(link));
	printf("Link Equalization Request 			%ld\n", link::status2::linkequalizationrequest(link));
	printf("---------------------------------------------------------------------\n");
		
	PVOID dsn = get_dsn(cfg);

	printf(
		"\n[PCI Express Extended Capability - DSN]\n"
		"---------------------------------------------------------------------\n"
	);
	printf("DSN_CAP_NEXTPTR 				0x%lx\n", dsn::dsn_cap_nextptr(dsn));
	printf("DSN_CAP_ON 					%ld\n", dsn::dsn_cap_on(dsn));
	printf("DSN_CAP_ID 					0x0%lx\n", dsn::dsn_cap_id(dsn));
	printf("---------------------------------------------------------------------\n");
}

static void scan_pci(BOOL disable, BOOL dump_cfg, BOOL dump_bar)
{
	using namespace pci;


	std::vector<PCIE_DEVICE_INFO> devices = get_endpoint_device_list();


	if (dump_cfg)
	{
		for (auto &entry : devices)
		{
			DEVICE_INFO &dev = entry.d;
			printf("[%d:%d:%d] [%02X:%02X]", dev.bus, dev.slot, dev.func, *(WORD*)(dev.cfg), *(WORD*)(dev.cfg + 0x02));
			PrintPcieConfiguration(dev.cfg, sizeof(dev.cfg));
			printf("\n");
			filter_pci_cfg(dev.cfg);
			printf("\n");
		}
	}
	
	else if (dump_bar)
	{
		for (auto &dev : devices)
		{
			if (!GET_BIT(*(WORD*)(dev.d.cfg + 0x04), 2))
			{
				continue;
			}

			DWORD cnt=6;
			if (GET_BITS(header_type(dev.d.cfg), 6, 0) == 1)
			{
				cnt = 2;
			}

			DWORD *bar = (DWORD*)(dev.d.cfg + 0x10);
			for (DWORD i = 0; i < cnt; i++)
			{
				if (bar[i] > 0x10000000)
				{
					printf("[%d:%d:%d] [%02X:%02X]\n",
						dev.d.bus, dev.d.slot, dev.d.func, *(WORD*)(dev.d.cfg), *(WORD*)(dev.d.cfg + 0x02));
					PrintPcieBarSpace(bar[i]);
					printf("\n\n\n\n");
				}
			}
				
		}
	}
	else
	{
		test_devices(devices, disable);
	}
}

static BOOLEAN IsAddressEqual(QWORD address0, QWORD address2, INT64 cnt)
{
	INT64 res = abs(  (INT64)(address2 - address0)  );
	return res <= cnt;
}

static void scan_section(DWORD diff, CHAR *section_name, QWORD local_image, QWORD runtime_image, QWORD size, QWORD section_address)
{
	for (QWORD i = 0; i < size; i++)
	{
		if (((unsigned char*)local_image)[i] == ((unsigned char*)runtime_image)[i])
		{
			continue;
		}

		DWORD cnt = 0;
		while (1)
		{

			if (i + cnt >= size)
			{
				break;
			}

			if (((unsigned char*)local_image)[i + cnt] == ((unsigned char*)runtime_image)[i + cnt])
			{
				break;
			}

			cnt++;
		}
		if (cnt >= diff)
		{
			printf("%s:0x%llx is modified (%ld bytes): ", section_name, section_address + i, cnt);
			for (DWORD j = 0; j < cnt; j++)
			{
				PRINT_GREEN("%02X ", ((unsigned char*)local_image)[i + j]);
			}
			printf("-> ");
			for (DWORD j = 0; j < cnt; j++)
			{
				PRINT_RED("%02X ", ((unsigned char*)runtime_image)[i + j]);
			}
			printf("\n");
		}
		i += cnt;
	}
}

static void compare_sections(QWORD local_image, QWORD runtime_image, DWORD diff)
{
	QWORD image_dos_header = (QWORD)local_image;
	QWORD image_nt_header = *(DWORD*)(image_dos_header + 0x03C) + image_dos_header;
	unsigned short machine = *(WORD*)(image_nt_header + 0x4);

	QWORD section_header_off = machine == 0x8664 ?
		image_nt_header + 0x0108 :
		image_nt_header + 0x00F8;

	for (WORD i = 0; i < *(WORD*)(image_nt_header + 0x06); i++) {
		QWORD section = section_header_off + (i * 40);
		ULONG section_characteristics = *(ULONG*)(section + 0x24);

		UCHAR *section_name = (UCHAR*)(section + 0x00);
		ULONG section_virtual_address = *(ULONG*)(section + 0x0C);
		ULONG section_virtual_size = *(ULONG*)(section + 0x08);

		if (section_characteristics & 0x00000020 && !(section_characteristics & 0x02000000))
		{
			//
			// skip Warbird page
			//
			if (!strcmp((const char*)section_name, "PAGEwx3"))
			{
				continue;
			}
		
			scan_section(
				diff,
				(CHAR*)section_name,
				(QWORD)((BYTE*)local_image + section_virtual_address),
				(QWORD)(runtime_image + section_virtual_address),
				section_virtual_size,
				section_virtual_address
			);
		}
	}
}

static void scan_image(std::vector<FILE_INFO> modules, DWORD pid, FILE_INFO file, BOOL use_cache)
{
	//
	// dump image
	//
	QWORD runtime_image = (QWORD)cl::vm::dump_module(pid, file.base, DMP_FULL | DMP_RUNTIME);
	if (runtime_image == 0)
	{
		LOG_RED("failed to scan %s\n", file.path.c_str());
		return;
	}


	//
	// try to use existing memory dumps
	//
	HMODULE local_image = 0;

	if (use_cache)
	{
		local_image = (HMODULE)LoadImageEx(("./dumps/" + file.name).c_str(), 0, file.base, runtime_image);
		if (local_image == 0)
		{
			local_image = (HMODULE)LoadImageEx(file.path.c_str(), 0, file.base, runtime_image);
		}
	}
	else
	{
		const char *sub_str = strstr(file.path.c_str(), "\\dump_");

		if (sub_str)
		{
			std::string sub_name = sub_str + 6;
			std::string resolved_path;

			for (auto &lookup : modules)
			{
				if (!_strcmpi(lookup.name.c_str(), sub_name.c_str()))
				{
					resolved_path = lookup.path;
				}
			}

			if (resolved_path.size() < 1)
			{
				resolved_path = "C:\\Windows\\System32\\Drivers\\" + sub_name;
			}

			file.path = resolved_path;
		}

		local_image = (HMODULE)LoadImageEx(file.path.c_str(), 0, file.base, runtime_image);
	}

	if (local_image == 0)
	{
		LOG_RED("failed to scan %s\n", file.path.c_str());
		cl::vm::free_module((PVOID)runtime_image);
		return;
	}

	QWORD image_dos_header = (QWORD)local_image;
	QWORD image_nt_header = *(DWORD*)(image_dos_header + 0x03C) + image_dos_header;
	unsigned short machine = *(WORD*)(image_nt_header + 0x4);

	DWORD min_difference = 1;

	LOG("scanning: %s\n", file.path.c_str());

	compare_sections((QWORD)local_image, runtime_image, min_difference);

	cl::vm::free_module((PVOID)runtime_image);

	FreeImageEx((void *)local_image);
}

static BOOL write_dump_file(std::string name, PVOID buffer, QWORD size)
{
	if (CreateDirectoryA("./dumps/", NULL) || ERROR_ALREADY_EXISTS == GetLastError())
	{
		std::string path = "./dumps/" + name;
		FILE* f = fopen(path.c_str(), "wb");

		if (f)
		{
			fwrite(buffer, size, 1, f);

			fclose(f);

			return 1;
		}
	}

	return 0;
}

static BOOL dump_module_to_file(std::vector<FILE_INFO> modules, DWORD pid, FILE_INFO file)
{
	const char *sub_str = strstr(file.path.c_str(), "\\dump_");

	if (sub_str)
	{
		std::string sub_name = sub_str + 6;
		std::string resolved_path;

		for (auto &lookup : modules)
		{
			if (!_strcmpi(lookup.name.c_str(), sub_name.c_str()))
			{
				resolved_path = lookup.path;
			}
		}

		if (resolved_path.size() < 1)
		{
			resolved_path = "C:\\Windows\\System32\\Drivers\\" + sub_name;
		}

		file.path = resolved_path;
	}

	PVOID disk_base = (PVOID)LoadFileEx(file.path.c_str(), 0);
	if (disk_base == 0)
	{
		return 0;
	}

	QWORD target_base = (QWORD)cl::vm::dump_module(pid, file.base, DMP_FULL | DMP_RAW);
	if (target_base == 0)
	{
		free(disk_base);
		return FALSE;
	}

	//
	// copy discardable sections from disk
	//
	QWORD disk_nt = (QWORD)pe::get_nt_headers((QWORD)disk_base);
	PIMAGE_SECTION_HEADER section_disk = pe::nt::get_image_sections(disk_nt);
	for (WORD i = 0; i < pe::nt::get_section_count(disk_nt); i++)
	{
		if (section_disk[i].SizeOfRawData)
		{
			if ((section_disk[i].Characteristics & 0x02000000))
			{
				memcpy(
					(void*)(target_base + section_disk[i].PointerToRawData),
					(void*)((QWORD)disk_base + section_disk[i].PointerToRawData),
					section_disk[i].SizeOfRawData
				);
			}
		}
	}

	//
	// free disk base
	//
	free(disk_base);

	//
	// write dump file to /dumps/modulename
	//
	BOOL status = write_dump_file(file.name.c_str(), (PVOID)target_base, *(QWORD*)(target_base - 16 + 8));

	if (status)
		LOG("module %s is succesfully cached\n", file.name.c_str());
	cl::vm::free_module((PVOID)target_base);

	return status;
}

static BOOL unlink_detection(
	std::vector<EFI_PAGE_TABLE_ALLOCATION>& page_table_list,
	std::vector<EFI_MEMORY_DESCRIPTOR>& memory_map,
	EFI_PAGE_TABLE_ALLOCATION *out
	)
{
	BOOL status = 0;
	for (auto& ptentry : page_table_list)
	{
		BOOL found = 0;

		for (auto& mmentry : memory_map)
		{
			if (ptentry.PhysicalStart >= mmentry.PhysicalStart && ptentry.PhysicalStart <= (mmentry.PhysicalStart + (mmentry.NumberOfPages * 0x1000)))
			{
				found = 1;
				break;
			}
		}

		if (!found)
		{
			printf("\n");
			LOG("unlinked page allocation!!! [%llx - %llx]\n",
				ptentry.PhysicalStart,
				ptentry.PhysicalStart + (ptentry.NumberOfPages * 0x1000)
			);
			*out = ptentry;
			status = 1;
		}
	}

	return status;
}

static BOOL invalid_range_detection(
	std::vector<EFI_MEMORY_DESCRIPTOR>& memory_map,
	EFI_PAGE_TABLE_ALLOCATION& dxe_range,
	EFI_MEMORY_DESCRIPTOR *out
	)
{
	BOOL status=0;
	for (auto& entry : memory_map)
	{
		if (entry.PhysicalStart >= dxe_range.PhysicalStart &&
			(entry.PhysicalStart + (entry.NumberOfPages * 0x1000)) <=
			(dxe_range.PhysicalStart + (dxe_range.NumberOfPages * 0x1000))
			)
		{
			continue;
		}

		if ((entry.Type == 5 || entry.Type == 6) && entry.Attribute == 0x800000000000000f &&
			entry.PhysicalStart > dxe_range.PhysicalStart)
		{
			printf("\n");
			LOG("DXE is found from invalid range!!! [%llx - %llx] 0x%llx\n",
				entry.PhysicalStart,
				entry.PhysicalStart + (entry.NumberOfPages * 0x1000),
				entry.VirtualStart
			);

			*out   = entry;
			status = 1;
		}
	}

	return status;
}

static void scan_runtime(std::vector<EFI_MODULE_INFO> &dxe_modules)
{
	std::vector<QWORD> HalEfiRuntimeServicesTable = cl::efi::get_runtime_table();
	if (!HalEfiRuntimeServicesTable.size())
	{
		return;
	}

	for (int i = 0; i < HalEfiRuntimeServicesTable.size(); i++)
	{
		QWORD rt_func = HalEfiRuntimeServicesTable[i];
		if (cl::vm::read<WORD>(4, rt_func) == 0x25ff)
		{
			LOG_RED("EFI Runtime service [%d] is hooked with byte patch: %llx\n", i, rt_func);
			continue;
		}

		QWORD physical_address = cl::get_physical_address(rt_func);
		BOOL found = 0;
		for (auto& base : dxe_modules)
		{
			if (physical_address >= (QWORD)base.physical_address && physical_address <= (QWORD)((QWORD)base.physical_address + base.size))
			{
				found = 1;
				break;
			}
		}

		if (!found)
		{
			LOG_RED("EFI Runtime service [%d] is hooked with pointer swap: %llx, %llx\n", i, rt_func, physical_address);
		}
	}
}

static void dump_to_file(PCSTR filename, QWORD physical_address, QWORD size)
{
	LOG("dumping out: [%llX - %llX]\n", physical_address, physical_address + size);
	PVOID buffer = malloc(size);
	cl::io::read(physical_address, buffer, size);
	if (*(WORD*)(buffer) == IMAGE_DOS_SIGNATURE)
	{
		QWORD nt = pe::get_nt_headers((QWORD)buffer);
		PIMAGE_SECTION_HEADER section = pe::nt::get_image_sections(nt);
		for (WORD i = 0; i < pe::nt::get_section_count(nt); i++)
		{
			section[i].PointerToRawData = section[i].VirtualAddress;
			section[i].SizeOfRawData    = section[i].Misc.VirtualSize;
		}
	}
	FILE *f = fopen(filename, "wb");
	fwrite(buffer, size, 1, f);
	fclose(f);
	free(buffer);
}

static void scan_efi(BOOL dump)
{
	std::vector<EFI_MEMORY_DESCRIPTOR> memory_map = cl::efi::get_memory_map();
	if (!memory_map.size())
	{
		return;
	}

	std::vector<EFI_MODULE_INFO> dxe_modules = cl::efi::get_dxe_modules(memory_map);
	if (!dxe_modules.size())
	{
		return;
	}

	std::vector<EFI_PAGE_TABLE_ALLOCATION> table_allocations = cl::efi::get_page_table_allocations();
	if (!table_allocations.size())
	{
		return;
	}

	EFI_PAGE_TABLE_ALLOCATION dxe_range = cl::efi::get_dxe_range(dxe_modules[0], table_allocations) ;
	if (dxe_range.PhysicalStart == 0)
	{
		return;
	}
	
	//
	// print everything
	//
	for (auto &entry : memory_map)
	{
		LOG("0x%llx, %lld [%llx - %llx] 0x%llx\n",
			entry.Attribute,
			entry.Type,
			entry.PhysicalStart,
			entry.PhysicalStart + (entry.NumberOfPages * 0x1000),
			entry.VirtualStart
		);
	}

	scan_runtime(dxe_modules);

	EFI_MEMORY_DESCRIPTOR eout_0{};
	if (invalid_range_detection(memory_map, dxe_range, &eout_0) && dump)
	{
		dump_to_file("eout_0.bin", eout_0.PhysicalStart, eout_0.NumberOfPages*0x1000);
	}
	EFI_PAGE_TABLE_ALLOCATION eout_1{};
	if (unlink_detection(table_allocations, memory_map, &eout_1) && dump)
	{
		dump_to_file("eout_1.bin", eout_1.PhysicalStart, eout_1.NumberOfPages*0x1000);
	}

	//
	// later runtime checks
	// 
	// if (is_efi_address(rip) && !is_inside(rip, dxe_range))
	//	printf("wssu doing m8???\n");
	//
}

