-- The headless game: the emitted Lua under the engine stub, driven over stdin so the
-- real app can play against it. scripts/fake_game.py --mod starts one of these, points
-- the app at the same --root and --user trees, and then plays the player: looks,
-- presses, picks rows, lets the holder give in, saves, quits, relaunches, loads.
--
--   luajit host.lua --mod <moonshine_names.lua> --root DIR --user DIR [--log PATH] [--echo]
--
-- One command per line on stdin, one reply line on stdout, `ok ...` or `err ...`.
-- Rows and lists are one line with fields split by \t and items by \x1f.
--
--   quest <id|name>                 ok id=<id> kind= giver= target= item= name= reward= pay= count=
--   soul <name> [x y z]             make a soul (idempotent); the player is "dude" at 0,0,0
--   give <soul|player> <class> <n>  put n of a class in a pocket
--   count <soul|player> <class>     ok <n>
--   kill <soul>                     the soul dies
--   look <soul>                     ok favour|plain|talk|none   (what the prompt reads)
--   begin <soul>                    press the mod's prompt
--   tick [ms]                       advance the clock (default 400); ok active= menu= drawn= turn= ports=
--   rows                            ok <row>\t<intent>\t<text>\x1f...   (the menu as drawn)
--   pick <row>                      confirm a row of the presented menu
--   leave                           pick the way out, wherever it was drawn
--   holder <id> <how>               the holder's own menu ended on <how> (persuade|threaten|pay|brokered|nested)
--   delivered <id>                  the receiver's one-sequence Talk ran
--   carried <id>                    the onward soul's one-sequence Talk ran (an ask's word carried on)
--   gift <id> took|refused          the bribe menu after an ask's winning row
--   checked <id>                    a mind target's visit row ran
--   broker <id>                     a dispute holder's broker row ran
--   directed <id>                   an ambush target's first row ran (the false way)
--   state <id>                      ok accepted= handed= done= how= flags=
--   ended <id> <how>                the store told the errand is done that way (opens a gated scene)
--   offerable <id>                  ok true|false <link>
--   quests                          ok <id>\t<kind>\t<giver>\t<target>\t<item>\t<count>\t<pay>\t<reward>\t<onward>\t<gift>\t<dispute>\t<spot>\t<keepsake>\t<after>\t<afterHow>\t<name>\t<order>\t<hours>\t<carry>\t<loot>\x1f...
--   move <soul|player> x y z        put somebody somewhere
--   pos <soul|player>               ok x y z
--   down <soul>                     the soul is knocked out
--   hour <h>                        the game clock's hour (fractional), for the hour windows
--   clock <h>                       put the world clock on h hours (the mind's gap, the curse's two days)
--   key                             the leave key (F9) pressed at an armed spot; ok armed|unarmed
--   visit <id>                      a mind target's Talk as the game gates it: counted when the
--                                   visit is due (the due global up), early otherwise; ok counted|early
--   save <path> / load <path>       the saved game through Player.OnSave/OnLoad
--   arrive                          a world with no store in it (OnInit resets)
--   log                             ok <path of kcd.log>
--   quit

local here = string.match(arg[0], "^(.*)/[^/]*$") or "."
local Engine = dofile(here .. "/engine.lua")

local opts = { echo = false }
local i = 1
while i <= #arg do
    local a = arg[i]
    if a == "--mod" then opts.mod = arg[i + 1]; i = i + 1
    elseif a == "--root" then opts.root = arg[i + 1]; i = i + 1
    elseif a == "--user" then opts.user = arg[i + 1]; i = i + 1
    elseif a == "--log" then opts.log = arg[i + 1]; i = i + 1
    elseif a == "--echo" then opts.echo = true
    end
    i = i + 1
end
if opts.mod == nil or opts.root == nil or opts.user == nil then
    io.stderr:write("usage: luajit host.lua --mod LUA --root DIR --user DIR [--log PATH] [--echo]\n")
    os.exit(2)
end
opts.log = opts.log or (opts.root .. "/kcd.log")

local M = Engine.boot(opts.mod, { root = opts.root, user = opts.user, log = opts.log, echo = opts.echo })

local function reply(text) io.stdout:write(text, "\n"); io.stdout:flush() end
local function ok(text) reply(text and ("ok " .. text) or "ok") end
local function err(text) reply("err " .. tostring(text)) end

local function who(name)
    if name == "player" or name == "dude" then return player end
    return Engine.entities[name]
end

local function findQuest(key)
    if M.quests[key] then return key, M.quests[key] end
    for id, q in pairs(M.quests) do if q.name == key then return id, q end end
    return nil
end

local function status()
    -- Ports as id:command, since the reply is split on spaces.
    local ports = {}
    for i, p in ipairs(Engine.ports) do ports[i] = (string.gsub(p, " ", ":")) end
    return string.format("active=%s menu=%s drawn=%d turn=%d ports=%s",
        tostring(M.active), tostring(Engine.dialog.menu), M.menuDrawn, M.turn, table.concat(ports, ","))
end

local handlers = {}

function handlers.quest(a)
    local id, q = findQuest(table.concat(a, " ", 2))
    if id == nil then return err("no quest " .. table.concat(a, " ", 2)) end
    -- name last, since it has spaces in it.
    return ok(string.format("id=%s kind=%s giver=%s target=%s item=%s reward=%d pay=%d count=%d name=%s",
        id, q.kind, q.giver, q.target, q.item, q.reward or 0, q.pay or 0, q.count or 1, q.name))
end

function handlers.soul(a)
    if a[2] == nil then return err("soul <name> [x y z]") end
    local pos = { x = tonumber(a[3]) or 1, y = tonumber(a[4]) or 0, z = tonumber(a[5]) or 0 }
    local e = Engine.soul(a[2], { pos = pos })
    e.pos = pos
    return ok(e.name)
end

function handlers.give(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    e.inventory:CreateItem(a[3], 100, tonumber(a[4]) or 1)
    return ok(tostring(e.inventory:GetCountOfClass(a[3])))
end

function handlers.count(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    return ok(tostring(e.inventory:GetCountOfClass(a[3])))
end

function handlers.kill(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    e.alive = false
    return ok()
end

function handlers.look(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    local acts = Engine.look(e)
    if #acts == 0 then return ok("none") end
    if acts[1].base then return ok("talk") end
    local hint = acts[1].fields and acts[1].fields.hint
    if hint == M.favourHint then return ok("favour") end
    if hint == M.hint then return ok("plain") end
    return ok(tostring(hint))
end

function handlers.begin(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    local began, why = Engine.begin(e)
    if not began then return err(why) end
    return ok(status())
end

function handlers.tick(a)
    Engine.advance(tonumber(a[2]) or 400)
    return ok(status())
end

function handlers.rows()
    local parts = {}
    for row = 1, M.menuRows - 1 do
        local opt = M.options[row]
        if opt ~= nil then parts[#parts + 1] = row .. "\t" .. opt.intent .. "\t" .. opt.text end
    end
    parts[#parts + 1] = M.menuLeaveRow .. "\tleave\t" .. M.menuLeave
    return ok(table.concat(parts, "\31"))
end

function handlers.pick(a)
    local row = tonumber(a[2])
    if row == nil then return err("pick <row>") end
    local fine, why = pcall(Engine.pick, row)
    if not fine then return err(why) end
    return ok(status())
end

function handlers.leave()
    local fine, why = pcall(Engine.pick, M.menuLeaveRow)
    if not fine then return err(why) end
    return ok(status())
end

function handlers.holder(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    Engine.holderGave(id, a[3] or "persuade")
    return ok(status())
end

function handlers.delivered(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    Engine.delivered(id)
    return ok(status())
end

function handlers.carried(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    Engine.carried(id)
    return ok(status())
end

function handlers.gift(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    if a[3] ~= "took" and a[3] ~= "refused" then return err("gift <id> took|refused") end
    M.QuestGift(id, a[3])
    return ok(status())
end

function handlers.checked(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    M.QuestChecked(id)
    return ok(status())
end

function handlers.broker(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    M.QuestBroker(id)
    return ok(status())
end

function handlers.directed(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    M.QuestDirected(id)
    return ok(status())
end

function handlers.state(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    local f = M.state.quests[id] or {}
    return ok(string.format("accepted=%s handed=%s done=%s how=%s flags=%s",
        tostring(f.accepted == true), tostring(f.handed == true), tostring(f.done == true),
        tostring(f.how), M.QuestFlagLetters(f)))
end

-- The errand done a given way, as the store would have it after the fact and without the
-- playing: for a scene gated on the one before it (QuestLinkState reads done and how).
function handlers.ended(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    M.StateQuest(id, "complete")
    local f = M.state.quests[id]
    if a[3] ~= nil and a[3] ~= "truth" then f.how = a[3] end
    pcall(M.QuestFlags)
    return ok(status())
end

function handlers.quests()
    local ids = {}
    for id in pairs(M.quests) do ids[#ids + 1] = id end
    table.sort(ids)
    local parts = {}
    for _, id in ipairs(ids) do
        local q = M.quests[id]
        parts[#parts + 1] = table.concat({
            id, q.kind, q.giver, q.target, q.item, tostring(q.count or 1), tostring(q.pay or 0),
            tostring(q.reward or 0), q.onward or "", q.gift and "yes" or "", q.dispute and "yes" or "",
            q.spot and string.format("%g,%g,%g", q.spot[1], q.spot[2], q.spot[3]) or "",
            q.keepsake or "", q.after or "", q.afterHow or "", q.name, tostring(q.order or 0),
            q.hours and string.format("%g-%g", q.hours[1], q.hours[2]) or "",
            q.carry or "", q.loot and q.loot[1] or "",
        }, "\t")
    end
    return ok(table.concat(parts, "\31"))
end

function handlers.move(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    e.pos = { x = tonumber(a[3]) or 0, y = tonumber(a[4]) or 0, z = tonumber(a[5]) or 0 }
    return ok()
end

function handlers.pos(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    return ok(string.format("%g %g %g", e.pos.x, e.pos.y, e.pos.z))
end

function handlers.down(a)
    local e = who(a[2])
    if e == nil then return err("no soul " .. tostring(a[2])) end
    e.down = true
    return ok()
end

function handlers.hour(a)
    local h = tonumber(a[2])
    if h == nil then return err("hour <h>") end
    Engine.hour = h
    return ok()
end

function handlers.clock(a)
    local h = tonumber(a[2])
    if h == nil then return err("clock <h>") end
    Engine.worldBase = Engine.worldBase + h * 3600
    Engine.hour = (Engine.hour + h) % 24
    return ok(string.format("hour=%g", Engine.hour))
end

function handlers.key()
    if M.spotArmed == nil then return ok("unarmed") end
    local fine, why = pcall(M.SpotKey)
    if not fine then return err(why) end
    return ok("armed")
end

function handlers.visit(a)
    local id, q = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    if q.kind ~= "mind" then return err("not a mind: " .. id) end
    if Engine.gvar(q.dueVar) ~= 1 then return ok("early") end
    M.QuestChecked(id)
    return ok("counted")
end

function handlers.offerable(a)
    local id = findQuest(a[2])
    if id == nil then return err("no quest " .. tostring(a[2])) end
    return ok(tostring(M.QuestOfferable(id)) .. " " .. M.QuestLinkState(id))
end

function handlers.save(a)
    if a[2] == nil then return err("save <path>") end
    local fine, why = pcall(Engine.save, a[2])
    if not fine then return err(why) end
    return ok(string.format("longest=%d", Engine.lastSave.longest))
end

function handlers.load(a)
    if a[2] == nil then return err("load <path>") end
    local fine, why = Engine.load(a[2])
    if not fine then return err(why) end
    return ok(status())
end

function handlers.arrive()
    Engine.arrive()
    return ok(status())
end

function handlers.log() return ok(opts.log) end

reply("ready " .. opts.log)
for line in io.lines() do
    local a = {}
    for word in string.gmatch(line, "%S+") do a[#a + 1] = word end
    if a[1] == "quit" then break end
    local h = handlers[a[1] or ""]
    if h == nil then
        err("unknown command " .. tostring(a[1]))
    else
        local fine, why = pcall(h, a)
        if not fine then err(why) end
    end
end
