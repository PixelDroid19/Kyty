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
