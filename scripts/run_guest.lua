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
	PrintfDirection = 'Console';
	ProfilerDirection = 'None';
}

kyty_init(cfg)
kyty_mount(guest_root, '/app0')
kyty_load_elf('/app0/eboot.bin')

for _, module in ipairs({
	'libc_1', 'libc_internal_1', 'libkernel_1', 'libVideoOut_1',
	'libSysmodule_1', 'libDiscMap_1', 'libGraphicsDriver_1', 'libPad_1',
	'libAudio_1', 'libUserService_1', 'libSystemService_1',
	'libAppContent_1', 'libSaveData_1', 'libDialog_1', 'libNet_1',
	'libPlayGo_1'
}) do
	kyty_load_symbols(module)
end

kyty_execute()
