local guest_root = arg[1]
if guest_root == nil or guest_root == '' then
	error('usage: fc_script scripts/run_guest.lua <guest-root>')
end

-- Strict acceptance runs must not set any KYTY_BRINGUP_* variable. Unsafe
-- bring-up is diagnostic only and never counts as compatibility evidence.
-- Authorized smoke: KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1 plus explicit mode=unsafe.
local function env(name)
	return os.getenv(name)
end

local bringup_keys = {
	'KYTY_BRINGUP_MODE',
	'KYTY_BRINGUP_FEATURES',
	'KYTY_BRINGUP_SUBSYSTEMS',
	'KYTY_BRINGUP_BURST_LIMIT',
	'KYTY_BRINGUP_BURST_WINDOW_MS',
}

local has_bringup = false
for _, k in ipairs(bringup_keys) do
	if env(k) ~= nil then
		has_bringup = true
		break
	end
end

if has_bringup then
	if env('KYTY_BRINGUP_ALLOW_DIAGNOSTIC') ~= '1' then
		error(
			'strict capture rejects KYTY_BRINGUP_* (set KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1 only for authorized diagnostic smoke; never as acceptance)')
	end
	print('WARNING: KYTY_BRINGUP_* is active — diagnostic only, not compatibility evidence')
end

-- Legacy vars were removed; refuse them so old scripts fail loudly.
if env('KYTY_STUB_MISSING') ~= nil or env('KYTY_GFX_PERMISSIVE') ~= nil then
	error('KYTY_STUB_MISSING and KYTY_GFX_PERMISSIVE are removed; use KYTY_BRINGUP_MODE=unsafe with features')
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
