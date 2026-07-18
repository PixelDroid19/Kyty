local guest_root = arg[1]
local out_dir = arg[2] or '_scratch_playable/symbol_dump'

if guest_root == nil or guest_root == '' then
	error('usage: fc_script scripts/dump_guest_symbols.lua <guest-root> [out-dir]')
end

local function env_or(name, fallback)
	local value = os.getenv(name)
	if value == nil or value == '' then
		return fallback
	end
	return value
end

local function env_bool(name, fallback)
	local value = os.getenv(name)
	if value == nil or value == '' then
		return fallback
	end
	return value == '1' or value == 'true' or value == 'TRUE'
end

local function env_number(name, fallback)
	local value = tonumber(os.getenv(name) or '')
	return value or fallback
end

kyty_init({
	ScreenWidth = env_number('KYTY_SCREEN_WIDTH', 1280);
	ScreenHeight = env_number('KYTY_SCREEN_HEIGHT', 720);
	Neo = true;
	VulkanValidationEnabled = env_bool('KYTY_VULKAN_VALIDATION', false);
	ShaderValidationEnabled = env_bool('KYTY_SHADER_VALIDATION', false);
	ShaderOptimizationType = env_or('KYTY_SHADER_OPTIMIZATION', 'Performance');
	ShaderLogDirection = 'Silent';
	ShaderLogFolder = '_Shaders';
	CommandBufferDumpEnabled = false;
	CommandBufferDumpFolder = '_Buffers';
	PrintfDirection = 'Silent';
	PrintfOutputFolder = '_Logs';
	ProfilerDirection = 'None';
	PipelineDumpEnabled = false;
	PipelineDumpFolder = '_Pipelines';
})

kyty_mount(guest_root, '/app0')
kyty_load_param_json(guest_root .. '/sce_sys/param.json')
kyty_load_elf('/app0/eboot.bin')
kyty_load_symbols_all()
kyty_dbg_dump_symbols(out_dir)
