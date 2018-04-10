-- This is a simple example of an echo test application built in Lua,
-- and conceived to be used in conjunction with the janus_lua.c plugin
--
-- Note: this example depends on lua-json to do JSON processing
-- (http://luaforge.net/projects/luajson/)
json = require('json')
-- We also import our own SDP helper utilities: you may have better ones
sdp = require('janus-sdp')
-- Let's also use our ugly stdout logger just for the fun of it: to add
-- some color to the text we use the ansicolors library
-- (https://github.com/kikito/ansicolors.lua)
colors = require "ansicolors"
logger = require('janus-logger')

-- Example details
name = "recordplay.lua"
logger.prefix(colors("[%{blue}" .. name .. "%{reset}]"))
logger.print("Loading...")

-- State and properties
sessions = {}
tasks = {}

-- Methods
function init(config)
	-- This is where we initialize the plugin, for static properties
	logger.print("Initialized")
end

function destroy()
	-- This is where we deinitialize the plugin, when Janus shuts down
	logger.print("Deinitialized")
end

function createSession(id)
	-- Keep track of a new session
	logger.print("Created new session: " .. id)
	sessions[id] = { id = id, lua = name }
end

function destroySession(id)
	-- A Janus plugin session has gone
	logger.print("Destroyed session: " .. id)
	hangupMedia(id)
	sessions[id] = nil
end

function querySession(id)
	-- Return info on a session
	logger.print("Queried session: " .. id)
	local s = sessions[id]
	if s == nil then
		return nil
	end
	local info = { script = s["lua"], id = s["id"] }
	local infojson = json.encode(info)
	return infojson
end

function handleMessage(id, tr, msg, jsep)
	-- Handle a message, synchronously or asynchronously, and return
	-- something accordingly: if it's the latter, we'll do a coroutine
	logger.print("Handling message for session: " .. id .. " - " .. tr)

	local s = sessions[id]
	if s == nil then
		return -1, "Session not found"
	end
	-- Decode the message JSON string to a table
	local msgT = json.decode(msg)
	-- Let's return a synchronous response if there's no jsep, asynchronous otherwise
	if jsep == nil then
		processRequest(id, tr, msgT)
		logger.print("handleMessage " .. id .. " - " .. tr)
		local response = { echotest = "response2", result = "ok" }
		local responsejson = json.encode(response)
		return 0, responsejson
	else
		-- Decode the JSEP JSON string to a table too
		local jsepT = json.decode(jsep)
		-- We need a new coroutine here
		local async = coroutine.create(function(id, tr, comsg, cojsep)
			-- We'll only execute this when the scheduler resumes the task
			logger.print("Handling async message for session: " .. id)
			local s = sessions[id]
			if s == nil then
				logger.print("Can't handle async message: so such session")
				return
			end
			local offer = sdp.parse(cojsep.sdp)
			logger.print("Got offer: " .. sdp.render(offer))
			local answer = sdp.generateAnswer(offer, { audio = true, video = true, data = true })
			logger.print("Generated answer: " .. sdp.render(answer))
			logger.print("Processing request: " .. dumpTable(comsg))
			processRequest(id, tr, comsg)
			logger.print("Pushing event:")
			local event = { echotest = "event", result = "ok2" }
			local jsonevent = json.encode(event)
			logger.print("  1-- " .. jsonevent)
			local tmp = sdp.render(answer)
			local jsepanswer = { type = "answer", sdp = tmp }
			logger.print("  2-- " .. sdp.render(answer))
			local jsonjsep = json.encode(jsepanswer)
			logger.print("  3-- " .. jsonjsep)
			pushEvent(id, tr, jsonevent, jsonjsep)
			-- Just for fun (and to showcase the feature), let's send an event to handlers
			-- notice how we pass the id now, meaning this event is tied to a specific session
			local event = { event = "processed", request = comsg }
			local eventjson = json.encode(event)
			notifyEvent(id, eventjson)
		end)
		-- Enqueue it: the scheduler will resume it later
		tasks[#tasks+1] = { co = async, id = id, tr = tr, msg = msgT, jsep = jsepT }
		-- Return explaining that this is will be handled asynchronously
		pokeScheduler()
		return 1, nil
	end
end

function setupMedia(id)
	-- WebRTC is now available
	logger.print("WebRTC PeerConnection is up for session: " .. id)
	-- Attach the session's stream to itself (echo test)
	-- addRecipient(id, id)
end

function hangupMedia(id)
	-- WebRTC not available anymore
	logger.print("WebRTC PeerConnection is down for session: " .. id)
	-- Detach the stream
	removeRecipient(id, id)
	-- Clear some flags
	local s = sessions[id]
	if s ~= nil then
		s.audioCodec = nil
		s.videoCodec = nil
	end
end

function resumeScheduler()
	-- This is the function responsible for resuming coroutines associated
	-- with whatever is relevant to the Lua script, e.g., for this script,
	-- with asynchronous requests: if you're handling async stuff yourself,
	-- you're free not to use this and just return, but the C Lua plugin
	-- expects this method to exist so it MUST be present, even if empty
	logger.print("Resuming coroutines")
	for index,task in ipairs(tasks) do
		local success, result = coroutine.resume(task.co, task.id, task.tr, task.msg, task.jsep)
		if not success then
			logger.print(colors("[%{red}exception%{reset}]") .. " " .. dumpTable(result))
		end
	end
	logger.print("Coroutines resumed")
	tasks = {}
end

-- We use this internal method to process an API request
function processRequest(id, tr, msg)
	if msg == nil then
		return -1
	end
	-- We implement most of the existing EchoTest API messages, here
	if msg["audio"] == true then
		configureMedium(id, "audio", "in", true)
		configureMedium(id, "audio", "out", true)
	elseif msg["audio"] == false then
		configureMedium(id, "audio", "in", false)
		configureMedium(id, "audio", "out", false)
	end
	if msg["video"] == true then
		configureMedium(id, "video", "in", true)
		configureMedium(id, "video", "out", true)
		sendPli(id)
	elseif msg["video"] == false then
		configureMedium(id, "video", "in", false)
		configureMedium(id, "video", "out", false)
	end
	if msg["data"] == true then
		configureMedium(id, "data", "in", true)
		configureMedium(id, "data", "out", true)
	elseif msg["data"] == false then
		configureMedium(id, "data", "in", false)
		configureMedium(id, "data", "out", false)
	end
	if msg["bitrate"] ~= nil then
		setBitrate(id, msg["bitrate"])
	end
	if msg["record"] == true then
		local fnbase = msg["filename"]
		if fnbase == nil then
			fnbase = "lua-echotest-" .. id .. "-" -- .. require 'socket'.gettime()
		end

		logger.print("startRecording id: " .. id .. " fnbase: " .. fnbase .. " -- " .. msg["file"])
		startRecording(id,
			"audio", "opus", "/tmp", fnbase .. "audio",
			"video", "vp8", "/tmp", fnbase .. "video"
			-- "data", "text", "/tmp", fnbase .. "data"
		)
	elseif msg["record"] == false then
		stopRecording(id, "audio", "video", "data")
	end
	if msg["play"] == true then
		logger.print("play true " .. id)
		-- local event = { echotest = "event", result = "ok" }
		-- local jsonevent = json.encode(event)
		-- pushEvent(id, tr, jsonevent, nil)
		-- startPlaying(id, tr,
		-- 	"/tmp", "rec-5162561637200768-audio",
		-- 	"/tmp", "rec-5162561637200768-video"
		-- )
		-- startPlaying(id, tr,
		-- 	"/tmp", "lua-echotest-1336105117--audio",
		-- 	"/tmp", "lua-echotest-1336105117--audio"
		-- )
		-- startPlaying(id, tr,
		-- 	"/tmp", "lua-echotest-633500220--audio",
		-- 	"/tmp", "lua-echotest-633500220--video"
		-- )

		local fnbase = msg["filename"]
		if fnbase == nil then
			fnbase = "lua-echotest-" .. id .. "-" -- .. require 'socket'.gettime()
		end
		startPlaying(id, tr,
			"/tmp", fnbase .. "audio",
			"/tmp", fnbase .. "video"
		)
	elseif msg["play"] == false then
		logger.print("play false 2")
		stopPlaying(id)
	end
	if msg["loop"] == true then
		addRecipient(id, id)
		sendPli(id)
		logger.print("loop true")
	elseif msg["loop"] == false then
		removeRecipient(id, id)
		logger.print("loop false")
	end
	return 0
end

function luaPushEvent(id, tr, json)
	pushEvent(id, tr, json, nil)
end

-- Helper for logging tables
-- https://stackoverflow.com/a/27028488
function dumpTable(o)
	if type(o) == 'table' then
		local s = '{ '
		for k,v in pairs(o) do
			if type(k) ~= 'number' then k = '"'..k..'"' end
			s = s .. '['..k..'] = ' .. dumpTable(v) .. ','
		end
		return s .. '} '
	else
		return tostring(o)
	end
end

-- Done
logger.print("Loaded")
