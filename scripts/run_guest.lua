local guest_root = arg[1]
if guest_root == nil or guest_root == '' then
	error('usage: fc_script scripts/run_guest.lua <guest-root>')
end

local cfg = {
	ScreenWidth = 1280;
	ScreenHeight = 720;
	Neo = true;
	VulkanValidationEnabled = false;
	ShaderValidationEnabled = false;
	ShaderOptimizationType = 'Performance';
	ShaderLogDirection = 'Silent';
	CommandBufferDumpEnabled = false;
	-- Silent is required for usable host FPS. Console HLE logging can drop a
	-- Release build from ~40+ FPS to ~1 FPS on the same host/GPU. Use Console
	-- only when debugging a specific guest call sequence.
	PrintfDirection = 'Silent';
	ProfilerDirection = 'None';
}

kyty_init(cfg)
kyty_mount(guest_root, '/app0')
kyty_load_elf('/app0/eboot.bin')

kyty_load_symbols_all()

kyty_execute()
