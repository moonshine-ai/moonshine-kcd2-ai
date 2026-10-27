-- Run the harness cases against an emitted moonshine_names.lua.
--
--   luajit mod/harness/run.lua <path/to/moonshine_names.lua> [--only PATTERN] [--echo] [--list]
--
-- Exit 0 when every case holds, 1 otherwise; one line per case, the failing check
-- named. --echo prints the mod's log as it goes, which is how a failing case is read.
-- scripts/build_mod.sh runs this after luac -p.

local here = string.match(arg[0], "^(.*)/[^/]*$") or "."
local modPath, only, echo, list = nil, nil, false, false
local i = 1
while i <= #arg do
    local a = arg[i]
    if a == "--only" then only = arg[i + 1]; i = i + 1
    elseif a == "--echo" then echo = true
    elseif a == "--list" then list = true
    else modPath = a end
    i = i + 1
end
if modPath == nil then
    io.stderr:write("usage: luajit run.lua <moonshine_names.lua> [--only PATTERN] [--echo] [--list]\n")
    os.exit(2)
end

local Engine = dofile(here .. "/engine.lua")
local suite = dofile(here .. "/cases.lua")

if list then
    for _, name in ipairs(suite.order) do print(name) end
    os.exit(0)
end

local M = Engine.boot(modPath, { echo = echo })

local passed, failed = 0, 0
for _, name in ipairs(suite.order) do
    if only == nil or string.find(name, only, 1, true) then
        Engine.reset()
        local failure = nil
        local function check(cond, what)
            if failure == nil and not cond then failure = what or "a check" end
            if not cond then error({ harness = true }) end
        end
        local ok, err = pcall(suite.cases[name], Engine, M, check)
        if ok then
            passed = passed + 1
            print("ok    " .. name)
        else
            failed = failed + 1
            if failure == nil then
                failure = "raised " .. tostring(type(err) == "table" and err.harness and "?" or err)
            end
            print("FAIL  " .. name .. "\n      " .. failure)
            if not echo then
                -- The last of the mod's log, which is where the why usually is.
                local from = math.max(1, #Engine.lines - 12)
                for j = from, #Engine.lines do print("      | " .. Engine.lines[j]) end
            end
        end
    end
end

print(string.format("%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
