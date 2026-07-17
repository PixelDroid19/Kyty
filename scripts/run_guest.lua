local guest_root = arg[1]
if guest_root == nil or guest_root == '' then
	error('usage: fc_script scripts/run_guest.lua <guest-root>')
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

local cfg = {
	ScreenWidth = env_number('KYTY_SCREEN_WIDTH', 1280);
	ScreenHeight = env_number('KYTY_SCREEN_HEIGHT', 720);
	Neo = true;
	VulkanValidationEnabled = env_bool('KYTY_VULKAN_VALIDATION', false);
	ShaderValidationEnabled = env_bool('KYTY_SHADER_VALIDATION', false);
	ShaderOptimizationType = env_or('KYTY_SHADER_OPTIMIZATION', 'Performance');
	ShaderLogDirection = env_or('KYTY_SHADER_LOG_DIRECTION', 'Silent');
	ShaderLogFolder = env_or('KYTY_SHADER_LOG_FOLDER', '_Shaders');
	CommandBufferDumpEnabled = env_bool('KYTY_COMMAND_BUFFER_DUMP', false);
	CommandBufferDumpFolder = env_or('KYTY_COMMAND_BUFFER_DUMP_FOLDER', '_Buffers');
	-- Silent is required for usable host FPS and host resource safety.
	-- Console/File HLE logging can drop a Release build from ~40+ FPS to ~1 FPS
	-- and grow multi‑hundred‑MB logs on a full title boot. Dumps and verbose
	-- logging stay opt-in so end users are not surprised by disk/RAM pressure.
	PrintfDirection = env_or('KYTY_PRINTF_DIRECTION', 'Silent');
	PrintfOutputFolder = env_or('KYTY_PRINTF_OUTPUT_FOLDER', '_Logs');
	ProfilerDirection = env_or('KYTY_PROFILER_DIRECTION', 'None');
	PipelineDumpEnabled = env_bool('KYTY_PIPELINE_DUMP', false);
	PipelineDumpFolder = env_or('KYTY_PIPELINE_DUMP_FOLDER', '_Pipelines');
}

kyty_init(cfg)
kyty_mount(guest_root, '/app0')
kyty_load_param_json(guest_root .. '/sce_sys/param.json')
kyty_load_elf('/app0/eboot.bin')

kyty_load_symbols_all()

kyty_execute()
