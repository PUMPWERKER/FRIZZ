-- reaper.lua: tools/reaper.py's runner inside REAPER. reaper.py puts PLAN (a table: the log,
-- the command file, the MIDI devices) in front of it as the startup script of a configuration
-- of its own. It mutes the master, checks the devices are where reaper.py expects them, then,
-- every time REAPER runs it (defer, ~30 times a second), carries out the lines reaper.py has
-- added to the command file since:
--   ID play | stop | pause            the transport, as its buttons
--   ID locate BEAT                    the play position (the edit cursor, seeking while playing)
--   ID tempo BPM                      the project's tempo
--   ID loop BEAT BEAT | loop off      the loop range, repeat on / off
--   ID lane LENGTH B,S,D1,D2 ...      a track sending to the device on the events' own channels,
--                                     a MIDI item of LENGTH beats from 0 with those CCs
--   ID monitor                        a track record-armed on the device's input, monitored:
--                                     what comes in goes out again
--   ID clear                          every track deleted
--   ID trace on | off                 the play position logged every time ("pos")
--   ID send HEX                       bytes to the device (SendMIDIMessageToHardware), SysEx
--                                     too; one a time round, so a key's press and release
--                                     never go in one USB packet
--   ID quit                           saves the project to PLAN.project (so quitting asks
--                                     nothing) and quits
-- What comes in from the device is logged as it's seen in REAPER's input history.
--
-- The log, a line each, the time in s since the start (time_precise): "T ready", "T dev out|in
-- INDEX NAME", "T audio MODE SRATE BSIZE", "T cmd ID NAME pos SECONDS BEATS state S", "T in
-- HEX", "T pos SECONDS BEATS state S", "T error TEXT", "T done".
local R = reaper
local out = assert(io.open(PLAN.log, "w"))
local t0 = R.time_precise()

local function log(fmt, ...)
    out:write(string.format("%.4f " .. fmt .. "\n", R.time_precise() - t0, ...))
    out:flush()
end

local function quit()
    log("done")
    out:close()
    R.Main_SaveProjectEx(0, PLAN.project, 0)
    R.Main_OnCommand(40004, 0) -- File: Quit
end

local function unhex(s)
    return (s:gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end))
end

local function hex(s)
    return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

log("ready")
for i = 0, R.GetNumMIDIOutputs() - 1 do
    local ok, name = R.GetMIDIOutputName(i, "")
    if ok then log("dev out %d %s", i, name) end
end
for i = 0, R.GetNumMIDIInputs() - 1 do
    local ok, name = R.GetMIDIInputName(i, "")
    if ok then log("dev in %d %s", i, name) end
end
local _, mode = R.GetAudioDeviceInfo("MODE", "")
local _, srate = R.GetAudioDeviceInfo("SRATE", "")
local _, bsize = R.GetAudioDeviceInfo("BSIZE", "")
log("audio %s %s %s", mode, srate, bsize)

local function check(kind, index, want)
    if index == nil then return true end
    local ok, name
    if kind == "out" then ok, name = R.GetMIDIOutputName(index, "") else ok, name = R.GetMIDIInputName(index, "") end
    if not ok or name ~= want then
        log("error %s %d is %s, not %s", kind, index, ok and name or "nothing", want)
        return false
    end
    return true
end
if not (check("out", PLAN.out, PLAN.out_name) and check("in", PLAN.input, PLAN.in_name)) then
    quit()
    return
end

R.SetMediaTrackInfo_Value(R.GetMasterTrack(0), "B_MUTE", 1)
R.GetSetRepeat(0)
R.SetEditCurPos(0, false, false)

local function beats(t)
    return R.TimeMap2_timeToQN(0, t)
end

local function new_track()
    local n = R.CountTracks(0)
    R.InsertTrackAtIndex(n, false)
    local tr = R.GetTrack(0, n)
    R.SetMediaTrackInfo_Value(tr, "I_MIDIHWOUT", PLAN.out << 5) -- channels as they are
    return tr
end

local function words(s)
    local w = {}
    for x in s:gmatch("%S+") do w[#w + 1] = x end
    return w
end

local tracing = false

-- one line: true once done, false to keep it for the next time round
local function run(w, sent)
    local name = w[2]
    if name == "send" then
        if sent then return false end
        R.SendMIDIMessageToHardware(PLAN.out, unhex(w[3]))
    elseif name == "play" then R.OnPlayButton()
    elseif name == "stop" then R.OnStopButton()
    elseif name == "pause" then R.OnPauseButton()
    elseif name == "locate" then R.SetEditCurPos(R.TimeMap2_QNToTime(0, tonumber(w[3])), false, true)
    elseif name == "tempo" then R.SetCurrentBPM(0, tonumber(w[3]), false)
    elseif name == "loop" then
        if w[3] == "off" then
            R.GetSetRepeat(0)
        else
            R.GetSet_LoopTimeRange(true, true, R.TimeMap2_QNToTime(0, tonumber(w[3])),
                R.TimeMap2_QNToTime(0, tonumber(w[4])), false)
            R.GetSetRepeat(1)
        end
    elseif name == "lane" then
        local tr = new_track()
        local item = R.CreateNewMIDIItemInProj(tr, 0, tonumber(w[3]), true) -- in beats
        local take = R.GetActiveTake(item)
        for i = 4, #w do
            local b, s, d1, d2 = w[i]:match("([^,]+),([^,]+),([^,]+),([^,]+)")
            local ppq = R.MIDI_GetPPQPosFromProjQN(take, tonumber(b))
            s = tonumber(s)
            R.MIDI_InsertCC(take, false, false, ppq, s & 0xF0, s & 0x0F, tonumber(d1), tonumber(d2))
        end
        R.MIDI_Sort(take)
    elseif name == "monitor" then
        local tr = new_track()
        R.SetMediaTrackInfo_Value(tr, "I_RECINPUT", 4096 + (PLAN.input << 5)) -- all channels
        R.SetMediaTrackInfo_Value(tr, "I_RECMODE", 2) -- record nothing: monitoring only
        R.SetMediaTrackInfo_Value(tr, "I_RECMON", 1)
        R.SetMediaTrackInfo_Value(tr, "I_RECARM", 1)
    elseif name == "clear" then
        for i = R.CountTracks(0) - 1, 0, -1 do R.DeleteTrack(R.GetTrack(0, i)) end
    elseif name == "trace" then
        tracing = w[3] == "on"

    end
    local pos = R.GetPlayState() & 1 == 1 and R.GetPlayPosition() or R.GetCursorPosition()
    log("cmd %s %s pos %.4f %.4f state %d", w[1], name, pos, beats(pos), R.GetPlayState())
    if name == "quit" then
        R.OnStopButton()
        quit()
        return nil
    end
    return true
end

-- what came in since the last look, oldest first
local last_seq = R.MIDI_GetRecentInputEvent(0)
local function listen()
    local got, idx = {}, 0
    while true do
        local seq, buf = R.MIDI_GetRecentInputEvent(idx)
        if seq == 0 or seq == last_seq then break end
        got[#got + 1] = buf
        idx = idx + 1
    end
    if #got > 0 then last_seq = R.MIDI_GetRecentInputEvent(0) end
    for i = #got, 1, -1 do log("in %s", hex(got[i])) end
end

local read_at, pending = 0, {}
local function loop()
    listen()
    local f = io.open(PLAN.cmds, "r")
    if f then
        f:seek("set", read_at)
        local text = f:read("a") or ""
        f:close()
        -- whole lines only: one still being written waits
        local whole = text:match("^(.*\n)") or ""
        read_at = read_at + #whole
        for line in whole:gmatch("([^\n]*)\n") do pending[#pending + 1] = words(line) end
    end
    local sent = false
    while #pending > 0 do
        local w = pending[1]
        -- an error is logged and the line dropped: a script that stops would leave REAPER
        -- showing its error, and waiting
        local ok, done = pcall(run, w, sent)
        if not ok then
            log("error %s: %s", table.concat(w, " "):sub(1, 60), tostring(done))
            done = true
        end
        if done == nil then return end -- quit
        if not done then break end
        sent = sent or w[2] == "send"
        table.remove(pending, 1)
    end
    if tracing then
        local pos = R.GetPlayPosition()
        log("pos %.4f %.4f state %d", pos, beats(pos), R.GetPlayState())
    end
    R.defer(loop)
end
log("listening")
loop()
