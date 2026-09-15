-- Night Watch: a Tier-3 example mod.
--
-- A script's top level runs once, when the world loads. Use it to register
-- handlers and commands; do world work inside handlers, when the world and the
-- survivor exist. Everything the game offers is in the `madfall` table - see
-- MODDING.md, "Tier 3: scripts".

local BOUNTY_EVERY = 25

-- The store is saved with the world. Values are nil, booleans, numbers or strings.
local function bump(key, by)
	local value = (madfall.store_get(key) or 0) + (by or 1)
	madfall.store_set(key, value)
	return value
end

madfall.on("world_loaded", function(event)
	local sessions = bump("sessions")
	madfall.log("Night Watch ready: session " .. sessions .. ", day " .. event.day)
end)

madfall.on("zombie_killed", function(event)
	local kills = bump("kills")
	if kills % BOUNTY_EVERY == 0 then
		madfall.give("madfall:arrow", 10)
		madfall.message("Night Watch bounty: " .. kills .. " kills. Here are 10 arrows.")
	end
end)

madfall.on("block_broken", function(event)
	bump("mined")
end)

madfall.on("dusk", function(event)
	if event.horde then
		madfall.message("Horde night. Hold the line until dawn.")
	end
end)

madfall.on("horde_night", function(event)
	madfall.store_set("horde_pending", true)
end)

madfall.on("dawn", function(event)
	local nights = bump("nights")
	if madfall.store_get("horde_pending") then
		madfall.store_set("horde_pending", nil)
		local player = madfall.player()
		if player and player.alive then
			madfall.give("madfall:bandage", 2)
			madfall.give("madfall:canned_food", 1)
			madfall.message("You held the line. Supplies have arrived.")
		end
	end
	madfall.log("dawn of day " .. event.day .. " (" .. nights .. " nights watched)")
end)

-- mod.example_scripted.stats
madfall.command("stats", function(args)
	local text = string.format("Night Watch: %d kills, %d blocks mined, %d nights, %d sessions",
		madfall.store_get("kills") or 0, madfall.store_get("mined") or 0,
		madfall.store_get("nights") or 0, madfall.store_get("sessions") or 0)
	madfall.log(text)
	madfall.message(text)
end)

-- mod.example_scripted.beacon [height]: a wooden pillar with a torch on top, beside the survivor.
madfall.command("beacon", function(args)
	local player = madfall.player()
	if not player then
		madfall.log("beacon: no survivor in the world")
		return
	end
	local height = math.max(1, math.min(tonumber(args[1]) or 4, 16))
	local x, y = player.x + 2, player.y
	local placed = 0
	for z = player.z, player.z + height - 1 do
		if madfall.set_block(x, y, z, "madfall:wood_frame") then
			placed = placed + 1
		end
	end
	if madfall.set_block(x, y, player.z + height, "madfall:torch") then
		placed = placed + 1
	end
	madfall.log(string.format("beacon: placed %d block(s) at %d %d %d", placed, x, y, player.z))
end)

-- mod.example_scripted.selftest: checks the sandbox from the inside.
madfall.command("selftest", function(args)
	local failures = {}
	local function expect(condition, what)
		if not condition then
			failures[#failures + 1] = what
		end
	end
	expect(io == nil, "io is reachable")
	expect(os == nil, "os is reachable")
	expect(package == nil and require == nil, "package is reachable")
	expect(debug == nil, "debug is reachable")
	expect(load == nil and loadfile == nil and dofile == nil, "chunk loading is reachable")
	expect(string.dump == nil, "string.dump is reachable")
	expect(madfall.api_version == 1, "unexpected api_version")
	expect(madfall.mod_id == "example_scripted", "unexpected mod_id")

	local ok = pcall(madfall.on, "no_such_event", function() end)
	expect(not ok, "an unknown event was accepted")

	local day, hour = madfall.time()
	expect(type(day) == "number" and hour >= 0 and hour <= 24, "time() is wrong")

	if #failures == 0 then
		madfall.log("selftest passed")
	else
		madfall.log("selftest FAILED: " .. table.concat(failures, "; "))
	end
end)
