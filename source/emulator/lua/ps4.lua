
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

kyty_mount('z:/dev/ps4/tests/01_Hello_8/', '/app0');

kyty_load_elf('/app0/main.elf');
kyty_load_elf('/app0/sce_module/libc.prx', 0);
kyty_load_elf('/app0/sce_module/libSceFios2.prx', 0);

kyty_load_symbols('libAudio_1');
kyty_load_symbols('libc_internal_1');
kyty_load_symbols('libDebug_1');
kyty_load_symbols('libDialog_1');
kyty_load_symbols('libDiscMap_1');
kyty_load_symbols('libGraphicsDriver_1');
kyty_load_symbols('libkernel_1');
kyty_load_symbols('libNet_1');
kyty_load_symbols('libPad_1');
kyty_load_symbols('libPlayGo_1');
kyty_load_symbols('libSysmodule_1');
kyty_load_symbols('libSystemService_1');
kyty_load_symbols('libUserService_1');
kyty_load_symbols('libVideoOut_1');

kyty_execute();
