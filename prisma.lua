-- Prisma keypad configuration
--
-- This is a real Lua script, run once at startup. Prisma-specific
-- functions live under the "prisma" module table:
--
--   prisma.image(key, path)          upload an image to a key right now
--   prisma.image(key, path, r, g, b) same, but tint SVG input by alpha
--                                    mask (for "symbolic" icon sets)
--   prisma.blank(key)                upload a solid black image to a key
--   prisma.color(key, r, g, b)       upload a synthesized solid-color square
--   prisma.brightness(percent)       set screen brightness (0-100)
--   prisma.exec(cmd)                 run a shell command, fire-and-forget
--   prisma.capture(cmd)              run a shell command, block, return stdout
--   prisma.on_press(key, function)   register a handler to run when key is pressed
--   prisma.every(seconds, function)  register a repeating timer (seconds may
--                                    be fractional, e.g. prisma.every(0.1, f))
--
-- Keys are numbered 1-15, matching the physical 3x5 grid printed on the
-- device (not 0-14).

ICONS = "/usr/share/icons/Adwaita/"
SYMBOLIC_STATUS_ICONS = ICONS .. "symbolic/status/"

-- POSIX-shell-safe single-quoting for any value that gets spliced into a
-- prisma.exec()/prisma.capture() command string. MANDATORY for anything
-- that isn't a literal we wrote ourselves -- e.g. MPRIS title/artist/
-- artUrl below come from whatever web page Firefox happens to be
-- playing media from (set via the page's own JS MediaSession API), so
-- they're attacker-controlled strings. Without this, a page setting
-- mediaSession.metadata.title to something like `"; rm -rf ~ ; echo "`
-- would get arbitrary shell execution the next time this fires.
local function shell_quote(s)
	return "'" .. s:gsub("'", "'\\''") .. "'"
end

prisma.brightness(80)

-- No playerctl on this box -- talk to MPRIS directly over D-Bus via
-- busctl+jq instead. Any MPRIS-compliant player works this way,
-- including Firefox's built-in media-session shim (org.mpris.MediaPlayer2.firefox.*),
-- so this covers web audio/video in a browser tab, not just native
-- players.
local function mpris_bus()
	return prisma.capture([[busctl --user list --no-legend 2>/dev/null | awk '{print $1}' | grep '^org\.mpris\.MediaPlayer2\.' | head -1]])
end

local function mpris_metadata_field(bus, jq_filter)
	if bus == "" then
		return ""
	end
	return prisma.capture('busctl --user -j get-property "' .. bus ..
		'" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Metadata 2>/dev/null | jq -r \'' .. jq_filter .. '\'')
end

-- Synchronous (capture, not exec) so callers can query fresh state
-- immediately afterward without racing an async D-Bus call -- same
-- reasoning as the mute buttons chaining toggle+query into one
-- capture() below.
local function mpris_call(bus, method)
	if bus == "" then
		return
	end
	prisma.capture('busctl --user call "' .. bus .. '" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player ' .. method)
end

-- Shows the current MPRIS player's album/track art. mpris:artUrl can be
-- file://, http(s)://, or missing entirely -- curl handles file:// and
-- http(s):// uniformly, and since art isn't guaranteed to be png/jpg (or
-- to exist at all), sniff the downloaded bytes with `file` rather than
-- trusting any extension in the URL, and just leave the last-known art
-- up if there's nothing playing or the fetch fails.
local ART_TMP_BASE = "/tmp/prisma_mpris_art"

local function update_now_playing_art()
	local bus = mpris_bus()
	local art_url = mpris_metadata_field(bus, '.data."mpris:artUrl".data // empty')
	if art_url == "" then
		return
	end

	local raw = ART_TMP_BASE .. ".raw"
	local ok = prisma.capture('curl -sL -m 3 ' .. shell_quote(art_url) .. ' -o ' .. raw .. ' 2>/dev/null && echo OK')
	if ok ~= "OK" then
		return
	end

	local mime = prisma.capture("file -b --mime-type " .. raw)
	local ext
	if mime == "image/png" then
		ext = "png"
	elseif mime == "image/jpeg" then
		ext = "jpg"
	else
		return -- unsupported art format, leave whatever was there before
	end

	local named = ART_TMP_BASE .. "." .. ext
	prisma.capture("cp " .. raw .. " " .. named)
	prisma.image(2, named)
end

prisma.on_press(2, function()
	local bus = mpris_bus()
	local title = mpris_metadata_field(bus, '.data."xesam:title".data // empty')
	local artist = mpris_metadata_field(bus, '(.data."xesam:artist".data // [""]) | join(", ")')
	prisma.exec('notify-send "Now Playing" ' .. shell_quote(artist .. ' - ' .. title))
	update_now_playing_art()
end)

update_now_playing_art()
prisma.every(15, update_now_playing_art)

-- No combined "play-pause" icon exists in Adwaita's symbolic set (only
-- separate start/pause), so show whichever icon reflects what pressing
-- the button will actually do next, driven by MPRIS PlaybackStatus.
local function mpris_playback_status(bus)
	if bus == "" then
		return ""
	end
	return prisma.capture('busctl --user -j get-property "' .. bus ..
		'" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player PlaybackStatus 2>/dev/null | jq -r .data')
end

local function update_playpause_icon()
	local status = mpris_playback_status(mpris_bus())
	local icon = status == "Playing" and "media-playback-pause-symbolic.svg" or "media-playback-start-symbolic.svg"
	prisma.image(3, ICONS .. "symbolic/actions/" .. icon, 255, 255, 255)
end

update_playpause_icon()
prisma.every(5, update_playpause_icon)

prisma.on_press(3, function()
	mpris_call(mpris_bus(), "PlayPause")
	update_playpause_icon()
end)

local SINK = "@DEFAULT_AUDIO_SINK@"
local SOURCE = "@DEFAULT_AUDIO_SOURCE@"

-- Muted = red, unmuted = green, on top of the same tint-by-alpha-mask
-- mechanism used for weather icons above.
local MUTED_COLOR = {220, 40, 40}
local UNMUTED_COLOR = {40, 200, 40}

local function set_speaker_icon(vol)
	local muted = vol:find("MUTED") ~= nil
	local icon = muted and "audio-volume-muted-symbolic.svg" or "audio-volume-high-symbolic.svg"
	local c = muted and MUTED_COLOR or UNMUTED_COLOR
	prisma.image(4, SYMBOLIC_STATUS_ICONS .. "/" .. icon, c[1], c[2], c[3])
end

local function set_mic_icon(vol)
	local muted = vol:find("MUTED") ~= nil
	local icon = muted and "microphone-sensitivity-muted-symbolic.svg" or "microphone-sensitivity-high-symbolic.svg"
	local c = muted and MUTED_COLOR or UNMUTED_COLOR
	prisma.image(5, SYMBOLIC_STATUS_ICONS .. "/" .. icon, c[1], c[2], c[3])
end

set_speaker_icon(prisma.capture("wpctl get-volume " .. SINK))
prisma.on_press(4, function()
	-- Chained into one synchronous capture() so the toggle is guaranteed
	-- to land before we query state -- exec() is fire-and-forget, so a
	-- separate exec() + capture() pair races: capture() can run its
	-- get-volume before the async toggle actually applies.
	set_speaker_icon(prisma.capture("wpctl set-mute " .. SINK .. " toggle && wpctl get-volume " .. SINK))
end)

set_mic_icon(prisma.capture("wpctl get-volume " .. SOURCE))
prisma.on_press(5, function()
	set_mic_icon(prisma.capture("wpctl set-mute " .. SOURCE .. " toggle && wpctl get-volume " .. SOURCE))
end)

-- Sits directly above the shutdown button (11) on the grid.
prisma.image(6, SYMBOLIC_STATUS_ICONS .. "system-lock-screen-symbolic.svg", 255, 255, 255)
prisma.on_press(6, function()
	prisma.exec("xscreensaver-command -lock")
end)

prisma.on_press(7, function()
	prisma.exec("wpctl set-volume " .. SINK .. " 5%-")
end)

prisma.on_press(8, function()
	prisma.exec("amixer set Master toggle")
end)

prisma.on_press(9, function()
	prisma.exec("brightnessctl set 80%")
end)

-- Dedicated volume up/down keys with icons. wpctl's -l caps the final
-- volume at 120% so repeated presses can't push it into
-- speaker-damaging/distorted territory.
prisma.image(10, SYMBOLIC_STATUS_ICONS .. "audio-volume-high-symbolic.svg", 255, 255, 255)
prisma.on_press(10, function()
	prisma.exec("wpctl set-volume -l 1.2 " .. SINK .. " 5%+")
end)

-- Requires 3 successive presses within CONFIRM_WINDOW seconds to actually
-- suspend, so a stray/accidental tap doesn't kill the session -- each
-- partial press nudges the icon color (white -> yellow -> orange) as
-- feedback, and the counter resets if too much time passes between
-- presses or after it fires.
local SHUTDOWN_ICON = ICONS .. "symbolic/actions/system-shutdown-symbolic.svg"
local SHUTDOWN_CONFIRM_PRESSES = 3
local SHUTDOWN_CONFIRM_WINDOW = 3 -- seconds
local shutdown_presses = 0
local shutdown_last_press = 0
local shutdown_colors = {
	{255, 255, 255}, -- 0 presses so far (idle)
	{255, 220, 0},   -- 1 press
	{255, 140, 0},   -- 2 presses
}

prisma.image(11, SHUTDOWN_ICON, shutdown_colors[1][1], shutdown_colors[1][2], shutdown_colors[1][3])
prisma.on_press(11, function()
	local now = os.time()
	if now - shutdown_last_press > SHUTDOWN_CONFIRM_WINDOW then
		shutdown_presses = 0
	end
	shutdown_last_press = now
	shutdown_presses = shutdown_presses + 1

	if shutdown_presses >= SHUTDOWN_CONFIRM_PRESSES then
		shutdown_presses = 0
		prisma.image(11, SHUTDOWN_ICON, 255, 0, 0)
		prisma.exec("systemctl suspend")
	else
		local c = shutdown_colors[shutdown_presses + 1]
		prisma.image(11, SHUTDOWN_ICON, c[1], c[2], c[3])
	end
end)

-- The counter above already resets itself lazily on the *next* press
-- once the window has elapsed, but without this the icon would stay
-- stuck on yellow/orange indefinitely after an abandoned partial
-- sequence -- looking "armed" when it isn't. Poll once a second and
-- snap it back to idle white if it's been sitting untouched.
prisma.every(1, function()
	if shutdown_presses > 0 and os.time() - shutdown_last_press > SHUTDOWN_CONFIRM_WINDOW then
		shutdown_presses = 0
		local c = shutdown_colors[1]
		prisma.image(11, SHUTDOWN_ICON, c[1], c[2], c[3])
	end
end)

prisma.on_press(12, function()
	prisma.exec("scrot -s")
end)

local function weather_icon(weather)
	local dir = ICONS .. "/symbolic/status/"

	if weather:find("Sunny") or weather:find("Clear") then
		return dir .. "weather-clear-symbolic.svg"
	elseif weather:find("Partly cloudy") then
		return dir .. "weather-few-clouds-symbolic.svg"
	elseif weather:find("Cloudy") or weather:find("Overcast") then
		return dir .. "weather-overcast-symbolic.svg"
	elseif weather:find("Thundery") or weather:find("Storm") or weather:find("Thunder") then
		return dir .. "weather-storm-symbolic.svg"
	elseif weather:find("Snow") or weather:find("Ice") or weather:find("Sleet") or weather:find("Blizzard") then
		return dir .. "weather-snow-symbolic.svg"
	elseif weather:find("Rain") or weather:find("Drizzle") or weather:find("Shower") then
		return dir .. "weather-showers-symbolic.svg"
	elseif weather:find("Fog") or weather:find("Mist") or weather:find("Haze") then
		return dir .. "weather-fog-symbolic.svg"
	else
		return dir .. "weather-overcast-symbolic.svg"
	end
end

local function update_weather()
	-- -m 3: prisma.capture() blocks the entire single-threaded event
	-- loop (including button responsiveness) until the command
	-- finishes, so an unbounded curl on a flaky/dead network would hang
	-- the whole daemon, not just this one update.
	local weather = prisma.capture([[curl -s -m 3 "wttr.in/?format=j2" | jq -r '.current_condition[0].weatherDesc[0].value' 2>/dev/null]])
	if weather == "" then
		return
	end
	-- Adwaita's "symbolic" SVGs are hardcoded to fill="#222222"
	-- (near-black), meant for theme-engine recoloring, not literal
	-- rendering -- tint white so it actually shows up against the
	-- key's black background.
	prisma.image(13, weather_icon(weather), 255, 255, 255)
end

update_weather()
prisma.every(1800, update_weather)

prisma.on_press(14, function()
	prisma.exec("brightnessctl set 40%")
end)

-- Dedicated volume up/down keys with icons.
prisma.image(15, SYMBOLIC_STATUS_ICONS .. "audio-volume-low-symbolic.svg", 255, 255, 255)
prisma.on_press(15, function()
	prisma.exec("wpctl set-volume " .. SINK .. " 5%-")
end)
