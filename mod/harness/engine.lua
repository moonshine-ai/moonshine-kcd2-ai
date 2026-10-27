-- The engine, as far as the emitted Lua can tell. A stub of every global the mod
-- touches (mods/kcd2/README.md is the record of what each one really does), a fake
-- clock that Script.SetTimer runs on, a saved game the player's OnSave/OnLoad carry
-- the store through under the 65,536-byte rule, a dialogue loop that Autoselects the
-- sequence whose gvar matches as talk.xml does, and a kcd.log writer. Nothing here
-- pretends to be the engine's own behaviour -- whether the graph takes a token,
-- whether the menu draws -- that stays the Windows laptop's. What it gives is a place the real
-- emitted Lua can run, on the Mac, in milliseconds.
--
--   local Engine = dofile("mod/harness/engine.lua")
--   Engine.boot("path/to/moonshine_names.lua", { log = "kcd.log", root = DIR, user = DIR })
--   local npc = Engine.soul("kkut_additive_man_47")
--   Engine.begin(npc)            -- the player pressed the mod's prompt
--   Engine.advance(400)          -- the pump ticks, the beats play, timers fire
--
-- Lua 5.1 (LuaJIT): the engine's dialect. No 5.2+ forms here or in the mod.

local Engine = {}

-- ---------------------------------------------------------------------------
-- The clock. Milliseconds since boot; nothing moves unless advance() is called.
Engine.now = 0
Engine.timers = {}
Engine.timerSeq = 0
-- World seconds (Calendar.GetWorldTime) run at worldRate per real second from
-- worldBase; the default puts the first conversation in a drizzly late afternoon,
-- as scripts/fake_game.py does.
Engine.worldBase = 1000000
Engine.worldRate = 1
Engine.hour = 16.7823
Engine.rain = 0.24

function Engine.seconds() return Engine.now / 1000 end
function Engine.worldTime() return Engine.worldBase + Engine.seconds() * Engine.worldRate end

local function stamp()
    local s = math.floor(Engine.seconds())
    return string.format("<%02d:%02d:%02d>", 12 + math.floor(s / 3600) % 12, math.floor(s / 60) % 60, s % 60)
end

-- Advance the clock by ms, firing every timer that comes due in order. A timer
-- that arms another inside the window is honoured within the same call.
function Engine.advance(ms)
    local stop = Engine.now + ms
    while true do
        local next_, at = nil, nil
        for i, t in ipairs(Engine.timers) do
            if t.at <= stop and (at == nil or t.at < at or (t.at == at and t.seq < Engine.timers[next_].seq)) then
                next_, at = i, t.at
            end
        end
        if next_ == nil then break end
        local t = table.remove(Engine.timers, next_)
        Engine.now = math.max(Engine.now, t.at)
        local ok, err = pcall(t.fn)
        if not ok then Engine.log("[harness] timer raised " .. tostring(err)) end
    end
    Engine.now = stop
end

function Engine.setTimer(ms, fn)
    Engine.timerSeq = Engine.timerSeq + 1
    Engine.timers[#Engine.timers + 1] = { at = Engine.now + ms, fn = fn, seq = Engine.timerSeq }
end

-- ---------------------------------------------------------------------------
-- The log. Every System.LogAlways line goes to the file (what the app follows) and
-- is kept in memory for the cases to search.
Engine.logPath = nil
Engine.logFile = nil
Engine.lines = {}
Engine.echo = false

function Engine.log(text)
    local line = stamp() .. " " .. text
    Engine.lines[#Engine.lines + 1] = line
    if Engine.logFile then Engine.logFile:write(line, "\n"); Engine.logFile:flush() end
    if Engine.echo then io.stderr:write(line, "\n") end
end

-- The lines since a mark, or all of them; `find` is the first containing a plain
-- substring, `count` how many do.
function Engine.mark() return #Engine.lines end
function Engine.find(needle, since)
    for i = (since or 0) + 1, #Engine.lines do
        if string.find(Engine.lines[i], needle, 1, true) then return Engine.lines[i], i end
    end
    return nil
end
function Engine.count(needle, since)
    local n = 0
    for i = (since or 0) + 1, #Engine.lines do
        if string.find(Engine.lines[i], needle, 1, true) then n = n + 1 end
    end
    return n
end

-- ---------------------------------------------------------------------------
-- Globals the dialogue graph reads (gvar) and the mod writes.
Engine.globals = {}
function Engine.gvar(name) return Engine.globals[name] or 0 end

-- ---------------------------------------------------------------------------
-- Files. System.LoadTextFile resolves "%USER%/..." against `user` and anything else
-- against `root`, the two trees the app writes its turn files into. With neither
-- set, `Engine.files[path]` is read instead, for cases that need no disk.
Engine.root = nil
Engine.user = nil
Engine.files = {}
-- The app's heartbeat, alive_<bucket>.txt beside the turn files: readable unless a case
-- says otherwise, since nearly every case is a conversation the app is answering.
Engine.appUp = true
-- What System.AddCCommand registered, by name; kept across resets, since the mod registers
-- once at load. Engine.console plays a line typed at the game's console: the engine's
-- substitution of %% with the words after the name, each quoted and comma-separated.
Engine.commands = {}
function Engine.console(line)
    local words = {}
    for w in string.gmatch(line, "%S+") do words[#words + 1] = w end
    local cmd = Engine.commands[words[1]]
    if cmd == nil then
        Engine.log("[Warning] Unknown command: " .. tostring(words[1]))
        return false
    end
    local quoted = {}
    for i = 2, #words do quoted[#quoted + 1] = string.format("%q", words[i]) end
    local body = string.gsub(cmd.body, "%%%%", function() return table.concat(quoted, ",") end)
    local chunk = assert(loadstring(body, "console"))
    return chunk()
end

local function readFile(path)
    local f = io.open(path, "rb")
    if f == nil then return nil end
    local got = f:read("*a")
    f:close()
    return got
end

function Engine.loadTextFile(path)
    if Engine.files[path] ~= nil then return Engine.files[path] end
    if string.find(path, "alive_%d+%.txt$") then
        if Engine.appUp then return "alive" end
        return nil
    end
    if Engine.root == nil and Engine.user == nil then return nil end
    local real
    if string.sub(path, 1, 7) == "%USER%/" then
        if Engine.user == nil then return nil end
        real = Engine.user .. "/" .. string.sub(path, 8)
    else
        if Engine.root == nil then return nil end
        real = Engine.root .. "/" .. path
    end
    return readFile(real)
end

-- ---------------------------------------------------------------------------
-- Entities. A soul is a table with the handful of components the mod reaches for:
-- soul (stats, metaroles, perks), actor (alive, talkable), inventory (counts by
-- class), human (dialog state), and a position.
Engine.entities = {}
Engine.entitySeq = 100
Engine.clipLengths = {}
Engine.inventories = {}
Engine.ground = {}

local Inventory = {}
Inventory.__index = Inventory

-- Items are rows, the way the game keeps them (health_probe.lua, 6 Oct 2026): one row per
-- class and health, with an amount, and a wuid each. The harness's wuid is the row itself
-- ({ class, inv, health, amount }), which is also what PlaceItem below takes out by.
-- GetCountOfClass sums the rows; CreateItem stacks onto a row of the same class and health
-- (health on 0..1, anything above clamped to 1, as the engine does); the by-class takers
-- (DeleteItemOfClass, MoveItemOfClass) go for the worst row first, as measured.
function Inventory.new(owner)
    Engine.entitySeq = Engine.entitySeq + 1
    local inv = setmetatable({ id = Engine.entitySeq, owner = owner, rows = {} }, Inventory)
    Engine.inventories[inv.id] = inv
    return inv
end
function Inventory:GetId() return self.id end
function Inventory:GetCountOfClass(class)
    local n = 0
    for _, r in ipairs(self.rows) do if r.class == class then n = n + r.amount end end
    return n
end
function Inventory:CreateItem(class, health, amount)
    -- The engine refuses a class it does not know and a class marked IsQuestItem; the
    -- harness knows every class, since what it checks is the mod's bookkeeping.
    if type(class) ~= "string" or class == "" then error("CreateItem: bad class") end
    health = math.max(0, math.min(1, tonumber(health) or 1))
    for _, r in ipairs(self.rows) do
        if r.class == class and r.health == health then r.amount = r.amount + (amount or 1); return true end
    end
    self.rows[#self.rows + 1] = { class = class, inv = self.id, health = health, amount = amount or 1 }
    return true
end
-- Rows of a class, worst first: the order the engine's by-class takers draw from.
function Inventory:worstFirst(class)
    local out = {}
    for _, r in ipairs(self.rows) do if r.class == class then out[#out + 1] = r end end
    table.sort(out, function(a, b) return a.health < b.health end)
    return out
end
function Inventory:dropEmpty()
    for i = #self.rows, 1, -1 do if self.rows[i].amount <= 0 then table.remove(self.rows, i) end end
end
function Inventory:DeleteItemOfClass(class, n)
    local left = n or 1
    for _, r in ipairs(self:worstFirst(class)) do
        local take = math.min(r.amount, left)
        r.amount = r.amount - take
        left = left - take
        if left <= 0 then break end
    end
    self:dropEmpty()
    return (n or 1) - left
end
function Inventory:MoveItemOfClass(toId, class, n, _)
    local to = Engine.inventories[toId]
    if to == nil then error("MoveItemOfClass: no inventory " .. tostring(toId)) end
    local left = n or 1
    for _, r in ipairs(self:worstFirst(class)) do
        local take = math.min(r.amount, left)
        r.amount = r.amount - take
        if take > 0 then to:CreateItem(class, r.health, take) end
        left = left - take
        if left <= 0 then break end
    end
    self:dropEmpty()
    return (n or 1) - left
end
function Inventory:AddItem() error("AddItem raises from mod Lua") end
function Inventory:DeleteItem() return nil end   -- measured: answers nil and removes nothing
-- A wuid for one item of the class, when there is one.
function Inventory:FindItem(class)
    for _, r in ipairs(self.rows) do if r.class == class and r.amount > 0 then return r end end
    return nil
end
-- The game's GetMoney reads in groschen with a decimal (6380.7 for 63807 pieces).
function Inventory:GetMoney()
    return self:GetCountOfClass(Engine.moneyClass) / 10
end
function Inventory:GetInventoryTable()
    local out = {}
    for _, r in ipairs(self.rows) do out[#out + 1] = r end
    return out
end
-- ItemManager.GetItem's table for a wuid, and RemoveItem's whole-row removal.
function Engine.itemOf(wuid)
    if type(wuid) ~= "table" or wuid.inv == nil then return nil end
    local inv = Engine.inventories[wuid.inv]
    if inv == nil then return nil end
    for _, r in ipairs(inv.rows) do
        if r == wuid then return { health = r.health, amount = r.amount, id = r, class = r.class, entity = 0 } end
    end
    return nil
end
function Engine.removeItem(wuid)
    if type(wuid) ~= "table" or wuid.inv == nil then return nil end
    local inv = Engine.inventories[wuid.inv]
    if inv == nil then return nil end
    for i, r in ipairs(inv.rows) do if r == wuid then table.remove(inv.rows, i); break end end
    return nil
end

local function soulComponent(e)
    local soul = {
        metaroles = {}, perks = {}, stats = { rch = 0.5, bld = 0, drt = 0.02, dru = 0, bad = 0, sml = 0 },
        states = { health = 100 }, contexts = {},
    }
    function soul:GetMetaRoles() local out = {}; for k in pairs(self.metaroles) do out[#out + 1] = k end; table.sort(out); return out end
    function soul:AddMetaRoleByName(name) self.metaroles[name] = true; return true end
    function soul:GetDerivedStat(name) return self.stats[name] end
    function soul:GetState(name) return self.states[name] end
    function soul:HasScriptContext(name) return self.contexts[name] == true end
    function soul:HasPerk(id) return self.perks[id] == true end
    function soul:GetId() return e.id end
    function soul:GetStatLevel() return 1 end
    function soul:GetSocialClass() return { Id = 1, Name = "townsman" } end
    function soul:DealDamage(hp) e.alive = false end
    function soul:GetRelationship() return 0 end
    -- Buffs by guid, as the game keeps them: AddBuff returns an instance the caller may
    -- hand back to RemoveBuff; RemoveAllBuffsByGuid clears every instance of one.
    soul.buffs = {}
    function soul:AddBuff(guid) self.buffs[#self.buffs + 1] = guid; return #self.buffs end
    function soul:RemoveBuff(inst) self.buffs[inst] = false end
    function soul:RemoveAllBuffsByGuid(guid) for i, g in ipairs(self.buffs) do if g == guid then self.buffs[i] = false end end end
    function soul:HasBuff(guid) for _, g in ipairs(self.buffs) do if g == guid then return true end end; return false end
    return soul
end

local function actorComponent(e)
    local actor = {}
    function actor:IsDead() return not e.alive end
    function actor:IsUnconscious() return e.down == true end
    function actor:CanInteractWith() return true end
    function actor:GetCurrentAnimationState() return e.anim end
    -- The game's bind puts an inventory row on the body; here it is remembered, so a
    -- case can ask what a beggar wears (host "worn").
    function actor:EquipInventoryItem(wuid)
        if type(wuid) ~= "table" or wuid.class == nil then error("EquipInventoryItem: bad wuid") end
        e.worn = e.worn or {}
        e.worn[#e.worn + 1] = wuid.class
        return nil
    end
    -- The game's own Talk: true while some quest's holder menu is enterable on this
    -- soul (Active and not handed, and our talking global down), as the engine
    -- decides it from the holder XML's entry condition.
    function actor:CanTalk() return Engine.holderMenuOpen(e.name) end
    function actor:RequestDialog(userId, role, a, b, c) return Engine.requestDialog(e) end
    -- Dirt and blood on everything worn, <-1..1> deltas as the game's AddDirt/AddBlood take.
    e.dirt, e.blood = 0, 0
    function actor:AddDirt(d) e.dirt = math.max(0, math.min(1, e.dirt + d)) end
    function actor:AddBlood(d) e.blood = math.max(0, math.min(1, e.blood + d)) end
    -- A clothing preset put on the soul (the game's bind takes the preset guid); remembered
    -- by guid, in order, for a case to read (emit.cpp, WatchDress).
    e.presets = {}
    function actor:EquipClothingPreset(guid)
        if type(guid) ~= "string" or guid == "" then error("EquipClothingPreset: bad guid") end
        e.presets[#e.presets + 1] = guid
        return true
    end
    return actor
end

function Engine.soul(name, opts)
    opts = opts or {}
    local e = Engine.entities[name]
    if e ~= nil then return e end
    Engine.entitySeq = Engine.entitySeq + 1
    e = { name = name, id = Engine.entitySeq, alive = true, down = false, anim = opts.anim or "",
          pos = opts.pos or { x = 0, y = 0, z = 0 } }
    e.soul = soulComponent(e)
    e.actor = actorComponent(e)
    e.inventory = Inventory.new(e)
    function e:GetName() return self.name end
    -- A Stash's inventory id (behaviors.md §6.3): the same inventory every entity here has.
    function e:GetInventoryToOpen() return self.inventory:GetId() end
    function e:GetWorldPos() return { x = self.pos.x, y = self.pos.y, z = self.pos.z } end
    function e:SetWorldPos(p) self.pos = { x = p.x, y = p.y, z = p.z } end
    -- The way the entity faces, a unit vector (+y until a case turns it: e.dir).
    e.dir = opts.dir or { x = 0, y = 1, z = 0 }
    function e:GetDirectionVector() return { x = self.dir.x, y = self.dir.y, z = self.dir.z } end
    -- Sounds played on the entity, by ATL trigger name in the order they were asked for
    -- (Sound.GetAudioTriggerID hands the name back as the id, below).
    e.sounds = {}
    function e:GetDefaultAuxAudioProxyID() return 1 end
    function e:ExecuteAudioTrigger(id, _) self.sounds[#self.sounds + 1] = id end
    function e:SetTimer(id, ms) Engine.setTimer(ms, function() if Player.OnTimer then Player.OnTimer(self, id, ms) end end) end
    -- Clips the animation set knows and their lengths (Engine.clipLengths); the mod's
    -- own shape clips load only where a case says they do.
    function e:GetAnimationLength(slot, clip) return Engine.clipLengths[clip] or 0 end
    -- Clips started and layers stopped, in order, for the mouth cases (emit.cpp, LipApply).
    e.anims = {}
    e.stops = {}
    function e:StartAnimation(slot, clip, layer, blend, speed, loop)
        self.anims[#self.anims + 1] = { clip = clip, layer = layer, blend = blend, speed = speed, at = Engine.now }
        return true
    end
    function e:StopAnimation(slot, layer) self.stops[#self.stops + 1] = layer end
    -- The frame a borrowed shape is held at and the speed it is slowed to (emit.cpp,
    -- LipMouthTick), kept on the clip last started.
    function e:SetAnimationTime(slot, layer, at)
        local last = self.anims[#self.anims]
        if last ~= nil and last.layer == layer then last.held = at end
    end
    function e:SetAnimationSpeed(slot, layer, speed)
        local last = self.anims[#self.anims]
        if last ~= nil and last.layer == layer then last.speedSet = speed end
    end
    Engine.entities[name] = e
    return e
end

-- ---------------------------------------------------------------------------
-- The player. One entity with a human component the dialogue state lives on.
function Engine.makePlayer()
    local p = Engine.soul("dude", { pos = { x = 0, y = 0, z = 0 } })
    p.human = {}
    function p.human:IsInDialog() return Engine.dialog.open end
    function p.human:IsWeaponDrawn() return false end
    function p.human:InterruptDialogs() Engine.closeDialog("interrupted") end
    function p.human:Dismount() Engine.mounted = false end
    function p.human:IsMounted() return Engine.mounted == true end
    -- The thing put down on the ground at a spot entity (a leave's, a return's, a find's
    -- keepsake): out of the pocket and onto Engine.ground by class, where a case can find it.
    function p.human:PlaceItem(wuid, spotId, _)
        if type(wuid) ~= "table" then error("PlaceItem: no such item") end
        Engine.inventories[wuid.inv]:DeleteItemOfClass(wuid.class, 1)
        Engine.ground[#Engine.ground + 1] = { class = wuid.class, spot = spotId }
    end
    p.inventory:CreateItem(Engine.moneyClass, 100, 63807) -- 6380.7 groschen, the Windows laptop's purse
    return p
end

Engine.moneyClass = "5ba5b5c1-money-0000-0000-000000000000"

-- ---------------------------------------------------------------------------
-- The dialogue loop, as talk.xml autoselects it. While the talking global is up a
-- beat boundary picks: the quest beat whose code is armed (fires its port, exit
-- script QuestBeat), else the menu when the face code is the choose code (presented
-- and waited on until pick()), else a face beat (exit script Beat()). With the
-- global down nothing is enterable and the session ends.
Engine.dialog = { open = false, npc = nil, menu = false, beats = 0 }
Engine.beatMillis = 6000
Engine.questBeatMillis = 2500
Engine.ports = {}
-- Skald's view of each quest, from the ports the beats fire: none/active/done, and
-- handed from the holder menu's handover port.
Engine.questState = {}

function Engine.requestDialog(npc)
    if Engine.dialog.open then return false end
    if Engine.gvar(Moonshine.talkVar) ~= 1 then
        -- Our loop is not enterable; the game's own Talk would open the holder menu
        -- if it had one, which the harness drives through holderGave() instead.
        return false
    end
    Engine.dialog.open = true
    Engine.dialog.npc = npc
    Engine.dialog.menu = false
    Engine.dialog.beats = 0
    Engine.log("[harness] dialogue opened on " .. npc.name)
    Engine.setTimer(300, Engine.boundary)
    return true
end

function Engine.closeDialog(why)
    if not Engine.dialog.open then return end
    Engine.dialog.open = false
    Engine.dialog.menu = false
    Engine.log("[harness] dialogue closed, " .. tostring(why) .. ", after " .. Engine.dialog.beats .. " beats")
end

local function codeName(code)
    for key, c in pairs(Moonshine.questCodes) do
        if c == code then return key end
    end
    return nil
end

function Engine.boundary()
    if not Engine.dialog.open or Engine.dialog.menu then return end
    if Engine.gvar(Moonshine.talkVar) ~= 1 then
        Engine.closeDialog("talking global down")
        return
    end
    local code = Engine.gvar(Moonshine.questVar)
    if code ~= 0 then
        local name = codeName(code)
        if name == nil then
            -- No sequence matches: the graph stalls on a code it does not know, as it
            -- would in the game. Try again next beat; the Lua's patience drops it.
            Engine.log("[harness] no sequence for quest code " .. tostring(code))
            Engine.setTimer(Engine.beatMillis, Engine.boundary)
            return
        end
        Engine.dialog.beats = Engine.dialog.beats + 1
        Engine.setTimer(Engine.questBeatMillis, function()
            Engine.firePort(name)
            Moonshine.QuestBeat(code)
            Engine.setTimer(0, Engine.boundary)
        end)
        return
    end
    if Engine.gvar(Moonshine.faceVar) == Moonshine.chooseCode then
        Engine.dialog.menu = true
        Engine.log("[harness] menu presented")
        return
    end
    Engine.dialog.beats = Engine.dialog.beats + 1
    Engine.setTimer(Engine.beatMillis, function()
        Moonshine.Beat()
        Engine.setTimer(0, Engine.boundary)
    end)
end

-- The player confirms a row of the presented menu.
function Engine.pick(row)
    if not Engine.dialog.open or not Engine.dialog.menu then
        error("pick(" .. tostring(row) .. ") with no menu presented")
    end
    Engine.dialog.menu = false
    Moonshine.Picked(row)
    if row == Moonshine.menuRows then
        Engine.closeDialog("EndDialogue row")
    else
        Engine.setTimer(0, Engine.boundary)
    end
end

-- A quest port, as the quest XML takes it: "<id> accept" sets the quest Active,
-- "<id> complete*" sets it Done. Recorded in order for the cases.
function Engine.firePort(name)
    Engine.ports[#Engine.ports + 1] = name
    local id, command = string.match(name, "^(%S+)%s+(%S+)$")
    local qs = Engine.questState[id] or { state = "none", handed = false }
    if command == "accept" then qs.state = "active"
    -- The giver told and paid, Henry sent on: the graph's onwardState goes Active and
    -- the onward soul's one-sequence Talk (q_<id>_onward.xml) opens off it.
    elseif command == "onward" then qs.onward = "active"
    elseif string.sub(command, 1, 8) == "complete" then qs.state = "done" end
    Engine.questState[id] = qs
    Engine.log("[harness] port " .. name)
end

-- The holder's menu is enterable while Port('active') AND !Port('handed') and the
-- talking global is down (README, *Two enterable dialogues on one request*).
function Engine.holderMenuOpen(soulName)
    if Engine.gvar(Moonshine.talkVar) == 1 then return false end
    for id, q in pairs(Moonshine.quests) do
        local qs = Engine.questState[id]
        if q.target == soulName then
            if qs ~= nil and qs.state == "active" and not qs.handed then return true end
        end
        -- The onward soul's door: Port('active') off onwardState, !Port('handed') off its
        -- own handover, which is not the holder's.
        if q.onward == soulName and q.onward ~= "" then
            if qs ~= nil and qs.state == "active" and qs.onward == "active" and not qs.onwardHanded then return true end
        end
    end
    return false
end

-- The holder gave in on a row of the game's own menu: the handover port and the
-- row's exit script, which is the same Lua handover the app used to command.
function Engine.holderGave(id, how)
    local qs = Engine.questState[id] or { state = "none", handed = false }
    qs.handed = true
    Engine.questState[id] = qs
    Engine.ports[#Engine.ports + 1] = id .. " handover"
    Moonshine.QuestHolderGave(id, how)
end

-- The receiver's one-sequence Talk for a letter; `how` is an interrogation's answer row
-- (truth, pinned, covered, evaded, caught), nil for the plain hand row.
function Engine.delivered(id, how)
    local qs = Engine.questState[id] or { state = "none", handed = false }
    qs.handed = true
    Engine.questState[id] = qs
    Engine.ports[#Engine.ports + 1] = id .. " handover"
    Moonshine.QuestDelivered(id, how)
end

-- The one-sequence Talk on the soul an ask's word is carried on to (q_<id>_onward.xml's
-- hand row): its own handover port, and the row's exit script.
function Engine.carried(id)
    local qs = Engine.questState[id] or { state = "none", handed = false }
    qs.onwardHanded = true
    Engine.questState[id] = qs
    Engine.ports[#Engine.ports + 1] = id .. " onward handover"
    Moonshine.QuestCarried(id)
end

-- ---------------------------------------------------------------------------
-- The saved game. OnSave fills a table; serialise() is the engine's writer with its
-- one rule: a string of 65,536 bytes or more writes fine and never loads again.
Engine.saveLimit = 65536

local function serialise(v, out, seen)
    local t = type(v)
    if t == "string" then out[#out + 1] = string.format("%q", v)
    elseif t == "number" then out[#out + 1] = string.format("%.17g", v)
    elseif t == "boolean" then out[#out + 1] = tostring(v)
    elseif t == "nil" then out[#out + 1] = "nil"
    elseif t == "table" then
        if seen[v] then error("cycle in save") end
        seen[v] = true
        out[#out + 1] = "{"
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = k end
        table.sort(keys, function(a, b)
            if type(a) == type(b) then return a < b end
            return type(a) == "number"
        end)
        for _, k in ipairs(keys) do
            out[#out + 1] = "["
            serialise(k, out, seen)
            out[#out + 1] = "]="
            serialise(v[k], out, seen)
            out[#out + 1] = ","
        end
        out[#out + 1] = "}"
        seen[v] = nil
    else
        error("cannot save a " .. t)
    end
end

local function longest(v, seen)
    seen = seen or {}
    local t = type(v)
    if t == "string" then return #v end
    if t ~= "table" or seen[v] then return 0 end
    seen[v] = true
    local n = 0
    for k, x in pairs(v) do n = math.max(n, longest(k, seen), longest(x, seen)) end
    return n
end

-- Save: the player's hooks run as the engine runs them (OnSaveAI then OnSave), the
-- table is written to `path` if given, and the text returned.
function Engine.save(path)
    local save = {}
    if Player.OnSaveAI then Player.OnSaveAI(player, save) end
    if Player.OnSave then Player.OnSave(player, save) end
    -- The quest graph's own state rides in the engine's save, as it does in the game;
    -- under a key of the harness's, beside whatever the mod wrote.
    save.__harnessQuestState = Engine.questState
    local out = {}
    serialise(save, out, {})
    local text = "return " .. table.concat(out)
    if path then
        local f = assert(io.open(path, "wb"))
        f:write(text)
        f:close()
    end
    Engine.lastSave = { text = text, longest = longest(save) }
    return text
end

-- Load: a world arrives (OnInit resets the store), then OnLoad with the saved table,
-- unless a string in it is at or over the limit, in which case the engine's "Game
-- load failed" is what happens and nothing is called.
function Engine.load(textOrPath)
    local text = textOrPath
    if string.sub(text, 1, 7) ~= "return " then
        text = readFile(textOrPath)
        if text == nil then return false, "no save at " .. tostring(textOrPath) end
    end
    local chunk, err = loadstring(text, "save")
    if chunk == nil then return false, err end
    local saved = chunk()
    if longest(saved) >= Engine.saveLimit then
        Engine.log("[harness] Game load failed: a string of " .. longest(saved) .. " bytes")
        return false, "Game load failed"
    end
    Engine.arrive()
    Engine.questState = saved.__harnessQuestState or {}
    saved.__harnessQuestState = nil
    if Player.OnLoadAI then Player.OnLoadAI(player, saved) end
    if Player.OnLoad then Player.OnLoad(player, saved) end
    return true
end

-- A world arriving with no save of ours in it: OnInit, OnSpawn, and the rules'
-- start-of-level, which is what a save from before the mod looks like.
function Engine.arrive()
    Engine.closeDialog("world changed")
    Engine.globals = {}
    if Player.OnInit then Player.OnInit(player) end
    if Player.OnSpawn then Player.OnSpawn(player) end
    if SinglePlayer and SinglePlayer.Server and SinglePlayer.Server.OnStartLevel then
        SinglePlayer.Server.OnStartLevel(SinglePlayer.Server)
    end
end

-- ---------------------------------------------------------------------------
-- The player looks at a soul (GetActions runs every frame) and presses the mod's
-- prompt. look() returns the actions offered; begin() presses ours.
function Engine.look(npc)
    local class = _G[npc.class or "NPC"]
    return class.GetActions(npc, player, false)
end

function Engine.begin(npc)
    local actions = Engine.look(npc)
    for _, a in ipairs(actions) do
        if a.fields and a.fields.func then
            a.fields.func(npc, player, 0)
            return true
        end
    end
    return false, "no mod prompt on " .. npc.name .. " (" .. #actions .. " actions)"
end

-- ---------------------------------------------------------------------------
-- The globals.
function Engine.installGlobals()
    System = {
        LogAlways = function(text) Engine.log(text) end,
        Log = function(text) Engine.log(text) end,
        LoadTextFile = function(path) return Engine.loadTextFile(path) end,
        GetCurrAsyncTime = function() return Engine.seconds() end,
        GetEntityByName = function(name) return Engine.entities[name] end,
        GetCVar = function(name) return Engine.cvars[name] end,
        SetCVar = function(name, value) Engine.cvars[name] = value end,
        -- The last frame's length in seconds; 60 fps unless a case says otherwise.
        GetFrameTime = function() return Engine.frameTime or (1 / 60) end,
        -- The console: a `goto X Y Z` moves the player, as devmode's does (Engine.consoled
        -- keeps every line typed).
        ExecuteCommand = function(line)
            Engine.consoled[#Engine.consoled + 1] = tostring(line)
            local x, y, z = string.match(tostring(line), "^goto%s+([-%d.]+)%s+([-%d.]+)%s+([-%d.]+)")
            if x ~= nil and player ~= nil then player:SetWorldPos({ x = tonumber(x), y = tonumber(y), z = tonumber(z) }) end
        end,
        -- A ray between two points meets nothing (true) unless a case has put a wall up
        -- (Engine.walled), as the game's bind answers for its physics.
        RayTraceCheck = function(from, to, skip1, skip2)
            Engine.rays[#Engine.rays + 1] = { from = from, to = to, skip1 = skip1, skip2 = skip2 }
            return Engine.walled ~= true
        end,
        -- A point is in the camera's frame unless a case has Henry looking away
        -- (Engine.framed false), as the game's bind answers for its frustum.
        IsPointVisible = function(p)
            Engine.framedAsked[#Engine.framedAsked + 1] = p
            return Engine.framed ~= false
        end,
        -- The console's own commands: the body is Lua with %% for the words typed after
        -- the name, each quoted. Engine.console(name, words...) plays a typed line.
        AddCCommand = function(name, body, help) Engine.commands[name] = { body = body, help = help } end,
        GetViewport = function() return 0, 0, 3840, 1600 end,
        GetTerrainElevation = function() return 0 end,
        -- The spec is kept on the entity (e.spec) so a case can read what a chest was
        -- spawned with: its class, position, lock and key.
        SpawnEntity = function(spec)
            local e = Engine.soul(spec.name or ("spawn" .. Engine.entitySeq), { pos = spec.position })
            e.spec = spec
            return e
        end,
        RemoveEntity = function() end,
        Quit = function() Engine.quit = true end,
    }
    Engine.cvars = { sys_PakPriority = "0" }
    Script = {
        SetTimer = function(ms, fn) Engine.setTimer(ms, fn) end,
        -- The game loads a pak path; here a file of that name beside the mod file, which
        -- is where the unzipped Data pak keeps them (Scripts/mods/moonshine_alms.lua).
        LoadScript = function(path)
            local file = string.match(tostring(path), "[^/\\]+$")
            if Engine.modDir == nil or file == nil then error("LoadScript: no mod dir for " .. tostring(path)) end
            local chunk, err = loadfile(Engine.modDir .. "/" .. file)
            if chunk == nil then error("LoadScript: " .. tostring(err)) end
            chunk()
            return true
        end,
    }
    Variables = {
        SetGlobal = function(name, value) Engine.globals[name] = value end,
        GetGlobal = function(name) return Engine.globals[name] end,
    }
    Calendar = {
        GetWorldTime = function() return Engine.worldTime() end,
        GetWorldHourOfDay = function() return Engine.hour end,
        -- The clock put forward (what player.lua does for a wait): the world seconds move
        -- by the difference, and the hour with them.
        SetWorldTime = function(t)
            local ahead = t - Engine.worldTime()
            Engine.worldBase = Engine.worldBase + ahead
            Engine.hour = (Engine.hour + ahead / 3600) % 24
        end,
    }
    EnvironmentModule = { GetRainIntensity = function() return Engine.rain end }
    EntityUtils = { GetName = function(e) return e.name end }
    Game = {
        ShowItemsTransfer = function() end,
        SendInfoText = function(text) Engine.infoTexts[#Engine.infoTexts + 1] = text end,
        QuickSave = function() end,
        QuickLoad = function() end,
        SaveGameViaResting = function() end,
        IsLoadingEngineSaveGame = function() return false end,
    }
    -- calls is the names in order; args the arguments of each, by the same index, so a case
    -- can read what SetSubtitles was given (the text and the speaker row).
    Engine.hud = { rows = nil, calls = {}, args = {} }
    UIAction = {
        SetArray = function(_, _, name, values) if name == "SequenceList" then Engine.hud.rows = values end end,
        CallFunction = function(_, _, name, ...)
            Engine.hud.calls[#Engine.hud.calls + 1] = name
            Engine.hud.args[#Engine.hud.calls] = { ... }
        end,
        SetVisible = function() end,
    }
    Shops = {
        DoMoneyTransaction = function(soulId, deci, _)
            -- Towards the soul: the player's purse to theirs.
            local to
            for _, e in pairs(Engine.entities) do if e.id == soulId then to = e end end
            if to == nil then error("DoMoneyTransaction: no soul " .. tostring(soulId)) end
            player.inventory:MoveItemOfClass(to.inventory:GetId(), Engine.moneyClass, deci)
        end,
    }
    ItemUtils = { itemIDs = { money = Engine.moneyClass } }
    ItemManager = {
        CreateItem = function() error("[Error] Lua error") end,
        GetItem = Engine.itemOf,
        RemoveItem = Engine.removeItem,
    }
    ActionMapManager = {
        LoadFromXML = function() return true end,
        EnableActionMap = function() return true end,
        SetActionListener = function() return true end,
    }
    XGenAIModule = {}
    Sound = { GetAudioTriggerID = function(name) return name end }
    Particle = { SpawnEffect = function() end }
    HUD = {}
    GameUtils = {}
    DialogUtils = {}
    DialogModule = {}
    Database = {}
    GameToken = {}
    inr_talk = "inr_talk"
    g_localActor = nil

    -- The interactor builder the shipped GetActions use.
    local setters = { hint = true, action = true, uiOrder = true, interaction = true, func = true, enabled = true }
    Action = function()
        local a = { fields = {} }
        local mt = { __index = function(_, k)
            if not setters[k] then return nil end
            return function(self, v) self.fields[k] = v; return self end
        end }
        return setmetatable(a, mt)
    end
    AddInteractorAction = function(output, _, action) output[#output + 1] = action end

    -- The entity classes the mod patches. The base GetActions offers the game's own
    -- Talk when the soul can talk, as the engine does for any enterable dialogue.
    local function baseActions(npc, user, firstFast)
        local out = {}
        if npc.actor:CanTalk(user.id) then out[#out + 1] = { action = "talk", base = true } end
        return out
    end
    NPC = { GetActions = baseActions }
    NPC_Female = { GetActions = baseActions }
    NPC_NAI = { GetActions = baseActions }

    Player = {
        OnAction = function(self, action, activation, value) end,
        OnSave = function(self, save) end,
        OnLoad = function(self, saved) end,
        OnSaveAI = function(self, save) end,
        OnLoadAI = function(self, saved) end,
        OnInit = function(self) end,
        OnSpawn = function(self) end,
        OnReset = function(self) end,
        OnTimer = function(self, id, ms) end,
        Client = { OnUpdate = function(self, dt) end },
    }
    SinglePlayer = { Server = { OnStartLevel = function(self) end } }
    player = Engine.makePlayer()
end

-- ---------------------------------------------------------------------------
-- Boot: globals, then the emitted Lua, then a world arriving, then enough clock
-- for InstallWhenReady to settle. Returns the Moonshine table.
function Engine.boot(modPath, opts)
    opts = opts or {}
    Engine.root = opts.root
    Engine.user = opts.user
    Engine.echo = opts.echo == true
    if opts.log then
        Engine.logPath = opts.log
        Engine.logFile = assert(io.open(opts.log, opts.append and "ab" or "wb"))
    end
    Engine.installGlobals()
    Engine.modDir = string.match(modPath, "^(.*)[/\\][^/\\]*$") or "."
    local chunk, err = loadfile(modPath)
    if chunk == nil then error("the mod did not parse: " .. tostring(err)) end
    chunk()
    Engine.arrive()
    Engine.advance(100)
    return Moonshine
end

-- A fresh engine state for another case: the clock, log memory, entities, dialogue
-- and store, with the mod's own runtime tables put back to their loaded values.
function Engine.reset()
    Engine.now = 0
    Engine.frameTime = nil
    Engine.timers = {}
    Engine.lines = {}
    Engine.globals = {}
    Engine.entities = {}
    Engine.inventories = {}
    Engine.ground = {}
    Engine.ports = {}
    Engine.questState = {}
    Engine.dialog = { open = false, npc = nil, menu = false, beats = 0 }
    Engine.hud = { rows = nil, calls = {}, args = {} }
    Engine.files = {}
    Engine.infoTexts = {}
    Engine.rays = {}
    Engine.walled = false
    Engine.framed = true
    Engine.framedAsked = {}
    Engine.consoled = {}
    Engine.mounted = false
    player = Engine.makePlayer()
    Moonshine.active = false
    Moonshine.npc = nil
    Moonshine.user = nil
    Moonshine.turn = 0
    Moonshine.hooked = 0
    Moonshine.consoleGen = 0
    Moonshine.consoleAt = -1
    Moonshine.words = nil
    Moonshine.inbox = nil
    Moonshine.missing = {}
    Moonshine.logged = {}
    Moonshine.holderGranted = {}
    Moonshine.questQueue = {}
    Moonshine.questCode = 0
    Moonshine.options = {}
    Moonshine.choosing = false
    Moonshine.menuWait = false
    Moonshine.menuUp = false
    Moonshine.menuDrawn = 0
    Moonshine.optionsTotal = nil
    Moonshine.ending = false
    Moonshine.more = false
    Moonshine.streaming = false
    Moonshine.streamArmed = false
    Moonshine.hold = 0
    Moonshine.ticks = 0
    -- The voice and the mouth, which Leave() clears in the game and a case may not reach.
    Moonshine.voiced = false
    Moonshine.spoken = false
    Moonshine.henryUntil = 0
    Moonshine.lipKey = nil
    Moonshine.lipSeq = 1
    Moonshine.lipWho = nil
    Moonshine.lipGen = Moonshine.lipGen + 1
    Moonshine.lipEnt = nil
    Moonshine.lipPlan = nil
    Moonshine.lipsync = false
    Moonshine.lipMouth = {}
    Moonshine.lipQueue = {}
    Moonshine.lipMouthAt = 1
    Moonshine.lipT0 = nil
    Moonshine.lipShape = nil
    Moonshine.lipHoldLeft = 0
    Moonshine.lipShapes = 0
    Moonshine.lipRefused = 0
    Moonshine.lipLate = 0
    Moonshine.mouthOwn = nil
    Engine.clipLengths = {}
    Moonshine.menuHeld = false
    Moonshine.watching = false
    Moonshine.watchTicked = nil
    Moonshine.watchDoubt = false
    Moonshine.hailed = {}
    Moonshine.entered = {}
    Moonshine.lookAt = nil
    Moonshine.lookWhen = 0
    Moonshine.stateSoul = nil
    Moonshine.state = Moonshine.FreshState()
    Moonshine.flagsSynced = false
    Engine.arrive()
end

return Engine
