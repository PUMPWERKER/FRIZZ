-- reaper.lua: tools/reaper.py's runner inside REAPER. reaper.py puts PLAN (a table: the log,
-- the command file, the MIDI devices) in front of it as the startup script of a configuration
-- of its own. It opens PLAN.sync if there is one (a project synchronized to its MIDI input,
-- reaper.py's daw-follows), mutes the master, checks the devices are where reaper.py expects
-- them, then,
-- every time REAPER runs it (defer, ~30 times a second), carries out the lines reaper.py has
-- added to the command file since:
--   ID play | stop | pause            the transport, as its buttons
--   ID locate BEAT                    the play position (the edit cursor, seeking while playing)
--   ID tempo BPM                      the project's tempo
--   ID loop BEAT BEAT | loop off      the loop range, repeat on / off
--   ID lane LENGTH B,S,D1,D2 ...      a track sending to the device on the events' own channels,
--                                     a MIDI item of LENGTH beats from 0 with those CCs, and
--                                     notes: a note on (0x9n) lasts until its note off
--   ID monitor                        a track record-armed on the device's input, monitored:
--                                     what comes in goes out again
--   ID clear                          every track deleted
--   ID trace on | off                 the play position logged every time ("pos")
--   ID media PATH BEATS               a track playing the WAV at PATH from 0, looped over BEATS,
--                                     to REAPER's outputs 3/4 only (not the master)
--   ID rec                            a track recording REAPER's input 1 (mono), armed, not
--                                     monitored, sent nowhere; its files into PLAN.recdir
--   ID record                         the transport's record button
--   ID takes                          the takes recorded since the last time logged ("take"),
--                                     their items deleted (the files stay)
--   ID send HEX                       bytes to the device (SendMIDIMessageToHardware), SysEx
--                                     too; one a time round, so a key's press and release
--                                     never go in one USB packet
--   ID quit                           saves the project to PLAN.project (so quitting asks
--                                     nothing) and quits
-- What comes in from the device is logged as it's seen in REAPER's input history.
--
-- The log, a line each, the time in s since the start (time_precise): "T ready", "T dev out|in
-- INDEX NAME", "T audio MODE SRATE BSIZE", "T sync SYNC INPUT clock MASK" (the project's
-- projsmptesync and projsmpteinput, reaper.ini's midiouts_clock, as REAPER has them), "T cmd ID
-- NAME pos SECONDS BEATS state S", "T in HEX", "T pos SECONDS BEATS state S rate RATE bpm BPM"
-- (the play rate, the tempo at the position), "T take POSITION OFFSET LENGTH FILE" (a recorded
-- take: where its item starts on the project's time line, s, where in FILE it starts, its
-- length), "T error TEXT", "T done".
local R = reaper
local out = assert(io.open(PLAN.log, "w"))
local t0 = R.time_precise()

local function log(fmt, ...)
    out:write(string.format("%.4f " .. fmt .. "\n", R.time_precise() - t0, ...))
    out:flush()
end

local recorded = false

local function quit()
    log("done")
    out:close()
    -- no tracks left: a project holding a recording asks about its files on quitting
    for i = R.CountTracks(0) - 1, 0, -1 do R.DeleteTrack(R.GetTrack(0, i)) end
    R.Main_SaveProjectEx(0, PLAN.project, 0)
    if recorded then R.Main_openProject("noprompt:" .. PLAN.project) end -- forgets its recordings
    R.Main_OnCommand(40004, 0) -- File: Quit
end

local function unhex(s)
    return (s:gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end))
end

local function hex(s)
    return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

if PLAN.sync then R.Main_openProject("noprompt:" .. PLAN.sync) end
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
local function var(name)
    local _, v = R.get_config_var_string(name)
    return v or "?"
end
log("sync %s %s clock %s", var("projsmptesync"), var("projsmpteinput"), var("midiouts_clock"))

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
R.GetSetProjectInfo_String(0, "RECORD_PATH", PLAN.recdir, true)
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
        local ev = {}
        for i = 4, #w do
            local b, s, d1, d2 = w[i]:match("([^,]+),([^,]+),([^,]+),([^,]+)")
            ev[#ev + 1] = {tonumber(b), tonumber(s), tonumber(d1), tonumber(d2)}
        end
        for i, e in ipairs(ev) do
            local ppq = R.MIDI_GetPPQPosFromProjQN(take, e[1])
            if e[2] & 0xF0 == 0x90 and e[4] > 0 then
                -- a note: on until its note off (0x80, or 0x90 at velocity 0) later in the list
                local off = e[1] + .25
                for k = i + 1, #ev do
                    local f = ev[k]
                    if f[3] == e[3] and (f[2] == (e[2] & 0x0F | 0x80) or f[2] == e[2] and f[4] == 0) then
                        off = f[1]
                        break
                    end
                end
                R.MIDI_InsertNote(take, false, false, ppq, R.MIDI_GetPPQPosFromProjQN(take, off), e[2] & 0x0F,
                    e[3], e[4], true)
            elseif e[2] & 0xF0 ~= 0x80 and e[2] & 0xF0 ~= 0x90 then
                R.MIDI_InsertCC(take, false, false, ppq, e[2] & 0xF0, e[2] & 0x0F, e[3], e[4])
            end
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
    elseif name == "media" then
        local n = R.CountTracks(0)
        R.InsertTrackAtIndex(n, false)
        local tr = R.GetTrack(0, n)
        R.SetMediaTrackInfo_Value(tr, "B_MAINSEND", 0)
        local send = R.CreateTrackSend(tr, nil) -- a hardware output
        R.SetTrackSendInfo_Value(tr, 1, send, "I_DSTCHAN", 2) -- outputs 3/4
        local item = R.AddMediaItemToTrack(tr)
        local take = R.AddTakeToMediaItem(item)
        R.SetMediaItemTake_Source(take, R.PCM_Source_CreateFromFile(w[3]))
        R.SetMediaItemInfo_Value(item, "D_POSITION", 0)
        R.SetMediaItemInfo_Value(item, "D_LENGTH", R.TimeMap2_QNToTime(0, tonumber(w[4])))
        R.SetMediaItemInfo_Value(item, "B_LOOPSRC", 1)
        R.UpdateArrange()
    elseif name == "rec" then
        local n = R.CountTracks(0)
        R.InsertTrackAtIndex(n, false)
        local tr = R.GetTrack(0, n)
        R.SetMediaTrackInfo_Value(tr, "B_MAINSEND", 0)
        R.SetMediaTrackInfo_Value(tr, "I_RECINPUT", 0) -- input 1, mono
        R.SetMediaTrackInfo_Value(tr, "I_RECMODE", 0) -- the input as it comes
        R.SetMediaTrackInfo_Value(tr, "I_RECMON", 0) -- not monitored: nothing goes out again
        R.SetMediaTrackInfo_Value(tr, "I_RECARM", 1)
    elseif name == "record" then
        R.Main_OnCommand(1013, 0) -- Transport: Record
        recorded = true
    elseif name == "takes" then
        for i = R.CountMediaItems(0) - 1, 0, -1 do
            local item = R.GetMediaItem(0, i)
            local tr = R.GetMediaItem_Track(item)
            if R.GetMediaTrackInfo_Value(tr, "I_RECARM") == 1 then
                for k = 0, R.CountTakes(item) - 1 do
                    local take = R.GetTake(item, k)
                    if take then
                        local file = R.GetMediaSourceFileName(R.GetMediaItemTake_Source(take), "")
                        log("take %.6f %.6f %.6f %s", R.GetMediaItemInfo_Value(item, "D_POSITION"),
                            R.GetMediaItemTakeInfo_Value(take, "D_STARTOFFS"),
                            R.GetMediaItemInfo_Value(item, "D_LENGTH"), file)
                    end
                end
                R.DeleteTrackMediaItem(tr, item) -- told once: the next recording on a clear track
            end
        end
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
        log("pos %.4f %.4f state %d rate %.4f bpm %.3f", pos, beats(pos), R.GetPlayState(),
            R.Master_GetPlayRate(0), R.TimeMap_GetDividedBpmAtTime(pos))
    end
    R.defer(loop)
end
log("listening")
loop()
