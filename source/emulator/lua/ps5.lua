
local cfg = {
	ScreenWidth = 1280;
	ScreenHeight = 720;
	InternalResolutionWidth = 1280;
	InternalResolutionHeight = 720;
	Neo = true;
	VulkanValidationEnabled = false;
	ShaderValidationEnabled = false;
	ShaderOptimizationType = 'Performance'; -- None, Size, Performance
	-- Diagnostics are opt-in: File/Console logging and buffer dumps can fill
	-- host disk and stall FPS. Enable only while debugging a specific path.
	ShaderLogDirection = 'Silent'; -- Silent, Console, File
	ShaderLogFolder = '_Shaders';
	CommandBufferDumpEnabled = false;
	CommandBufferDumpFolder = '_Buffers';
	PrintfDirection = 'Silent'; -- Silent, Console, File
	PrintfOutputFile = '_kyty.txt';
	ProfilerDirection = 'None'; -- None, File, Network, FileAndNetwork
	ProfilerOutputFile = '_profile.prof';
}

kyty_init(cfg);

kyty_mount('z:/dev/ps5/tests/01_Hello/Debug', '/app0');

kyty_load_elf('/app0/main.elf');
kyty_load_elf('/app0/sce_module/libc.prx');

kyty_load_symbols('libc_internal_1');
kyty_load_symbols('libkernel_1');
kyty_load_symbols('libVideoOut_1');
kyty_load_symbols('libSysmodule_1');
kyty_load_symbols('libDiscMap_1');

--kyty_dbg_dump('_elf/'); -- opt-in only; can write multi‑hundred MB ELF dumps

kyty_execute();
