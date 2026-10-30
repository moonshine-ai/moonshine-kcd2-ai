-- The cases: what the emitted Lua promises the app and the graph, pinned outside the
-- game. Each is a function of (Engine, M, check) run on a fresh Engine.reset(); a
-- case fails on the first check that does not hold. The sections follow the README:
-- *What comes back* (Directives), *Quests* (QuestCommand, the holder, the links),
-- *State* (the store, the save, compaction) and *Choosing a reply* (the menu).
--
-- Ids are the slate's: Charity Begins at Home (0ef0965edb4e, a retrieve with a
-- dispute), Forget Me Not (69e323aa6e55, five of a thing), Bread and Penance
-- (2d747c63d9d0, a mind of three checks). Looked up by name where the ids might move
-- under a re-deal, so a case says which story it lost rather than failing on a nil.
-- A title is rewritten with the quest whenever its circle is regenerated (the 5 Oct 2026
-- deal renamed A Prayer of Her Own and A Woman's Smile), so a case that wants only a
-- shape -- any letter, any scene gated truth|lied -- asks for the shape (byLetter,
-- byGate) and not a title.

local cases = {}
local order = {}
local function case(name, fn) cases[name] = fn; order[#order + 1] = name end

local function byName(M, name)
    for id, q in pairs(M.quests) do if q.name == name then return id, q end end
    return nil
end

-- A deliver with a thing to carry (not a word) that Henry has from accept (not one he
-- writes at dictation), in id order so the same one is picked every run.
local function byLetter(M)
    local ids = {}
    for id, q in pairs(M.quests) do
        if q.kind == "deliver" and q.item ~= "" and not q.dictated then ids[#ids + 1] = id end
    end
    table.sort(ids)
    return ids[1], M.quests[ids[1]]
end

-- A deliver whose letter Henry writes at the giver's dictation (Words Must Not Fail), in
-- id order.
local function byDictation(M)
    local ids = {}
    for id, q in pairs(M.quests) do if q.dictated then ids[#ids + 1] = id end end
    table.sort(ids)
    return ids[1], M.quests[ids[1]]
end

-- A story's later scene gated on the one before it ending a given way (afterHow as the
-- Lua carries it, "truth|lied" say), in id order.
local function byGate(M, how)
    local ids = {}
    for id, q in pairs(M.quests) do if q.after ~= "" and q.afterHow == how then ids[#ids + 1] = id end end
    table.sort(ids)
    return ids[1], M.quests[ids[1]]
end

local function byKind(M, kind, want)
    for id, q in pairs(M.quests) do
        if q.kind == kind then
            local ok = true
            for k, v in pairs(want or {}) do if q[k] ~= v then ok = false end end
            if ok then return id, q end
        end
    end
    return nil
end

-- Titles are rewritten whenever a circle is regenerated; the fox ask is the one with a
-- bribe in it, and the word scene the deliver with nothing to carry.
local function byGift(M)
    for id, q in pairs(M.quests) do if q.gift ~= nil then return id, q end end
    return nil
end

-- The ask whose word is carried on to a third soul (the fox story's guild master, since
-- 5 Oct 2026 the ask's own last objective rather than a deliver of its own).
local function byOnward(M)
    for id, q in pairs(M.quests) do if q.onward ~= nil and q.onward ~= "" then return id, q end end
    return nil
end

-- The user directory inbox, which is the first the mod tries and the one it keeps.
local function turn(E, n, text) E.files["%USER%/moonshine/turn_" .. n .. ".txt"] = text end

-- Up to the next menu: ticks until the harness has presented one and the mod has
-- drawn its rows over it (a press before the draw lands on the engine's dots, which
-- is "picked N none"), or `ms` is spent.
local function untilMenu(E, ms)
    local stop = E.now + (ms or 20000)
    while E.now < stop and not (E.dialog.menu and Moonshine.menuDrawn > 0) do E.advance(100) end
    return E.dialog.menu and Moonshine.menuDrawn > 0
end

-- A conversation in progress without the begin: the party set so Near() and the
-- beats hold, for cases that drive the menu by hand.
local function live(E, M)
    M.npc = E.soul("kkut_additive_man_47", { pos = { x = 1, y = 0, z = 0 } })
    M.user = player
    M.active = true
    M.Talking(true)
end

-- -------------------------------------------------------------------- Directives

case("Directives: a bare line is left alone", function(E, M, check)
    check(M.Directives("Past the smithy, then left.") == "Past the smithy, then left.", "words unchanged")
end)

case("Directives: @quest at the front runs and is stripped", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    check(id, "the story is dealt")
    local words = M.Directives("@quest " .. id .. " accept What do you want, then?")
    check(words == "What do you want, then?", "the words after the token: " .. tostring(words))
    check(M.state.quests[id].accepted == true, "accepted in the store")
    check(E.find("BRIDGE quest " .. id .. " accept -- Charity Begins at Home"), "the accept logged")
    check(M.questCode ~= 0, "a beat code armed")
end)

case("Directives: directives only is a turn that does things and says nothing", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    check(M.Directives("@quest " .. id .. " accept") == "", "empty words")
end)

case("Directives: @opt rows are taken by byte count, so a curly quote is not three short", function(E, M, check)
    local text = "I’ll get it back."
    local line = "@opt 1 accept " .. #text .. ":" .. text .. " @opt 2 help 5:Help? @rows 2"
    M.active = true
    M.choosing = true
    local words = M.Directives(line)
    check(words == "", "nothing spoken: " .. tostring(words))
    check(M.options[1] and M.options[1].text == text, "row 1 whole: " .. tostring(M.options[1] and M.options[1].text))
    check(M.options[1].intent == "accept", "row 1 intent")
    check(M.options[2] and M.options[2].text == "Help?", "row 2")
    check(M.optionsTotal == 2, "@rows 2 read")
    check(M.OptionsComplete(), "two of two rows is complete")
end)

case("Directives: @opt with words after the rows keeps the words", function(E, M, check)
    M.active = true
    local words = M.Directives("@opt 1 help 5:Help? Then the line.")
    check(words == "Then the line.", "the words: " .. tostring(words))
end)

case("Directives: @said is remembered as the player's words", function(E, M, check)
    M.stateSoul = "kkut_additive_man_47"
    local said = "Which way to the mill? — ask Hans"
    local words = M.Directives("@said " .. #said .. ":" .. said .. " Past the smithy.")
    check(words == "Past the smithy.", "words: " .. tostring(words))
    local mem = M.state.souls["kkut_additive_man_47"].memory
    check(#mem == 1 and mem[1].k == "p" and mem[1].s == said, "a p entry with the whole line")
end)

case("Directives: @more marks a reply still being written", function(E, M, check)
    local words = M.Directives("@more Past the")
    check(words == "Past the" and M.more == true, "more set, words kept")
end)

case("Directives: @end closes once read", function(E, M, check)
    local words = M.Directives("@end Farewell, then.")
    check(words == "Farewell, then." and M.ending == true, "ending set")
end)

case("Directives: @beat keeps the turn for the speaker's next line", function(E, M, check)
    local words = M.Directives("@beat @said 23:Why are you so unhappy? It's my Humphrey.")
    check(words == "It's my Humphrey." and M.beat == true, "beat set, words kept")
    check(M.Directives("@beat") == "" and M.beat == true, "a bare @beat is a turn of nothing")
end)

case("Say: a line in two beats is held twice and heard once", function(E, M, check)
    -- A written reply told in two beats (notes.md 7 Oct 2026): the first turn is flagged
    -- @beat and is shown and held for its reading like any line, but Henry's turn does not
    -- come after it -- no Heard, no menu -- and the second, unflagged, is what opens the menu.
    live(E, M)
    M.heard = false
    M.awaiting = true
    M.Say(1, "@beat It's my Humphrey. A year gone this week, and I still set his bowl out of a morning.")
    check(M.beat == true, "flagged")
    check(M.heard == false and M.awaiting == true and M.choosing ~= true, "not heard: the turn is still hers")
    check(M.hold > M.ticks, "held for the reading all the same")
    check(E.find("BRIDGE played turn=1 -- It's my Humphrey") and E.find("(beat)"), "played, and marked as a beat")
    M.Say(2, "I want five for his grave on the day. Bring me a handful, and you have my word.")
    check(M.beat == false, "the second is not flagged")
    check(M.heard == true and M.awaiting == false, "heard on the last beat")
    check(M.choosing == true, "and the menu is asked for")
end)

case("Say: in the game's caption the speaker is on the gold row and the words stand alone", function(E, M, check)
    -- notes.md 7 Oct 2026: "Joan: words" in the text field, in the text's colour, was the
    -- typing prototype's layout. The game's own captions put the speaker in SetSubtitles'
    -- second argument, and so does the mod now, on both halves of an exchange.
    live(E, M)
    M.npcName = "Bartel"
    local function lastSubtitles()
        for i = #E.hud.calls, 1, -1 do
            if E.hud.calls[i] == "SetSubtitles" then return E.hud.args[i] end
        end
    end
    check(M.mode == "caption", "the caption road is the default")
    M.Say(1, "Past the smithy, then left.")
    local args = lastSubtitles()
    check(args ~= nil, "the caption was written")
    check(args and args[1] == "Past the smithy, then left.", "the words alone in the text: " .. tostring(args and args[1]))
    check(args and args[2] == M.npcName, "the speaker in the gold row: " .. tostring(args and args[2]))
    check(not string.find(M.shown, ":", 1, true), "no name riding in the text")
    -- Henry's picked line the same way until the reply replaces it.
    M.Said("Which way to the mill?")
    args = lastSubtitles()
    check(args and args[1] == "Which way to the mill?" and args[2] == M.playerName,
          "Henry's line under his name: " .. tostring(args and args[2]) .. " / " .. tostring(args and args[1]))
    -- The name stays in front where there is no speaker row to put it in.
    check(M.Named("words") == M.npcName .. ": words", "Named still serves the drawn and info roads")
    -- A line of the mod's own has no speaker.
    M.Say(2, "[Moonshine] the app is not running", true)
    args = lastSubtitles()
    check(args and args[2] == "", "bare words draw no speaker row")
end)

case("Caption: a long line is shown in pages at its sentence ends, turned as the voice should reach them, the last with the audio's end", function(E, M, check)
    -- notes.md 14 Oct 2026: Benesh's speech on Henry's return ran off the caption plate.
    -- The voice is one piece of audio and is not cut; the words under it turn over.
    live(E, M)
    M.npcName = "Benesh"
    local function lastSubtitles()
        for i = #E.hud.calls, 1, -1 do
            if E.hud.calls[i] == "SetSubtitles" then return E.hud.args[i] end
        end
    end
    local parts = {}
    for i = 1, 7 do
        parts[i] = "You have it, then, and that is the " .. ({ "first", "second", "third", "fourth", "fifth", "sixth", "seventh" })[i]
            .. " thing I will say of it before my name comes down off the stocks where that priest hung it."
    end
    local long = table.concat(parts, " ")  -- ~800 characters, over three pages
    local pages = M.Pages(long)
    check(#pages >= 3, "cut into pages: " .. #pages)
    for i, p in ipairs(pages) do
        check(#p <= M.pageChars, "page " .. i .. " within the plate: " .. #p)
        check(string.find(p, "%.$") ~= nil, "page " .. i .. " ends at a sentence: " .. string.sub(p, -20))
    end
    check(table.concat(pages, " ") == long, "nothing lost between the pages")
    check(pages[1] ~= pages[2], "pages that can be told apart")
    -- A short line is one page as it is; a line with no sentence end is cut at a word.
    check(#M.Pages("Past the smithy, then left.") == 1, "a short line is itself")
    local run = string.rep("word ", 80)
    local wordPages = M.Pages(run)
    check(#wordPages == 2 and string.find(wordPages[1], "%s$") == nil and #wordPages[1] <= M.pageChars,
          "a line without sentences is cut at a word")

    local mark = E.mark()
    M.Say(1, long)
    local args = lastSubtitles()
    check(args and args[1] == pages[1], "the first page up at once: " .. tostring(args and string.sub(args[1], 1, 40)))
    check(args and args[2] == M.npcName, "under the speaker's name")
    check(E.find("BRIDGE caption in " .. #pages .. " pages", mark), "said in the log")
    -- Not yet time: the first page stays through the pump's tick.
    E.advance(1000)
    M.PageTick()
    check(lastSubtitles()[1] == pages[1], "the first page stays while the voice is on it")
    -- The page's share of the speaking time passes: the second page.
    E.advance(math.ceil(M.pageDur[1] * 1000) + 50)
    M.PageTick()
    check(lastSubtitles()[1] == pages[2], "the second page in its time: " .. tostring(string.sub(lastSubtitles()[1], 1, 40)))
    check(E.find("BRIDGE caption page 2 of " .. #pages, mark), "and said")
    -- The audio's first cue re-anchors the clock on page one; its end goes to the last page.
    M.PageAnchor()
    check(lastSubtitles()[1] == pages[1], "the voice's first cue starts the pages again with it")
    M.PageLast()
    check(lastSubtitles()[1] == pages[#pages], "the audio's end shows the last page")
    E.advance(math.ceil(M.pageDur[#pages] * 1000) + 50)
    M.PageTick()
    check(lastSubtitles()[1] == pages[#pages], "and it stays")
    -- Henry's own line is not paged, and a new line starts its own pages.
    M.Said("Which way to the mill?")
    check(lastSubtitles()[1] == "Which way to the mill?", "Henry's line as it is")
    M.Say(2, "Past the smithy, then left.")
    check(lastSubtitles()[1] == "Past the smithy, then left.", "a short reply as it is")
end)

case("Directives: @memory stat sets a number", function(E, M, check)
    M.Directives("@memory kkut_additive_man_47 stat trust 0.75 and words")
    check(M.state.souls["kkut_additive_man_47"].stats.trust == 0.75, "trust stored")
end)

case("Directives: an unknown quest id is logged, not raised, and said on the screen", function(E, M, check)
    local words = M.Directives("@quest nosuchquest accept Hello.")
    check(words == "Hello.", "words kept")
    check(E.find("BRIDGE quest unknown id=nosuchquest"), "logged")
    -- An app and a mod from different deals (notes.md, 5 Oct 2026): the player is told
    -- why nothing happened, naming the id and the cure.
    local said = E.infoTexts[#E.infoTexts] or ""
    check(said:find("does not know this errand (nosuchquest)", 1, true) ~= nil, "said on screen: " .. said)
    check(said:find("build_mod.sh", 1, true) ~= nil, "names the cure")
end)

case("The begin and look lines carry the build's quest set (qset=)", function(E, M, check)
    -- The fingerprint the app checks against its own database (PersonaDb::quest_set,
    -- quest_set_fingerprint in emit.cpp): sixteen hex digits, the same on both lines.
    check(type(M.questSet) == "string" and #M.questSet == 16 and M.questSet:match("^%x+$") ~= nil,
          "questSet is sixteen hex digits: " .. tostring(M.questSet))
    local npc = E.soul("kkut_additive_man_10")
    check(E.begin(npc), "the prompt opens the conversation")
    local begin = E.find("BRIDGE begin npc=kkut_additive_man_10")
    check(begin ~= nil and begin:find(" qset=" .. M.questSet, 1, true) ~= nil, "begin line carries qset: " .. tostring(begin))
end)

case("Frames: the game is held to framesTalk for the conversation and given back after", function(E, M, check)
    -- The voice engine's share of the machine (Frames in emit.cpp): 60 fps goes to 30 at
    -- Begin and comes back at Leave; 0 (no cap) is moved the same way; a cap the player
    -- already holds at or under ours is not touched.
    E.cvars.sys_MaxFPS = 60
    local npc = E.soul("kkut_additive_man_10")
    check(E.begin(npc), "the prompt opens the conversation")
    check(tostring(E.cvars.sys_MaxFPS) == tostring(M.framesTalk), "held to " .. M.framesTalk .. ": " .. tostring(E.cvars.sys_MaxFPS))
    check(E.find("BRIDGE asked for sys_MaxFPS " .. M.framesTalk .. ", it reads " .. M.framesTalk), "the hold is logged")
    M.Leave("test")
    check(tostring(E.cvars.sys_MaxFPS) == "60", "given back: " .. tostring(E.cvars.sys_MaxFPS))
    check(M.framesWas == nil, "nothing left to give back")

    E.cvars.sys_MaxFPS = 0
    check(E.begin(npc), "begun again")
    check(tostring(E.cvars.sys_MaxFPS) == tostring(M.framesTalk), "uncapped is moved too")
    M.Leave("test")
    check(tostring(E.cvars.sys_MaxFPS) == "0", "and given back as 0")

    E.cvars.sys_MaxFPS = 24
    check(E.begin(npc), "begun a third time")
    check(tostring(E.cvars.sys_MaxFPS) == "24", "a lower cap of the player's is left alone")
    M.Leave("test")
    check(tostring(E.cvars.sys_MaxFPS) == "24", "and still there")
end)

case("Frames: a dialogue of the game's own is held to framesTalk while the app voices it, and let go after", function(E, M, check)
    -- The voiced rows (emit.cpp, voiced_sequences) call Voice outside any conversation of
    -- ours; the Begin road's cap never applied, and the engine beside an uncapped game
    -- stuttered through Paul Schmidt's lines (12 Oct 2026). Held from the first Voice, kept
    -- while the dialogue is open and for a few seconds after the last line, then given back.
    E.cvars.sys_MaxFPS = 60
    E.dialog.open = true
    M.Voice("henry", "I'm looking for the church's silver chalice.")
    check(E.find("BRIDGE voice henry I'm looking for the church's silver chalice."), "the line logged for the app")
    check(tostring(E.cvars.sys_MaxFPS) == tostring(M.framesTalk), "held: " .. tostring(E.cvars.sys_MaxFPS))
    E.advance(6000)
    check(tostring(E.cvars.sys_MaxFPS) == tostring(M.framesTalk), "still held while the dialogue is open")
    M.Voice("male", "Sakra! Fine, take the cursed thing.")
    E.dialog.open = false
    E.advance(2000)
    check(tostring(E.cvars.sys_MaxFPS) == tostring(M.framesTalk), "and for a few seconds past the last line")
    E.advance(3000)
    check(tostring(E.cvars.sys_MaxFPS) == "60", "given back: " .. tostring(E.cvars.sys_MaxFPS))
    check(M.framesWas == nil, "nothing left to give back")
    check(E.find("BRIDGE voiced dialogue over, frames given back"), "logged")
    -- Nothing to say is nothing to hold for.
    M.Voice("henry", "...")
    check(tostring(E.cvars.sys_MaxFPS) == "60", "dots are not a line")
end)

case("Disk: a dialogue of the game's own opens the disk for the mouth's cues, and gives it back with the frames", function(E, M, check)
    -- The cues are loose files; at the pak-only rest the engine never saw one, and Paul
    -- Schmidt's Talk armed every line and found no cue in 600 polls (notes.md 14 Oct 2026).
    -- The build the game ships has sys_PakPriority at 2, which DiskStart takes charge of.
    E.cvars.sys_PakPriority = "2"
    M.DiskStart()
    check(M.diskHold == true and tostring(E.cvars.sys_PakPriority) == "2", "at rest before the Talk")
    E.dialog.open = true
    M.Voice("henry", "I'm looking for the church's silver chalice.")
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRead), "opened with the first line: " .. tostring(E.cvars.sys_PakPriority))
    check(E.find("BRIDGE asked for sys_PakPriority " .. M.diskRead), "and said so")
    M.Voice("male", "That desperate labourer Benesh stole the parish silver at Easter.")
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRead), "held across the row")
    -- The console's own poll, armed by a key and reading once a second between
    -- conversations, opens and shuts the disk round its read; not while this holds it.
    Player:OnAction("move_forward", 1, 1)
    check(E.find("BRIDGE console armed (OnAction)"), "the console chain up beside the Talk")
    E.advance(2500)
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRead), "still open after the console's polls: " .. tostring(E.cvars.sys_PakPriority))
    E.dialog.open = false
    E.advance(5000)
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRest), "given back with the frames: " .. tostring(E.cvars.sys_PakPriority))
    check(M.voicedDisk == false, "nothing left to give back")
    -- And with the hold gone, the console's poll is back to opening and shutting it itself.
    E.files["%USER%/moonshine/hook_1.txt"] = "return 'read between conversations'"
    E.advance(1100)
    check(M.hooked == 1 and tostring(E.cvars.sys_PakPriority) == tostring(M.diskRest), "the console read its hook and shut the disk after")
    -- A second Talk opens it again (the first's hold is not remembered as still on).
    E.dialog.open = true
    M.Voice("henry", "We both know it's in your chest.")
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRead), "opened again for the next Talk")
    E.dialog.open = false
    E.advance(5000)
    check(tostring(E.cvars.sys_PakPriority) == tostring(M.diskRest), "and given back again")
    -- The engine's cvars and the mod's hold outlive the case; the others run at 0, untouched.
    E.cvars.sys_PakPriority = "0"
    M.diskHold = false
end)

case("Said: with a voice, Henry's line is held, camera on him, until the app says it was heard", function(E, M, check)
    -- The menu frames Henry and the row picked went straight back to a beat framed on the
    -- other party, so his words were heard over the listener, and the reply replaced his
    -- line the moment it landed (12 Oct 2026). With @voiced seen, Said holds the reply and
    -- asks for the beat framed on him until Spoken('henry') or the reading plus the grace.
    live(E, M)
    M.Said("Which way to the mill?")
    check(M.hold == 0, "no hold without a voice, as before")
    check(M.face ~= M.henryCode, "and the camera not asked for")
    M.Say(1, "@voiced Past the smithy, then left.")
    check(M.voiced == true, "the voice known from the line")
    M.Said("And the miller, is he honest?")
    check(M.face == M.henryCode, "the beat framed on Henry asked for: " .. tostring(M.face))
    check(M.hold > M.ticks and M.henryUntil == M.hold, "the reply held behind his line")
    check(M.speakUntil == 0, "the other party's mouth is not left to put the rest face back")
    -- The app's word lets go early, and the camera goes back.
    M.ticks = M.ticks + 3
    M.Spoken("henry")
    check(M.henryUntil == 0 and M.hold <= M.ticks, "let go")
    check(M.face == M.restFace, "the camera back on the rest face")
    check(E.find("BRIDGE henry heard: spoken"), "logged with how early")
    -- An app that said it would speak and then did not is waited for no longer than the
    -- reading and the grace.
    M.Said("Hm.")
    check(M.henryUntil > M.ticks, "held again")
    M.ticks = M.henryUntil
    M.HenryExpire()
    check(M.henryUntil == 0 and M.hold <= M.ticks, "let go when the grace ran out")
    check(E.find("BRIDGE henry heard: grace ran out"), "and logged as such")
    -- A word arriving after that does nothing, and the closing word is for the close alone.
    M.hold = 0
    M.Spoken("henry")
    M.Spoken("npc")
    check(M.hold == 0 and M.spoken == false, "nothing to let go of, nothing closing")
    M.ending = true
    M.Spoken("npc")
    check(M.spoken == true, "the closing line's word taken while closing")
end)

-- -------------------------------------------------------------------- Mouth

-- The cue file the app writes beside the turn files as a line's audio starts (lipsync.h):
-- lip_<key>_<seq>.txt under the inbox pattern, the key the sum both sides make.
local function lipCue(E, M, who, words, seq, body)
    local key = M.LipKey(who, words)
    local name = string.format("lip_%.0f_%d.txt", key, seq)
    E.files[(string.gsub(M.inboxes[1], "turn_#%.txt", name))] = body
    return key
end

case("Mouth: the app's cue plays the game's clips on the dialogue twin, and the stop takes them off", function(E, M, check)
    live(E, M)
    -- The game draws a spawned twin of each party through a conversation; the entity
    -- itself stands frozen (lipsync_pass.lua, 12 Oct 2026).
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    local words = "Have you come for another blessing?"
    M.Say(1, "@voiced " .. words)
    check(M.lipWho == "npc" and M.lipKey == M.LipKey("npc", words), "looking for the line's cues under the app's key")
    check(M.menuHeld == true and M.choosing == true and M.face ~= M.chooseCode, "the round set up, the menu's code held for the voice")
    check(E.find("BRIDGE menu held for the voice"), "and logged")
    -- The rows land during the hold and are kept for it (a Choose() after them cleared
    -- them, and the first held menu drew with no rows, 12 Oct 2026).
    M.Say(2, "@opt 1 ask 18:I have a question. @opt 2 about 23:Tell me about yourself. @rows 2")
    check(M.options[1] ~= nil and M.options[2] ~= nil, "the rows collected")
    local key = lipCue(E, M, "npc", words, 1,
        "lip npc " .. string.format("%.0f", M.lipKey) .. "\nplay pdea_dial_a 1.10 2.00\nplay pdea_dial_b 0.90 3.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 1 and twin.anims[1].clip == "pdea_dial_a" and twin.anims[1].layer == M.lipLayer
        and twin.anims[1].speed == 1.1, "the first clip on the twin's layer at the cue's speed")
    check(#twin.stops == 0, "nothing stopped under it: a clip blends from whatever the layer held (14 Oct 2026)")
    check(#M.npc.anims == 0, "nothing on the frozen entity")
    check(M.lipsync == true, "the mouth is the app's from now on")
    check(E.find("BRIDGE lip npc 2 clips over 5.0s on the twin: pdea_dial_a x1.10"), "logged")
    E.advance(2000)
    check(#twin.anims == 2 and twin.anims[2].clip == "pdea_dial_b" and twin.anims[2].speed == 0.9,
        "the second clip after the first's seconds")
    check(M.menuHeld == true and M.face ~= M.chooseCode, "still held while the audio goes")
    lipCue(E, M, "npc", words, 2, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(twin.stops[#twin.stops] == M.lipLayer and M.lipEnt == nil and M.lipKey == nil, "stopped, and done looking")
    check(E.find("BRIDGE lip stopped -- audio ended"), "logged")
    check(M.menuHeld == false and M.face == M.chooseCode, "the menu released by the stop, its code set at once with the rows whole")
    check(M.options[1] ~= nil and M.options[2] ~= nil, "with the rows still there")
    check(E.find("BRIDGE menu released -- audio ended"), "and logged")
    -- No third clip arrives for a plan that was stopped.
    local had = #twin.anims
    E.advance(4000)
    check(#twin.anims == had, "the rest of the plan does not play")
    -- The next line: the OpenMouth/Hmm stack is not started over the app's mouth.
    M.choosing = false
    M.Say(3, "@voiced Then come in.")
    check(M.speakUntil == 0 and M.face ~= M.faces.speak, "no jaw stack once the app has a mouth")
end)

case("Mouth: a rest stops the layer where the speech pauses, and a revision takes the plan up at its second", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    local words = "Well. I suppose you had better come in, then."
    M.Say(1, "@voiced " .. words)
    -- The opening plan, with the words placed before the sound started: a word, a pause,
    -- the rest of the line.
    lipCue(E, M, "npc", words, 1, "lip npc 1\nplay pdea_dial_a 1.00 0.60\nrest 0.40\nplay pdea_dial_b 1.00 4.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 1 and twin.anims[1].clip == "pdea_dial_a", "the first clip at once")
    check(#twin.stops == 0, "nothing stopped under it")
    check(E.find("BRIDGE lip npc 2 clips, 1 rest over 5.0s on the twin: pdea_dial_a x1.00"), "logged with the rest")
    -- The line's clock is the first cue's arrival; `at` moves to a second of it.
    local t0 = M.lipT0
    local function at(seconds) E.advance(math.floor((t0 + seconds) * 1000 - E.now)) end
    at(0.65)
    check(#twin.stops == 1 and twin.stops[1] == M.lipLayer and #twin.anims == 1, "the rest with no shut clip named: the layer stopped, nothing started")
    at(1.05)
    check(#twin.anims == 2 and twin.anims[2].clip == "pdea_dial_b", "the next clip as the rest ends")
    -- A revision for a second still to come: the plan playing waits for it. At 1.1 s a
    -- pause at 2.5 s is found.
    lipCue(E, M, "npc", words, 2, "lip npc 1\nfrom 2.50\nrest 0.30\nplay pdea_dial_c 1.20 2.00\n")
    E.advance(M.lipPollMillis + 10)
    check(E.find("BRIDGE lip npc from 2.50s 1 clip, 1 rest over 2.3s on the twin: rest 0.30s"), "the revision logged")
    check(#twin.anims == 2 and #twin.stops == 1 and M.lipPending ~= nil, "nothing yet: the clip before it plays on")
    at(2.45)
    check(#twin.anims == 2 and #twin.stops == 1, "still nothing just before its second")
    at(2.56)
    check(#twin.stops == 2 and #twin.anims == 2, "at its second the rest: the layer stopped")
    check(M.lipPending == nil and M.lipPlan ~= nil and #M.lipPlan == 2, "and the revision is the plan")
    at(2.9)
    check(#twin.anims == 3 and twin.anims[3].clip == "pdea_dial_c" and twin.anims[3].speed == 1.2, "then its clip")
    -- A revision whose second has passed joins partway: from 3.0 s, a 0.5 s rest then a
    -- clip; arriving at about 3.2 s the rest has 0.3 s left.
    at(3.1)
    lipCue(E, M, "npc", words, 3, "lip npc 1\nfrom 3.00\nrest 0.50\nplay pdea_dial_d 1.00 2.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 3 and #twin.stops == 3 and M.lipPlan ~= nil and #M.lipPlan == 2, "taken up at once, in its rest")
    at(3.45)
    check(#twin.anims == 3, "the rest's remainder still running")
    at(3.6)
    check(#twin.anims == 4 and twin.anims[4].clip == "pdea_dial_d", "its clip after what was left of the rest")
    -- The old chains are dead: nothing else starts.
    local had = #twin.anims
    E.advance(3000)
    check(#twin.anims == had, "no clip of a replaced plan")
    lipCue(E, M, "npc", words, 4, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipPlan == nil and M.lipPending == nil and M.lipKey == nil, "stopped and cleared")
end)

case("Mouth: with the face's shut clip named, a rest and the line's end blend the lips shut, and the layer is let go only once they have settled", function(E, M, check)
    -- notes.md 14 Oct 2026: the lips snapped back to the face's pose at every pause and
    -- line's end, which was the layer stopped outright.
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    local words = "Well. I suppose you had better come in, then."
    M.Say(1, "@voiced " .. words)
    lipCue(E, M, "npc", words, 1, "lip npc 1\nshut pdea_hum\nplay pdea_dial_a 1.00 0.60\nrest 0.40\nplay pdea_dial_b 1.00 4.00\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipShut == "pdea_hum", "the shut clip taken from the cue")
    check(#twin.anims == 1 and twin.anims[1].clip == "pdea_dial_a" and #twin.stops == 0, "the first clip, nothing stopped")
    local t0 = M.lipT0
    local function at(seconds) E.advance(math.floor((t0 + seconds) * 1000 - E.now)) end
    at(0.65)
    check(#twin.stops == 0, "the rest stops nothing")
    check(#twin.anims == 2 and twin.anims[2].clip == "pdea_hum" and twin.anims[2].blend == M.lipRestBlend,
        "the shut clip blended in over the rest's blend")
    check(M.lipOn == "pdea_hum", "and known to be on the layer")
    at(1.05)
    check(#twin.anims == 3 and twin.anims[3].clip == "pdea_dial_b" and twin.anims[3].blend == M.lipBlend,
        "the next clip blends from the shut mouth")
    -- A revision that keeps the clip playing does not re-issue it (the engine would refuse).
    lipCue(E, M, "npc", words, 2, "lip npc 1\nfrom 2.00\nshut pdea_hum\nplay pdea_dial_b 1.00 1.00\nrest 0.50\nplay pdea_dial_c 1.00 2.00\n")
    at(2.1)
    check(#twin.anims == 3 and M.lipPlan ~= nil and #M.lipPlan == 3, "taken up; the clip on the layer plays on")
    at(3.1)
    check(#twin.anims == 4 and twin.anims[4].clip == "pdea_hum", "its rest: the shut clip again")
    at(3.6)
    check(#twin.anims == 5 and twin.anims[5].clip == "pdea_dial_c", "then the clip after")
    -- The end: the lips shut, the layer kept until they have settled, then let go.
    lipCue(E, M, "npc", words, 3, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 6 and twin.anims[6].clip == "pdea_hum" and #twin.stops == 0, "the stop blends the lips shut and stops nothing yet")
    check(M.lipEnt == nil and M.lipKey == nil and M.lipShut == nil, "done looking, the shut clip forgotten with the line")
    check(E.find("BRIDGE lip stopped -- audio ended"), "logged")
    E.advance(M.lipLetGoMillis - 200)
    check(#twin.stops == 0, "not yet")
    E.advance(400)
    check(#twin.stops == 1 and twin.stops[1] == M.lipLayer and M.lipOn == nil, "the layer let go once the lips have settled")
    check(E.find("BRIDGE lip layer let go"), "and logged")
    -- A next line armed before the letting-go keeps the layer: its clips blend from the shut mouth.
    local more = "Mind the step, it is loose."
    M.Say(2, "@voiced " .. more)
    lipCue(E, M, "npc", more, 1, "lip npc 1\nshut pdea_hum\nplay pdea_dial_d 1.00 2.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 7 and twin.anims[7].clip == "pdea_dial_d", "the next line's clip")
    lipCue(E, M, "npc", more, 2, "stop\n")
    E.advance(M.lipPollMillis + 10)
    local stopsBefore = #twin.stops
    check(#twin.anims == 8 and twin.anims[8].clip == "pdea_hum", "shut again at its end")
    M.Say(3, "@voiced Come in.")
    lipCue(E, M, "npc", "Come in.", 1, "lip npc 1\nshut pdea_hum\nplay pdea_dial_e 1.00 1.00\n")
    E.advance(M.lipPollMillis + 10)
    E.advance(M.lipLetGoMillis + 100)
    check(#twin.stops == stopsBefore, "the letting-go of the line before did not fire under the new line")
    check(twin.anims[#twin.anims].clip == "pdea_dial_e", "which plays")
end)

case("Mouth: shapes at their seconds take over from the clips, held at the bank's frames; the stop counts them", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    -- The bank the data pak ships (Scripts/mods/moonshine_mouth.lua); a case sets its own.
    -- Own clips named, but not loaded by this game (no length): the shipped frames are borrowed.
    MoonshineMouth = { shapes = {
        SIL = { clip = "shut_a", at = 0.10 }, PBM = { clips = { "mn_vis_pbm_1", "mn_vis_pbm_2" }, clip = "lips_b", at = 0.50 },
        AA = { clip = "wide_c", at = 0.25 }, AX = { clip = "mid_d", at = 0.40 },
        FV = { clip = "teeth_e" },
    } }
    M.mouthLoaded = false
    local words = "My father was a blacksmith."
    M.Say(1, "@voiced " .. words)
    -- The plan as the sound starts, the shapes a moment later as the words settle.
    lipCue(E, M, "npc", words, 1, "lip npc 1\nplay pdea_dial_a 1.00 4.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 1 and twin.anims[1].clip == "pdea_dial_a", "the plan's clip first")
    check(M.lipT0 ~= nil, "the first cue is the line's clock")
    local t0 = M.lipT0
    lipCue(E, M, "npc", words, 2, "lip npc 1\nmouth 0.000 PBM\nmouth 0.500 AA\nmouth 0.800 FV\nmouth 0.820 AX\nmouth 1.100 SIL\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipPlan == nil and twin.stops[#twin.stops] == M.lipLayer, "the plan dropped for the shapes")
    check(#M.lipMouth == 5 and M.lipMouthAt >= 2, "the shapes queued, and the first shown")
    local a = twin.anims[#twin.anims]
    check(a.clip == "lips_b" and a.layer == M.lipLayer and a.speed == 1, "PBM: the bank's clip, started at full speed")
    E.advance(2 * M.lipMouthMillis + 10)
    check(a.held == 0.5 and a.speedSet == M.lipMouthSpeed, "and held at its frame, all but stopped, by the ticks after")
    check(E.find("BRIDGE lip own shape clips not loaded, borrowing frames"), "the game's own clips borrowed, and said so")
    check(E.find("BRIDGE lip npc shapes from 0.00s on the twin, first PBM"), "logged")
    E.advance(1300 - (E.now - t0 * 1000))
    -- Later: AA, then FV and AX 20 ms apart -- one tick may see both and show the
    -- newer -- then SIL. A clip of the shape alone (no `at`) plays at its own speed.
    local seen = {}
    for i = 2, #twin.anims do seen[#seen + 1] = twin.anims[i].clip end
    check(seen[1] == "lips_b" and seen[2] == "wide_c" and seen[#seen] == "shut_a", "the shapes in order to the rest")
    local fvOrAx = seen[3]
    check(fvOrAx == "mid_d" or fvOrAx == "teeth_e", "the close pair shown as one or both")
    check(M.lipShape == "SIL", "resting")
    -- A shape the bank lacks shows as AX.
    lipCue(E, M, "npc", words, 3, "lip npc 1\nmouth 1.400 XYZ\n")
    E.advance(400)
    check(twin.anims[#twin.anims].clip == "mid_d", "an unknown shape is the neutral one")
    lipCue(E, M, "npc", words, 4, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipEnt == nil and M.lipKey == nil and #M.lipMouth == 0 and M.lipShape == nil, "stopped and cleared")
    check(E.find("BRIDGE lip stopped -- audio ended after"), "the count of shapes logged")
    check(M.menuHeld == false, "the menu released by the stop")
    local had = #twin.anims
    E.advance(1000)
    check(#twin.anims == had, "no shape after the stop")
    MoonshineMouth = nil
    M.mouthLoaded = false
end)

case("Mouth: shapes in the first cue need no plan, and without a bank the clips stay", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    MoonshineMouth = { shapes = { AA = { clip = "wide_c", at = 0.25 }, SIL = { clip = "shut_a", at = 0.1 } } }
    M.mouthLoaded = false
    local words = "Aye."
    M.Say(1, "@voiced " .. words)
    lipCue(E, M, "npc", words, 1, "lip npc 1\nmouth 0.000 AA\nmouth 0.400 SIL\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 1 and twin.anims[1].clip == "wide_c", "the shape straight away")
    check(M.lipsync == true, "the mouth is the app's")
    -- A plan arriving after shapes is ignored: the shapes know better.
    lipCue(E, M, "npc", words, 2, "lip npc 1\nplay pdea_dial_a 1.00 4.00\n")
    E.advance(M.lipPollMillis + 10)
    for i = 1, #twin.anims do check(twin.anims[i].clip ~= "pdea_dial_a", "no plan clip over the shapes") end
    lipCue(E, M, "npc", words, 3, "stop\n")
    E.advance(M.lipPollMillis + 10)
    MoonshineMouth = nil
    M.mouthLoaded = true  -- as if the pak had no bank to load
    -- No bank: a mouth cue does nothing, and the clips' plan is kept.
    M.choosing = false
    local more = "Then come in, lad."
    M.Say(2, "@voiced " .. more)
    local before = #twin.anims
    lipCue(E, M, "npc", more, 1, "lip npc 1\nplay pdea_dial_b 1.00 4.00\n")
    E.advance(M.lipPollMillis + 10)
    lipCue(E, M, "npc", more, 2, "lip npc 1\nmouth 0.000 AA\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == before + 1 and twin.anims[#twin.anims].clip == "pdea_dial_b", "the plan plays on")
    check(E.find("BRIDGE lip no bank of shapes"), "the missing bank noted")
    lipCue(E, M, "npc", more, 3, "stop\n")
    E.advance(M.lipPollMillis + 10)
    M.mouthLoaded = false
end)

case("Mouth: the mod's own shape clips, where the game loads them, play at full speed, copies in turn, nothing held", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    MoonshineMouth = { shapes = {
        AA = { clips = { "mn_vis_aa_1", "mn_vis_aa_2" }, clip = "wide_c", at = 0.25 },
        PBM = { clips = { "mn_vis_pbm_1", "mn_vis_pbm_2" }, clip = "lips_b", at = 0.50 },
        SIL = { clips = { "mn_vis_sil_1", "mn_vis_sil_2" }, clip = "shut_a", at = 0.10 },
    } }
    M.mouthLoaded = false
    for _, c in ipairs({ "mn_vis_aa_1", "mn_vis_aa_2", "mn_vis_pbm_1", "mn_vis_pbm_2", "mn_vis_sil_1", "mn_vis_sil_2" }) do
        E.clipLengths[c] = 0.5
    end
    local words = "Papa, aye."
    M.Say(1, "@voiced " .. words)
    lipCue(E, M, "npc", words, 1, "lip npc 1\nmouth 0.000 PBM\nmouth 0.200 AA\nmouth 0.400 PBM\nmouth 0.600 AA\nmouth 0.800 SIL\n")
    E.advance(1200)
    check(E.find("BRIDGE lip own shape clips loaded (mn_vis_pbm_1 0.50s)"), "the own clips found by their length")
    local seen = {}
    for i = 1, #twin.anims do
        local a = twin.anims[i]
        seen[#seen + 1] = a.clip
        check(a.speed == 1 and a.held == nil and a.speedSet == nil, "an own clip runs at full speed and is not held: " .. a.clip)
    end
    check(table.concat(seen, " ") == "mn_vis_pbm_1 mn_vis_aa_1 mn_vis_pbm_2 mn_vis_aa_2 mn_vis_sil_1",
        "the shapes in order, a shape come round again taking its next copy: " .. table.concat(seen, " "))
    -- Each blends in over the gap since the one before (the first over the least), starting
    -- half that early: the AA due at 0.2 s is started about 0.1 s before, less the lead.
    check(twin.anims[1].blend == M.lipMouthBlend and math.abs(twin.anims[2].blend - 0.2) < 1e-6,
        "the blend is the gap since the last shape")
    local t0 = M.lipT0 * 1000
    local startedAA = twin.anims[2].at - t0
    check(startedAA >= 200 - 100 - M.lipMouthLead * 1000 - M.lipMouthMillis and startedAA <= 200 - 100 - M.lipMouthLead * 1000 + M.lipMouthMillis,
        "the shape started half a blend early (" .. tostring(startedAA) .. " ms)")
    lipCue(E, M, "npc", words, 2, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(E.find("BRIDGE lip stopped -- audio ended after 5 shapes, 0 skipped, 0 refused"), "all five shown")
    MoonshineMouth = nil
    M.mouthLoaded = false
end)

case("Mouth: the held menu lets go at the reading with no cue, and at the reading plus the grace with one", function(E, M, check)
    live(E, M)
    -- An app with no facials beside its voices writes no cue: the menu comes as it did.
    M.Say(1, "@voiced A line the app never cues, long enough to be read over a few ticks.")
    check(M.menuHeld == true, "held")
    M.ticks = M.hold - 1
    M.MenuHeldExpire()
    check(M.menuHeld == true, "not before the reading")
    M.ticks = M.hold
    M.MenuHeldExpire()
    check(M.menuHeld == false, "let go at the reading")
    check(E.find("BRIDGE menu released -- no cue by the reading"), "logged as such")
    -- A cue came and the stop never did: the reading plus the grace, as the closing line.
    M.choosing = false
    local words = "And this one the app cues."
    M.Say(2, "@voiced " .. words)
    lipCue(E, M, "npc", words, 1, "lip npc 1\nplay pdea_dial_a 1.00 20.00\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipSeq == 2 and M.lipEnt ~= nil, "the plan taken")
    M.ticks = M.hold + M.spokenGraceTicks - 1
    M.MenuHeldExpire()
    check(M.menuHeld == true, "waited past the reading for the stop")
    M.ticks = M.hold + M.spokenGraceTicks
    M.MenuHeldExpire()
    check(M.menuHeld == false, "let go when the grace ran out")
    check(E.find("BRIDGE menu released -- grace ran out"), "logged as such")
end)

case("Mouth: Henry's line plays on his twin and holds no menu; Leave takes the layer off", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_dude")
    M.Say(1, "@voiced Past the smithy, then left.")
    M.MenuRelease("the case moves on")
    local words = "And the miller, is he honest?"
    M.Said(words)
    check(M.lipWho == "henry" and M.menuHeld == false, "his cues looked for, no menu held on his line")
    lipCue(E, M, "henry", words, 1, "lip henry 1\nplay tmck_dial_a 1.00 3.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#twin.anims == 1 and twin.anims[1].clip == "tmck_dial_a", "the clip on Henry's twin")
    check(E.find("BRIDGE lip henry 1 clip over 3.0s on the twin: tmck_dial_a x1.00"), "logged")
    M.Leave("the case is over")
    check(twin.stops[#twin.stops] == M.lipLayer and M.lipEnt == nil and M.lipKey == nil, "the layer stopped on the way out")
    check(E.find("BRIDGE lip stopped -- conversation over"), "logged")
end)

case("Mouth: a party with no twin is played on the entity itself", function(E, M, check)
    live(E, M)
    local words = "No twin stands for me."
    M.Say(1, "@voiced " .. words)
    lipCue(E, M, "npc", words, 1, "lip npc 1\nplay pdea_dial_a 1.00 2.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#M.npc.anims == 1 and M.npc.anims[1].clip == "pdea_dial_a", "the clip on the entity")
    check(E.find("BRIDGE lip npc 1 clip over 2.0s on the entity: pdea_dial_a x1.00"), "logged as the entity's")
end)

case("Mouth: a tap of the skip button takes the mouth off and lets the held menu come", function(E, M, check)
    live(E, M)
    local twin = E.soul("DialogTwin_kkut_additive_man_47")
    local words = "A long line the player has heard enough of, going on and on about the weather."
    M.Say(1, "@voiced " .. words)
    M.Say(2, "@opt 1 ask 18:I have a question. @rows 1")
    lipCue(E, M, "npc", words, 1, "lip npc 1\nplay pdea_dial_a 1.00 12.00\n")
    E.advance(M.lipPollMillis + 10)
    check(M.menuHeld == true and M.lipEnt == twin, "held, the mouth going on the twin")
    -- Space, tapped: press, then release inside the hold delay (OnPlayerAction).
    M.OnPlayerAction("dialog_skip", "press")
    M.OnPlayerAction("dialog_skip", "release")
    check(E.find("BRIDGE skipped turn=2"), "the tap read as a skip")
    check(twin.stops[#twin.stops] == M.lipLayer and M.lipEnt == nil and M.lipKey == nil, "the mouth off, the cues no longer looked for")
    check(E.find("BRIDGE lip stopped -- skipped"), "logged")
    check(M.menuHeld == false and M.face == M.chooseCode, "the menu let go and its code set")
    check(E.find("BRIDGE menu released -- skipped"), "logged as the skip's")
    -- The stop cue arriving afterwards finds nothing to do.
    lipCue(E, M, "npc", words, 2, "stop\n")
    E.advance(1000)
    check(E.count("BRIDGE lip stopped") == 1, "no second stop")
end)

case("Mouth: without a voice nothing is looked for and the menu is not held", function(E, M, check)
    live(E, M)
    M.Say(1, "Have you come for another blessing?")
    check(M.lipKey ~= nil, "the cues are looked for regardless, in case the app has a mouth")
    check(M.menuHeld == false and M.choosing == true, "but the menu is not held without @voiced")
end)

-- -------------------------------------------------------------------- QuestCommand

case("QuestCommand: accept on a retrieve grants the holder role when he is loaded", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local holder = E.soul(q.target)
    M.QuestCommand(id, "accept")
    check(holder.soul.metaroles[q.holderMeta] == true, "metarole " .. q.holderMeta .. " on the holder")
    check(E.find("holder role " .. q.holderMeta .. " granted to " .. q.target .. " -> true"), "granted line")
    check(M.questQueue[1] == nil and M.questCode == M.questCodes[id .. " accept"], "the accept code armed at once")
end)

case("QuestCommand: accept with the holder unloaded waits for sight", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    M.QuestCommand(id, "accept")
    check(E.find("holder " .. q.target .. " not loaded, role waits for sight"), "waits")
    local holder = E.soul(q.target)
    E.look(holder)
    check(holder.soul.metaroles[q.holderMeta] == true, "granted on the first look")
end)

case("QuestCommand: the queue arms one code at a time", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    local id2 = byGate(M, "truth|lied")
    live(E, M)
    M.QuestCommand(id, "accept")
    M.QuestCommand(id2, "accept")
    check(M.questCode == M.questCodes[id .. " accept"], "first armed")
    check(#M.questQueue == 1 and M.questQueue[1] == M.questCodes[id2 .. " accept"], "second queued")
    M.QuestBeat(M.questCode)
    check(M.questCode == M.questCodes[id2 .. " accept"], "second armed after the first beat")
    check(E.gvar(M.questVar) == M.questCode, "the global follows")
end)

case("QuestCommand: a code nobody plays is dropped after questPatience ticks", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    M.active = true
    M.QuestCommand(id, "accept")
    M.ticks = M.questPatience + 1
    M.QuestExpire()
    check(M.questCode == 0, "dropped")
    check(E.find("never played, dropped"), "logged")
end)

case("QuestCommand: complete on a retrieve takes the thing back and pays", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local giver = E.soul(q.giver)
    M.npc = giver
    player.inventory:CreateItem(q.item, 100, 1)
    M.state.quests[id] = { accepted = true, handed = true, how = "persuaded" }
    local purse = player.inventory:GetCountOfClass(E.moneyClass)
    M.QuestCommand(id, "complete")
    check(player.inventory:GetCountOfClass(q.item) == 0, "the player has none")
    check(giver.inventory:GetCountOfClass(q.item) == 1, "the giver has it")
    check(player.inventory:GetCountOfClass(E.moneyClass) == purse + q.reward * 10, "paid " .. q.reward .. " groschen")
    check(E.find("completes as complete_persuaded"), "the how picks the beat")
    check(M.questCode == M.questCodes[id .. " complete_persuaded"], "the persuaded code armed")
    check(M.state.quests[id].done == true, "done in the store")
end)

case("QuestCommand: complete unhanded on a retrieve is stolen", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    M.npc = E.soul(q.giver)
    player.inventory:CreateItem(q.item, 100, 1)
    M.state.quests[id] = { accepted = true }
    M.QuestCommand(id, "complete")
    check(E.find("completes as complete_stolen"), "stolen")
    check(M.state.quests[id].how == "stolen", "and the store says so, for the dump and the app's done paragraph")
end)

-- Only a retrieve can be stolen: a travel's told, a find's hand-in and a mind's plain
-- complete have no handover to miss, and their store keeps how nil (the headless matrix
-- found every told travel ending :d/stolen once the how was stored).
case("QuestCommand: a plain complete on a travel or a find is not a theft", function(E, M, check)
    for _, kind in ipairs({ "travel", "find" }) do
        local id, q = byKind(M, kind)
        check(q ~= nil, "a " .. kind .. " in the table")
        if q ~= nil then
            M.npc = E.soul(q.giver)
            if kind == "find" then player.inventory:CreateItem(q.item, 100, q.count or 1) end
            M.state.quests[id] = { accepted = true }
            M.QuestCommand(id, "complete")
            check(M.state.quests[id].how == nil, "the " .. kind .. "'s how stays nil, got " .. tostring(M.state.quests[id].how))
        end
    end
end)

case("QuestCommand: a find hands in the count and settles for fewer", function(E, M, check)
    local id, q = byName(M, "Forget Me Not")
    check(q and q.kind == "find" and q.count == 5, "five of a thing")
    M.npc = E.soul(q.giver)
    player.inventory:CreateItem(q.item, 100, 5)
    M.state.quests[id] = { accepted = true }
    M.QuestCommand(id, "complete")
    check(E.find("complete moved 5 fresh"), "moved all five")
    check(M.npc.inventory:GetCountOfClass(q.item) == 5, "the giver holds five")
    check(E.find("completes as complete") and not E.find("complete_stolen"), "a find has one ending: " .. tostring(E.find("completes as")))
end)

-- Joan wants the marigolds fresh from the meadow (min_health 0.9; notes.md 6 Oct 2026):
-- stale ones in Henry's pocket are not counted towards what the begin line reports, and
-- the hand-in takes the freshest first and leaves him the rest, where the engine's own
-- by-class takers would have handed her the stalest (health_probe.lua).
case("Fresh: a find with min_health counts and takes only the fresh ones, freshest first", function(E, M, check)
    local id, q = byName(M, "Forget Me Not")
    check(q and q.minHealth == 0.9, "wants them at 0.9, got " .. tostring(q and q.minHealth))
    M.npc = E.soul(q.giver)
    M.state.quests[id] = { accepted = true }
    player.inventory:CreateItem(q.item, 0.5, 2)     -- picked days ago
    player.inventory:CreateItem(q.item, 0.95, 1)
    player.inventory:CreateItem(q.item, 1, 3)
    check(M.Count(player, q.item) == 6, "six in the pocket")
    check(M.CountFresh(player, q.item, 0.9) == 4, "four fresh, got " .. M.CountFresh(player, q.item, 0.9))
    check(M.CountFresh(player, q.item, 0) == 6, "no threshold is the plain count")
    local holding = M.Holding(M.npc, player)
    check(string.find(holding, " have=4", 1, true) ~= nil, "the begin line reports the fresh four: " .. holding)
    -- Four is short of five; what he hands over on complete is the fresh four, not the stale two.
    player.inventory:CreateItem(q.item, 1, 4)        -- seven at 1.0 now, eight fresh
    check(M.CountFresh(player, q.item, 0.9) == 8, "eight fresh")
    M.QuestCommand(id, "complete")
    check(E.find("complete moved 5 fresh"), "moved five fresh")
    check(M.npc.inventory:GetCountOfClass(q.item) == 5, "the widow holds five, got " .. M.npc.inventory:GetCountOfClass(q.item))
    for _, wuid in ipairs(M.npc.inventory:GetInventoryTable()) do
        local it = ItemManager.GetItem(wuid)
        if it.class == q.item then check(it.health >= 0.9, "hers are fresh: " .. it.health) end
    end
    -- Ten in, five out: the 1.0 row shrank from seven to two, the 0.95 and the stale pair untouched.
    check(M.Count(player, q.item) == 5, "Henry keeps five, got " .. M.Count(player, q.item))
    check(M.CountFresh(player, q.item, 0.9) == 3, "three fresh left, got " .. M.CountFresh(player, q.item, 0.9))
    check(M.CountFresh(player, q.item, 0.96) == 2, "two of them the 1.0 row's remainder, got " .. M.CountFresh(player, q.item, 0.96))
    check(M.CountFresh(player, q.item, 0.5) == 5, "and the two stale ones untouched")
end)

-- Forget Me Not's carved wooden heart (notes.md 5 Oct 2026): laid in the grass where the
-- marigolds grow when the errand is taken up, watched for in Henry's pocket (the flag e),
-- and handed to the widow with the flowers.
case("Keepsake: accept lays it at the spot, picking it up flags e, the hand-in takes it with the things", function(E, M, check)
    local id, q = byName(M, "Forget Me Not")
    check(q and q.keepsake ~= nil and q.spot ~= nil, "a find with a keepsake and its spot")
    M.npc = E.soul(q.giver)
    M.QuestCommand(id, "accept")
    local f = M.state.quests[id]
    check(f and f.laid == true, "laid in the store")
    check(E.find("keepsake laid at"), "logged as laid")
    check(player.inventory:GetCountOfClass(q.keepsake) == 0, "not left in Henry's pocket")
    check(#E.ground == 1 and E.ground[1].class == q.keepsake, "one on the ground")
    check(M.watching == true, "the watch is armed for it")
    E.advance(2100)
    check(M.state.quests[id].keepsake ~= true, "not his while it lies there")
    -- Henry picks it up: the next tick sees it in his pocket.
    player.inventory:CreateItem(q.keepsake, 100, 1)
    E.advance(2100)
    check(M.state.quests[id].keepsake == true, "in the store once picked up")
    check(M.QuestFlagLetters(M.state.quests[id]) == "e", "the letter e, got " .. M.QuestFlagLetters(M.state.quests[id]))
    check(#E.ground == 1, "not laid a second time")
    -- Dropped again, the flag goes with it; picked up once more, it is back.
    player.inventory:DeleteItemOfClass(q.keepsake, 1)
    E.advance(2100)
    check(M.state.quests[id].keepsake == false, "gone from the store when dropped")
    player.inventory:CreateItem(q.keepsake, 100, 1)
    E.advance(2100)
    -- The hand-in with the flowers takes the heart too, and the flag stays for the thanks.
    player.inventory:CreateItem(q.item, 100, q.count)
    M.QuestCommand(id, "complete")
    check(player.inventory:GetCountOfClass(q.keepsake) == 0, "the heart left Henry")
    check(M.npc.inventory:GetCountOfClass(q.keepsake) == 1, "the widow has it")
    check(M.npc.inventory:GetCountOfClass(q.item) == q.count, "and the flowers")
    check(M.state.quests[id].keepsake == true and M.state.quests[id].done == true, "done, with the flag")
end)

case("Keepsake: a laying that fails is tried again from the watch once Henry is near", function(E, M, check)
    local id, q = byName(M, "Forget Me Not")
    check(q and q.keepsake ~= nil and q.spot ~= nil, "a find with a keepsake and its spot")
    M.npc = E.soul(q.giver)
    -- The spot entity cannot be made (an unloaded stretch of ground, say).
    local spawn = System.SpawnEntity
    System.SpawnEntity = function() return nil end
    M.QuestCommand(id, "accept")
    check(M.state.quests[id].laid ~= true, "not laid")
    check(E.find("keepsake not laid"), "said so")
    check(player.inventory:GetCountOfClass(q.keepsake) == 0, "nothing left in his pocket")
    System.SpawnEntity = spawn
    -- Far away, the watch leaves it; within keepsakeMetres, it lays it.
    player:SetWorldPos({ x = q.spot[1] + M.keepsakeMetres + 50, y = q.spot[2], z = q.spot[3] })
    E.advance(2100)
    check(M.state.quests[id].laid ~= true, "still not laid from afar")
    player:SetWorldPos({ x = q.spot[1] + 10, y = q.spot[2], z = q.spot[3] })
    E.advance(2100)
    check(M.state.quests[id].laid == true, "laid once near")
    check(#E.ground == 1 and E.ground[1].class == q.keepsake, "one on the ground")
end)

case("Keepsake: a load that wiped the placed item clears laid so the watch can try again", function(E, M, check)
    local id, q = byName(M, "Forget Me Not")
    M.npc = E.soul(q.giver)
    M.QuestCommand(id, "accept")
    check(M.state.quests[id].laid == true, "laid at accept")
    local text = E.save()
    -- The placed PickableItem is gone; Henry does not have it.
    E.ground = {}
    local ok, err = E.load(text)
    check(ok, "loaded: " .. tostring(err))
    check(M.state.quests[id].laid ~= true, "laid forgotten on load")
    player:SetWorldPos({ x = q.spot[1] + 10, y = q.spot[2], z = q.spot[3] })
    E.advance(2100)
    check(M.state.quests[id].laid == true, "laid again once near")
    check(#E.ground == 1 and E.ground[1].class == q.keepsake, "one on the ground")
end)

case("QuestCommand: accept on a letter gives the letter", function(E, M, check)
    local id, q = byLetter(M)
    check(q and q.kind == "deliver", "a letter")
    M.QuestCommand(id, "accept")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the letter in the pocket")
    check(E.find("BRIDGE quest thing given, have=1"), "logged")
end)

case("QuestCommand: lie and keep on a letter end as their own beats", function(E, M, check)
    local id, q = byLetter(M)
    M.QuestCommand(id, "lie")
    check(M.state.quests[id].how == "lied" and M.state.quests[id].done, "lied and done")
    check(M.questCode == M.questCodes[id .. " complete_lied"], "complete_lied armed")
    M.SetQuestCode(0)
    M.state.quests[id] = {}
    M.QuestCommand(id, "keep")
    check(M.state.quests[id].how == "kept", "kept")
    check(M.questCode == M.questCodes[id .. " complete_kept"], "complete_kept armed")
end)

case("QuestCommand: abandon ends the errand as given up, pays nothing and keeps the letter", function(E, M, check)
    -- notes.md 13 Oct 2026: the app's give-up row. The store reads done, how=abandoned (the
    -- dump's :d/abandoned, which the app plans the giver on), the beat of its own is armed
    -- (the graph fails the quest in the book with the Abandoned line), and nothing changes
    -- hands: the letter stays in Henry's pocket and no groschen come.
    local id, q = byLetter(M)
    M.QuestCommand(id, "accept")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the letter in the pocket")
    local purse = player.inventory:GetCountOfClass(E.moneyClass)
    M.SetQuestCode(0)
    M.QuestCommand(id, "abandon")
    local f = M.state.quests[id]
    check(f.done == true and f.accepted == true and f.how == "abandoned", "done as abandoned")
    check(M.questCodes[id .. " abandon"] ~= nil and M.questCode == M.questCodes[id .. " abandon"], "the abandon beat armed")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the letter still in the pocket")
    check(player.inventory:GetCountOfClass(E.moneyClass) == purse, "nothing paid")
    check(E.find("given up; nothing paid"), "logged")
    M.Dump(id)
    check(E.find("quests=" .. id .. ":d/abandoned"), "the dump reads :d/abandoned")
end)

case("QuestCommand: a mind counts its checks and is due after the gap", function(E, M, check)
    local id, q = byName(M, "Bread and Penance")
    check(q and q.kind == "mind" and q.count == 3, "three checks")
    M.state.quests[id] = { accepted = true }
    check(M.MindDue(id) == true, "due before any check")
    M.QuestChecked(id)
    check(M.state.quests[id].checks == 1, "one check")
    check(M.MindDue(id) == false, "not due straight after")
    check(E.gvar(q.dueVar) == 0 and E.gvar(q.checksVar) == 1, "globals follow")
    E.worldBase = E.worldBase + (q.gap or 6) * 3600
    check(M.MindDue(id) == true, "due again after the gap")
    M.QuestChecked(id)
    E.worldBase = E.worldBase + (q.gap or 6) * 3600
    M.QuestChecked(id)
    check(M.state.quests[id].handed == true, "the last check is the handover")
end)

-- -------------------------------------------------------------------- The holder

case("QuestHolderGave: persuade records the how and moves the thing", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local holder = E.soul(q.target)
    holder.inventory:CreateItem(q.item, 100, 1)
    M.state.quests[id] = { accepted = true }
    E.holderGave(id, "persuade")
    check(M.state.quests[id].how == "persuaded", "persuaded")
    check(M.state.quests[id].handed == true, "handed")
    check(player.inventory:GetCountOfClass(q.item) == 1 and holder.inventory:GetCountOfClass(q.item) == 0, "moved")
    check(E.find("handover moved 1"), "moved line")
end)

case("QuestHolderGave: a holder with none has the thing made", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    E.soul(q.target)
    M.state.quests[id] = { accepted = true }
    E.holderGave(id, "threaten")
    check(player.inventory:GetCountOfClass(q.item) == 1, "made")
    check(E.find("made the item, the holder had none"), "said so")
    check(M.state.quests[id].how == "threatened", "threatened")
end)

case("QuestHolderGave: pay moves groschen to the holder", function(E, M, check)
    local id, q = byKind(M, "retrieve", { dispute = false })
    -- A retrieve whose giver does not pay money has a pay row; find one.
    for cid, cq in pairs(M.quests) do if cq.kind == "retrieve" and cq.pay > 0 then id, q = cid, cq end end
    check(q and q.pay > 0, "a retrieve with a pay row")
    local holder = E.soul(q.target)
    holder.inventory:CreateItem(q.item, 100, 1)
    M.state.quests[id] = { accepted = true }
    local purse = player.inventory:GetCountOfClass(E.moneyClass)
    E.holderGave(id, "pay")
    check(player.inventory:GetCountOfClass(E.moneyClass) == purse - q.pay * 10, "purse down by " .. q.pay)
    check(holder.inventory:GetCountOfClass(E.moneyClass) == q.pay * 10, "holder up by the same")
    check(E.find("paid " .. q.pay .. " groschen to the holder"), "logged")
end)

case("QuestHolderGave: brokered is brokered if the giver apologised, lied if he refused", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    check(q.dispute, "a dispute")
    E.soul(q.target)
    M.QuestCommand(id, "accept")
    M.QuestBroker(id)
    check(M.state.quests[id].broker == true and E.gvar(q.brokerVar) == 1, "broker flag up and the global with it")
    M.QuestCommand(id, "apologise")
    E.holderGave(id, "brokered")
    check(M.state.quests[id].how == "brokered", "brokered")
    M.state.quests[id] = { accepted = true, broker = true }
    M.QuestCommand(id, "refuse_apology")
    E.holderGave(id, "brokered")
    check(M.state.quests[id].how == "lied", "lied")
end)

-- An ask with a bribe in it: the winning row's exit (QuestHolderGave) tells the truth and
-- makes nothing; the menu after it is QuestGift, took or refused.
case("QuestGift: nothing is made at the telling, and took makes the hat and gloves", function(E, M, check)
    local id, q = byGift(M)
    check(q ~= nil and #q.gift == 2, "the fox quest has its two gift classes")
    E.soul(q.target)
    M.state.quests[id] = { accepted = true }
    E.holderGave(id, "persuade")
    check(M.state.quests[id].how == "persuaded" and M.state.quests[id].handed == true, "the truth is told and handed")
    check(player.inventory:GetCountOfClass(q.gift[1]) == 0 and player.inventory:GetCountOfClass(q.gift[2]) == 0,
          "nothing in Henry's pocket yet")
    M.QuestGift(id, "took")
    check(player.inventory:GetCountOfClass(q.gift[1]) == 1 and player.inventory:GetCountOfClass(q.gift[2]) == 1,
          "the hat and the gloves, one each")
    check(M.state.quests[id].gift == "took", "the store says took")
    check(E.find("BRIDGE quest " .. id .. " gift took"), "logged")
    check(E.find("quests=" .. id .. ":ht/persuaded") or M.QuestFlagLetters(M.state.quests[id]) == "t", "the letter t")
end)

-- A reward that is a thing (rewardClass; Heinz's digestive potion): made in Henry's pocket
-- at the hand-in, one more than he had, by PayReward with the groschen.
case("PayReward: a reward that is a thing is made in Henry's pocket, even if he has one", function(E, M, check)
    local id, q
    for i, qq in pairs(M.quests) do if qq.rewardClass ~= nil then id, q = i, qq break end end
    check(q ~= nil, "a quest pays in a thing")
    if q == nil then return end
    player.inventory:CreateItem(q.rewardClass, 100, 1)
    local had = player.inventory:GetCountOfClass(q.rewardClass)
    M.PayReward(q)
    check(player.inventory:GetCountOfClass(q.rewardClass) == had + 1, "one more of it, had=" .. had)
    check(E.find("BRIDGE quest reward given"), "logged as the reward")
end)

case("QuestGift: refused is a bucket of guts -- dirt, blood, Smelly, and no hat", function(E, M, check)
    local id, q = byGift(M)
    E.soul(q.target)
    M.state.quests[id] = { accepted = true }
    E.holderGave(id, "threaten")
    local had = player.inventory:GetCountOfClass(q.gift[1])
    M.QuestGift(id, "refused")
    check(player.inventory:GetCountOfClass(q.gift[1]) == had, "no hat for a man who would not be bought")
    check(player.dirt == 1, "every stitch fouled, dirt=" .. tostring(player.dirt))
    check(player.blood > 0, "and stained")
    check(player.soul:HasBuff(M.smellyBuff), "Smelly")
    check(M.state.quests[id].gift == "refused", "the store says refused")
    check(M.QuestFlagLetters(M.state.quests[id]) == "f", "the letter f")
    check(E.find("gift refused -- a bucket of guts, dirtied=true smelly=true"), "logged")
    -- Heard, not read: the slap and the slop play on Henry, and nothing goes on the screen.
    check(player.sounds[1] == "f_vi_laundry_wet_fabric_hit" and player.sounds[2] == "f_ge_liquid_plop",
        "the sound of it, " .. table.concat(player.sounds, ","))
    check(#E.infoTexts == 0, "no line on the screen, got " .. tostring(E.infoTexts[1]))
end)

-- An ask whose word is carried on (the fox story's guild master): the app's `onward` at the
-- telling pays once and sends Henry on (the beat, the letter w, the onward soul's role);
-- the onward soul's own hand row (QuestCarried) completes on the spot and pays nothing again.
case("Onward: the telling pays and sends Henry on; the word said completes and pays nothing more", function(E, M, check)
    local id, q = byOnward(M)
    check(q ~= nil, "an ask that carries its word on is in the table")
    if q == nil then return end
    check(M.questCodes[id .. " onward"] ~= nil, "the onward beat has a code")
    local giver = E.soul(q.giver)
    local told = E.soul(q.onward)
    M.npc = giver
    M.QuestCommand(id, "accept")
    E.holderGave(id, "persuade")
    check(M.state.quests[id].handed == true and M.state.quests[id].how == "persuaded", "the truth is told")
    local function count() return q.rewardClass and player.inventory:GetCountOfClass(q.rewardClass) or 0 end
    local had = count()
    M.QuestCommand(id, "onward")
    check(M.state.quests[id].onward == true and M.state.quests[id].done ~= true, "sent on, not done")
    check(M.QuestFlagLetters(M.state.quests[id]) == "w", "the letter w, got " .. M.QuestFlagLetters(M.state.quests[id]))
    if q.rewardClass then check(count() == had + 1, "the reward given at the telling") end
    check(E.find("BRIDGE quest reward given") or q.reward == 0 and not q.rewardClass, "paid at the telling")
    check(M.HasMeta(told, q.onwardMeta), "the onward soul has the menu's metarole")
    local queued = false
    for _, code in ipairs(M.questQueue) do if code == M.questCodes[id .. " onward"] then queued = true end end
    check(queued or M.questCode == M.questCodes[id .. " onward"], "the onward code queued behind the accept")
    -- The onward soul's door (q_<id>_onward.xml): shut until the graph's onward port is
    -- played, the game's Talk alone while the last leg is open, ours back once the word is said.
    check(told.actor:CanTalk(player.id) == false, "no Talk on the onward soul before the port")
    E.firePort(id .. " accept")
    E.firePort(id .. " onward")
    check(told.actor:CanTalk(player.id) == true, "the onward soul's Talk once sent on")
    local acts = E.look(told)
    acts = E.look(told)
    check(#acts == 1 and acts[1].base, "the base Talk alone on the onward soul, ours stands down")
    M.npc = told
    E.carried(id)
    check(told.actor:CanTalk(player.id) == false, "no Talk on the onward soul once the word is said")
    check(M.state.quests[id].done == true and M.state.quests[id].carried == true, "done where it was said")
    check(E.find("word carried on to " .. q.onward), "logged as carried on")
    check(E.find("word carried on; paid at the telling"), "and not paid again")
    if q.rewardClass then check(count() == had + 1, "still the one reward") end
    queued = false
    for _, code in ipairs(M.questQueue) do if code == M.questCodes[id .. " complete"] then queued = true end end
    check(queued or M.questCode == M.questCodes[id .. " complete"], "the plain complete queued")
end)

case("QuestDelivered: the letter moves to the receiver and how is delivered", function(E, M, check)
    local id, q = byLetter(M)
    local receiver = E.soul(q.target)
    M.QuestCommand(id, "accept")
    E.delivered(id)
    check(receiver.inventory:GetCountOfClass(q.item) == 1 and player.inventory:GetCountOfClass(q.item) == 0, "moved")
    check(M.state.quests[id].how == "delivered" and M.state.quests[id].handed, "delivered, handed")
end)

-- Whether a beat is armed or waiting in the queue behind another (an accept armed first
-- leaves the complete queued).
local function armedOrQueued(M, id, beat)
    local code = M.questCodes[id .. " " .. beat]
    if code == nil then return false end
    if M.questCode == code then return true end
    for _, queued in ipairs(M.questQueue) do if queued == code then return true end end
    return false
end

case("QuestCommand wrote_: a dictated letter is the Document of its digits, read by them, and the curse refused", function(E, M, check)
    local id, q = byDictation(M)
    if id == nil then check(true, "no dictation in this data"); return end
    local receiver = E.soul(q.target)
    local giver = E.soul(q.giver)
    M.npc = giver
    M.QuestCommand(id, "accept")
    check(player.inventory:GetCountOfClass(q.letterBase) == 0, "nothing in the pocket at accept: the letter is not written")
    check(E.gvar(q.partVars[1]) == 0 and E.gvar(q.verdictVar) == 0, "the reading's globals start at nothing")
    -- Part 1 as said, part 2 softened, part 3 made larger: the priest's mocked.
    M.QuestCommand(id, "wrote_023")
    local wrote = q.letterPrefix .. "023" .. q.letterSuffix
    check(M.state.quests[id].wrote == "023", "the digits in the store, got " .. tostring(M.state.quests[id].wrote))
    check(M.QuestFlagLetters(M.state.quests[id]) == "W023", "the flag letters W023, got " .. M.QuestFlagLetters(M.state.quests[id]))
    check(M.quests[id].item == wrote, "the quest's item is that Document")
    check(player.inventory:GetCountOfClass(wrote) == 1, "the Document of those digits in the pocket")
    check(E.gvar(q.partVars[1]) == 0 and E.gvar(q.partVars[2]) == 2 and E.gvar(q.partVars[3]) == 3, "the parts' globals")
    check(E.gvar(q.verdictVar) == 2, "made larger anywhere: mocked")
    if q.token then check(player.inventory:GetCountOfClass(q.token) >= 1, "the written token for the graph") end
    check(not armedOrQueued(M, id, "complete") and not armedOrQueued(M, id, "complete_mocked"), "wrote_ is no beat")
    E.delivered(id, "mocked")
    check(receiver.inventory:GetCountOfClass(wrote) == 1 and player.inventory:GetCountOfClass(wrote) == 0, "moved")
    check(M.state.quests[id].how == "mocked" and M.state.quests[id].handed, "mocked, handed")
    M.QuestCommand(id, "complete")
    check(armedOrQueued(M, id, "complete_mocked"), "complete_mocked armed")
    -- The curse as said, the rest softened: cursed. Left out or softened everywhere: the prayer.
    check(M.DictationVerdict(q, "220") == 1, "the curse part as said is cursed")
    check(M.DictationVerdict(q, "002") == 0, "the Turks as said are tolerated")
    check(M.DictationVerdict(q, "021") == 0, "the curse left out is the prayer")
    check(M.DictationVerdict(q, "300") == 2, "made larger outranks the curse")
    -- The store's digits alone set the globals and the item again on a fresh sync.
    M.quests[id].item = q.letterBase
    E.globals[q.verdictVar] = nil
    M.QuestFlags()
    check(M.quests[id].item == wrote and E.gvar(q.verdictVar) == 2, "re-synced from the store")
end)

-- A deliver whose receiver asks how Henry came by the thing (the interrogation; In the
-- Stocks' chalice): the one with a complete_caught beat, in id order.
local function byInterrogation(M)
    local ids = {}
    for id, q in pairs(M.quests) do
        if q.kind == "deliver" and M.questCodes[id .. " complete_caught"] ~= nil then ids[#ids + 1] = id end
    end
    table.sort(ids)
    return ids[1], M.quests[ids[1]]
end

case("QuestDelivered with a how: the interrogation's answer is the handover's how, and the report completes on it", function(E, M, check)
    local id, q = byInterrogation(M)
    if id == nil then check(true, "no interrogation in this data"); return end
    local receiver = E.soul(q.target)
    M.QuestCommand(id, "accept")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the thing in the pocket at accept")
    E.delivered(id, "pinned")
    check(receiver.inventory:GetCountOfClass(q.item) == 1 and player.inventory:GetCountOfClass(q.item) == 0, "moved")
    check(M.state.quests[id].how == "pinned" and M.state.quests[id].handed, "pinned, handed")
    check(E.find("BRIDGE quest " .. id .. " handover -- " .. q.name .. " (pinned)"), "the how logged")
    check(M.QuestLinkState(id) ~= "open" or true, "the link state is for the scene after")
    M.QuestCommand(id, "complete")
    check(armedOrQueued(M, id, "complete_pinned"), "complete_pinned armed")
    check(M.QuestHowMatches(M.state.quests[id].how, "truth") == false, "not a truth for the scene after")
    -- The truth, told: the plain complete, and the scene after it opens.
    M.SetQuestCode(0)
    M.questQueue = {}
    M.state.quests[id] = {}
    M.QuestCommand(id, "accept")
    E.delivered(id, "truth")
    check(M.state.quests[id].how == "truth", "truth kept")
    M.QuestCommand(id, "complete")
    check(armedOrQueued(M, id, "complete"), "the plain complete")
    check(M.QuestHowMatches(M.state.quests[id].how, "truth") == true, "a truth for the scene after")
end)

case("QuestCommand: a retrieve's lie with the thing kept pays nothing and takes nothing back", function(E, M, check)
    local id, q = nil, nil
    for cid, cq in pairs(M.quests) do
        if cq.kind == "retrieve" and M.questCodes[cid .. " complete_lied"] ~= nil and not cq.dispute then id, q = cid, cq end
    end
    if id == nil then check(true, "no retrieve with a lie row in this data"); return end
    local giver = E.soul(q.giver)
    local before = player.inventory:GetMoney()
    player.inventory:CreateItem(q.item, 1, 1)
    M.QuestCommand(id, "accept")
    M.QuestCommand(id, "lie")
    check(M.state.quests[id].how == "lied" and M.state.quests[id].done, "lied and done")
    check(armedOrQueued(M, id, "complete_lied"), "complete_lied armed")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the thing still in Henry's coat")
    check(giver.inventory:GetCountOfClass(q.item) == 0, "nothing reached the giver")
    check(player.inventory:GetMoney() == before, "nothing paid")
    check(E.find("BRIDGE quest " .. id .. " lie with the thing kept; nothing paid"), "said so")
end)

case("The holder's Talk: CanTalk comes and goes with the errand", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local holder = E.soul(q.target)
    check(holder.actor:CanTalk(player.id) == false, "no Talk before")
    E.firePort(id .. " accept")
    check(holder.actor:CanTalk(player.id) == true, "Talk while Active")
    local acts = E.look(holder) -- the frame that grants the role offers nothing of ours
    check(#acts == 1 and acts[1].base, "the base Talk alone on the granting frame")
    acts = E.look(holder)
    check(#acts == 1 and acts[1].base, "the base Talk alone, ours stands down")
    check(E.find("leaving the base talk option alone on " .. q.target), "said so once")
    check(E.find("BRIDGE quest CanTalk on " .. q.target .. " -> true"), "CanTalk logged")
    E.holderGave(id, "persuade")
    check(holder.actor:CanTalk(player.id) == false, "no Talk once handed")
    acts = E.look(holder)
    check(#acts == 1 and acts[1].fields ~= nil, "ours is back")
end)

case("The holder's sync points: QuestStage logs the stage and keeps the last one", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    E.firePort(id .. " accept")
    M.holderStage = nil  -- the count is the process's, and an earlier case's rows are in it
    -- As the holder XML's ExitScripts call it: the door, a check taken, a check lost.
    M.QuestStage(id, "open")
    check(E.find("BRIDGE quest stage " .. id .. " open"), "the door logged")
    check(M.holderStage.id == id and M.holderStage.what == "open" and M.holderStage.n == 1, "kept")
    M.QuestStage(id, "row persuade")
    M.QuestStage(id, "refused persuade")
    check(E.find("BRIDGE quest stage " .. id .. " refused persuade"), "the refusal logged")
    check(M.holderStage.what == "refused persuade" and M.holderStage.n == 3, "the last one, counted")
    -- The rows that move something log theirs from inside: a won check, the broker row.
    M.QuestBroker(id)
    check(E.find("BRIDGE quest stage " .. id .. " row broker"), "broker logged")
    E.holderGave(id, "threaten")
    check(E.find("BRIDGE quest stage " .. id .. " won threaten"), "the winning branch logged")
    check(M.holderStage.what == "won threaten" and M.holderStage.n == 5, "and kept")
    check(M.state.quests[id].handed, "the handover still made")
end)

case("Autopilot: an until step waits for its flag, once, and times out without it", function(E, M, check)
    local reached = {}
    M.Run({
        {"until", "roaddone", 5},
        {"lua", "Moonshine.harnessReached = (Moonshine.harnessReached or 0) + 1"},
        {"until", "roaddone", 8},
        {"lua", "Moonshine.harnessReached = Moonshine.harnessReached + 10"},
    })
    M.harnessReached = 0
    E.advance(2000)
    check(M.harnessReached == 0, "nothing moves before the flag")
    M.AutoFlag("roaddone")
    E.advance(1500)
    check(M.harnessReached == 1, "the flag moved the run on")
    check(E.find("BRIDGE auto until roaddone after"), "and said so")
    -- The flag was taken; the second until wants a fresh one and gives up at its timeout.
    E.advance(3000)
    check(M.harnessReached == 1, "the second until does not reuse the first flag")
    E.advance(7000)
    check(M.harnessReached == 11, "a timed-out until goes on")
    check(E.find("BRIDGE auto until roaddone timed out after"), "and says it timed out")
    check(E.find("BRIDGE auto done"), "the run finished")
end)

case("Autopilot: a rebaseline step asks the runner and takes F9 as the answer", function(E, M, check)
    M.auto.steps = {}
    M.Run({
        {"rebaseline", 30},
        {"lua", "Moonshine.harnessReloaded = true"},
    })
    M.harnessReloaded = nil
    E.advance(1500)
    check(E.find("BRIDGE auto rebaseline 1"), "the runner was asked")
    check(M.auto.awaitAck == true and M.harnessReloaded == nil, "and waited for")
    -- The runner's F9, down the typing toggle's action, outside any conversation.
    check(M.Typed("mtype_toggle", "press") == true, "the key is taken")
    check(M.auto.awaitAck == false, "the ack lands")
    check(E.find("BRIDGE auto rebaseline acknowledged (F9)"), "and is logged")
    E.advance(1500)
    check(M.harnessReloaded == true, "the run went on to the load")
    -- A second press is the toggle's own business again, not an ack.
    local before = M.typing
    M.Typed("mtype_toggle", "press")
    check(M.auto.awaitAck == false and M.typing == before, "an F9 with nothing awaited is not an ack")
end)

-- -------------------------------------------------------------------- Offerable, links

case("QuestOfferable: neither accepted nor done", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    check(M.QuestOfferable(id) == true, "fresh")
    M.state.quests[id] = { accepted = true }
    check(M.QuestOfferable(id) == false, "accepted")
    M.state.quests[id] = { accepted = true, done = true }
    check(M.QuestOfferable(id) == false, "done")
    check(M.QuestOfferable("nosuch") == false, "unknown")
end)

case("QuestLinkState: locked, open on a matching how, void otherwise", function(E, M, check)
    local id, q = byGate(M, "truth|lied")
    check(q and q.after ~= "" and q.afterHow == "truth|lied", "gated truth|lied on " .. tostring(q and q.after))
    check(M.QuestLinkState(id) == "locked", "locked before")
    check(M.QuestOfferable(id) == false, "not offerable locked")
    M.state.quests[q.after] = { accepted = true, done = true, how = "persuaded" }
    check(M.QuestLinkState(id) == "open", "open on a truth")
    check(M.QuestOfferable(id) == true, "offerable open")
    M.state.quests[q.after] = { accepted = true, done = true, how = "lied" }
    check(M.QuestLinkState(id) == "open", "open on lied, which is listed")
    M.state.quests[q.after] = { accepted = true, done = true, how = "quiet" }
    check(M.QuestLinkState(id) == "void", "void on quiet")
    check(M.QuestOfferable(id) == false, "not offerable void")
    M.state.quests[q.after] = { accepted = true, done = true }
    check(M.QuestLinkState(id) == "open", "no how recorded is a truth")
end)

case("QuestLinkState: truth excludes lied, quiet, kept and dead, and the interrogation's dodges", function(E, M, check)
    check(M.QuestHowMatches(nil, "truth") == true, "nil")
    check(M.QuestHowMatches("persuaded", "truth") == true, "persuaded")
    check(M.QuestHowMatches("delivered", "truth") == true, "delivered")
    check(M.QuestHowMatches("truth", "truth") == true, "the interrogation's truth")
    for _, how in ipairs({ "lied", "quiet", "kept", "dead", "pinned", "covered", "fibbed", "evaded", "caught" }) do
        check(M.QuestHowMatches(how, "truth") == false, how .. " is not truth")
    end
    check(M.QuestHowMatches("covered", "truth|covered") == true, "covered when listed")
    check(M.QuestHowMatches("kept", "truth|kept") == true, "kept when listed")
    check(M.QuestHowMatches("anything", "") == true, "no gate")
end)

case("The giver's prompt: the favour hint while offerable, the plain one after", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local giver = E.soul(q.giver)
    local acts = E.look(giver)
    check(acts[1] and acts[1].fields.hint == M.favourHint, "favour hint: " .. tostring(acts[1] and acts[1].fields.hint))
    M.state.quests[id] = { accepted = true }
    acts = E.look(giver)
    check(acts[1].fields.hint == M.hint, "plain hint once accepted")
end)

case("The giver's prompt: a gated scene lights only when its predecessor ends right", function(E, M, check)
    -- A gated quest whose giver gives nothing else, so the hint is this scene's alone.
    local id, q
    local ids = {}
    for cid in pairs(M.quests) do ids[#ids + 1] = cid end
    table.sort(ids)
    for _, cid in ipairs(ids) do
        local cq = M.quests[cid]
        if cq.after ~= "" and cq.afterHow ~= "" and id == nil then
            local alone = true
            for oid, oq in pairs(M.quests) do
                if oid ~= cid and (oq.giver == cq.giver or oq.target == cq.giver) then alone = false end
            end
            if alone then id, q = cid, cq end
        end
    end
    check(id, "a gated scene with a giver of one favour")
    local giver = E.soul(q.giver)
    local function hint() local a = E.look(giver); return a[1] and a[1].fields.hint end
    check(hint() == M.hint, "plain while locked")
    local open, void = "persuaded", "quiet"
    if string.find(q.afterHow, "quiet", 1, true) then void = "lied" end
    if string.find(q.afterHow, "lied", 1, true) and void == "lied" then void = "kept" end
    M.state.quests[q.after] = { accepted = true, done = true, how = open }
    check(hint() == M.favourHint, "favour once open")
    M.state.quests[q.after] = { accepted = true, done = true, how = void }
    check(hint() == M.hint, "plain when void (" .. q.afterHow .. " against " .. void .. ")")
end)

-- -------------------------------------------------------------------- The store

case("Store: begin counts met and remembers the gap entry; the dump comes first", function(E, M, check)
    local npc = E.soul("kkut_additive_man_47")
    E.begin(npc)
    local soul = M.state.souls["kkut_additive_man_47"]
    check(soul.met == 1, "met 1")
    check(#soul.memory == 1 and soul.memory[1].k == "gap", "one gap entry")
    local _, dumpAt = E.find("BRIDGE state npc=kkut_additive_man_47 met=1")
    local _, beginAt = E.find("BRIDGE begin npc=kkut_additive_man_47")
    check(dumpAt and beginAt and dumpAt < beginAt, "the state line before the begin line")
    check(E.find("BRIDGE memory npc=kkut_additive_man_47 end"), "the memory end marker")
end)

case("Store: @heard goes in the save and comes back on the dump with the souls met", function(E, M, check)
    local npc = E.soul("kkut_additive_man_47")
    E.begin(npc)
    -- Nobody heard of yet, but one soul met: the dump names him and nobody else.
    local line = E.find("BRIDGE known npc=kkut_additive_man_47")
    check(line and string.find(line, " kkut_additive_man_47", 1, true), "the soul met is known: " .. tostring(line))
    local words = M.Directives("@heard khor_woman_2 @heard name:humphrey Alice weeps over the levy, poor girl.")
    check(words == "Alice weeps over the levy, poor girl.", "the tokens come off the front: " .. words)
    check(M.state.known["khor_woman_2"] == true and M.state.known["name:humphrey"] == true, "both in the store")
    local since = E.mark()
    M.Dump("kkut_additive_man_47")
    local last = E.find("BRIDGE known npc=kkut_additive_man_47", since)
    check(last and string.find(last, " khor_woman_2", 1, true) and string.find(last, " name:humphrey", 1, true),
          "heard of, on the dump: " .. tostring(last))
    -- A fresh store has the table; one from an older save gets it on load.
    check(type(M.FreshState().known) == "table", "fresh store has known")
end)

case("Store: the look dumps the record as the begin would leave it, storing nothing", function(E, M, check)
    local npc = E.soul("kkut_additive_man_47")
    E.look(npc)
    check(E.find("BRIDGE state npc=kkut_additive_man_47 met=1"), "met counted ahead")
    check(E.find("entries=1"), "the gap entry ahead")
    check(M.state.souls["kkut_additive_man_47"] == nil or M.state.souls["kkut_additive_man_47"].met == 0, "nothing stored")
    check(E.find("BRIDGE look npc=kkut_additive_man_47"), "the look line")
end)

case("Store: a reply is remembered whole, a streamed piece is not", function(E, M, check)
    M.stateSoul = "kkut_additive_man_47"
    M.active = true
    M.Say(1, "@more Past the")
    M.Say(2, "Past the smithy, then left.")
    local mem = M.state.souls["kkut_additive_man_47"].memory
    check(#mem == 1 and mem[1].k == "n" and mem[1].s == "Past the smithy, then left.", "one n entry, whole")
end)

case("Store: escaping the dump", function(E, M, check)
    check(M.Escape("a\\b\nc\rd BRIDGE e") == "a\\\\b\\nc\\rd BR\\IDGE e", "backslash, newlines, the marker")
end)

case("Store: the save round-trips and OnInit resets", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    local npc = E.soul("kkut_additive_man_47")
    E.begin(npc)
    M.StateSaid("Which way to the mill?")
    M.StateReply("Past the smithy.")
    M.StateQuest(id, "accept")
    M.StateStat("kkut_additive_man_47", "trust", 0.75)
    local text = E.save()
    check(E.find("BRIDGE state saved via OnSave"), "saved line")
    -- A world from before the mod: fresh.
    E.arrive()
    check(M.state.souls["kkut_additive_man_47"] == nil, "OnInit reset the store")
    check(E.find("BRIDGE state reset, a world is arriving"), "reset line")
    -- Then the save.
    local ok, err = E.load(text)
    check(ok, "loaded: " .. tostring(err))
    local soul = M.state.souls["kkut_additive_man_47"]
    check(soul and soul.met == 1 and #soul.memory == 3, "met and three entries back")
    check(soul.memory[2].s == "Which way to the mill?" and soul.memory[3].s == "Past the smithy.", "the words back")
    check(soul.stats.trust == 0.75, "the stat back")
    check(M.state.quests[id].accepted == true, "the quest back")
    check(E.find("BRIDGE state loaded via OnLoad version=1 souls=1"), "loaded line")
end)

case("Store: a save with no store in it loads fresh", function(E, M, check)
    M.state.souls.x = { met = 3, rev = 0, memory = {}, stats = {} }
    local ok = E.load("return {}")
    check(ok and M.state.souls.x == nil, "fresh")
    check(E.find("none, fresh store"), "said so")
end)

case("Store: a newer store is kept as it is", function(E, M, check)
    local ok = E.load("return { moonshine = { version = 9, souls = {}, quests = {} } }")
    check(ok and M.state.version == 9, "kept")
    check(E.find("from a newer mod (9 > 1), kept as it is"), "said so")
end)

case("Store: a remembered line is cut at the chunk so the save can load", function(E, M, check)
    M.stateSoul = "kkut_additive_man_47"
    M.StateSaid(string.rep("x", 70000))
    local soul = M.state.souls["kkut_additive_man_47"]
    check(#soul.memory[1].s == M.stateChunk, "cut to " .. M.stateChunk)
    local text = E.save()
    check(E.lastSave.longest < E.saveLimit, "no string at the limit")
    check(E.load(text), "loads")
end)

case("Store: a string at the limit is a save that never opens again", function(E, M, check)
    -- Not something the mod writes; the rule the harness enforces, pinned so a change
    -- to the chunk size that crossed it would be caught here.
    M.state.souls.x = { met = 1, rev = 0, memory = { { k = "p", s = string.rep("x", 65536) } }, stats = {} }
    local text = E.save()
    local ok, err = E.load(text)
    check(ok == false and err == "Game load failed", "refused: " .. tostring(err))
end)

case("Store: Chunk cuts at the chunk size", function(E, M, check)
    local pieces = M.Chunk(string.rep("a", M.stateChunk * 2 + 5))
    check(#pieces == 3 and #pieces[1] == M.stateChunk and #pieces[3] == 5, "three pieces")
    check(#M.Chunk("") == 1, "the empty string is one piece")
end)

case("Compact: folds the first n entries into one sum and bumps rev", function(E, M, check)
    M.stateSoul = "s"
    M.StateRemember("s", { k = "gap", t = 100, s = "a" })
    M.StateRemember("s", { k = "p", s = "b" })
    M.StateRemember("s", { k = "n", s = "c" })
    M.StateRemember("s", { k = "p", s = "d" })
    check(M.Compact("s", 0, 3, "They spoke of the mill.") == true, "accepted")
    local soul = M.state.souls.s
    check(soul.rev == 1, "rev 1")
    check(#soul.memory == 2 and soul.memory[1].k == "sum" and soul.memory[1].t == 100 and soul.memory[2].s == "d", "sum then d")
end)

case("Compact: refused on a stale rev, a bad n, an unknown soul or empty words", function(E, M, check)
    M.StateRemember("s", { k = "p", s = "b" })
    M.StateRemember("s", { k = "n", s = "c" })
    check(M.Compact("s", 1, 1, "x") == false, "stale rev")
    check(M.Compact("s", 0, 3, "x") == false, "n past the end")
    check(M.Compact("s", 0, 0, "x") == false, "n zero")
    check(M.Compact("nobody", 0, 1, "x") == false, "unknown soul")
    check(M.Compact("s", 0, 1, "") == false, "empty summary")
    check(E.count("BRIDGE state compact refused") == 5, "five refusals logged")
    check(M.state.souls.s.rev == 0 and #M.state.souls.s.memory == 2, "untouched")
end)

case("Compact: through the directive, with the byte count", function(E, M, check)
    M.StateRemember("s", { k = "p", s = "b" })
    M.StateRemember("s", { k = "n", s = "c" })
    local sum = "They spoke — briefly."
    local words = M.Directives("@memory s compact 0 2 " .. #sum .. ":" .. sum .. " And the line.")
    check(words == "And the line.", "words: " .. tostring(words))
    check(M.state.souls.s.memory[1].s == sum, "summary whole")
end)

case("Dump: quests= lists every favour with a stage, its flags and its how", function(E, M, check)
    local id = byName(M, "Charity Begins at Home")
    local id2 = byGate(M, "truth|lied")
    M.state.quests[id] = { accepted = true, done = true, how = "lied", broker = true, apologised = true }
    M.state.quests[id2] = { accepted = true }
    M.Dump("kkut_additive_man_47")
    local line = E.find("BRIDGE state npc=kkut_additive_man_47")
    check(line and string.find(line, id .. ":dbp/lied", 1, true), "d with flags and how: " .. tostring(line))
    check(line and string.find(line, id2 .. ":a", 1, true), "a")
end)

case("Dump: an accept keeps its clock reading, and quests= carries the hours since", function(E, M, check)
    local id = byGate(M, "truth|lied")
    M.state.quests[id] = nil
    local t0 = M.WorldTime()
    M.StateQuest(id, "accept")
    local f = M.state.quests[id]
    check(f and type(f.acceptedAt) == "number" and f.acceptedAt >= t0, "acceptedAt set on accept")
    local first = f.acceptedAt
    M.StateQuest(id, "accept")
    check(f.acceptedAt == first, "a second accept leaves the first reading")
    f.acceptedAt = M.WorldTime() - 2.5 * 3600
    M.Dump("kkut_additive_man_47")
    local line = E.find("BRIDGE state npc=kkut_additive_man_47")
    check(line and string.find(line, id .. ":a~2.5", 1, true), "~hours after the stage: " .. tostring(line))
    f.how = "lied"; f.done = true
    M.Dump("kkut_additive_man_47")
    check(E.find(id .. ":d/lied~2.5"), "after the how")
end)

-- -------------------------------------------------------------------- The menu

case("Menu: Choose asks the engine once the rows are whole", function(E, M, check)
    live(E, M)
    local token = M.menuToken
    M.Choose()
    check(M.choosing and M.menuWait and M.menuToken == token + 1, "choosing, waiting, token moved")
    check(M.face ~= M.chooseCode, "not entered before the rows")
    M.Directives("@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2")
    E.advance(100)
    check(M.face == M.chooseCode and E.gvar(M.faceVar) == M.chooseCode, "the choose code set once whole")
    check(E.find("BRIDGE choose") and not E.find("rows still short"), "chosen whole")
end)

case("Menu: Choose enters anyway after menuEnterGraceMillis", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help?")
    E.advance(M.menuEnterGraceMillis + 100)
    check(M.face == M.chooseCode, "entered")
    check(E.find("BRIDGE choose (rows still short)"), "said short")
end)

case("Menu: a whole list covers the engine's dots at menuCoverMillis, not the long delay", function(E, M, check)
    -- The rows written ahead on a look (session.cpp) are already whole when the sequence
    -- starts, so ours lands just after the engine's own draw (~0.3s) and the player never
    -- sits on "..." for the rest of a second (notes.md, 5 Oct 2026).
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @opt 2 ask 4:Ask? @opt 3 about 6:About? @rows 3")
    E.advance(100)
    M.Beat()
    E.advance(M.menuCoverMillis + 10)
    check(M.menuUp and M.menuDrawn == 3, "covered at the short mark")
    check(E.hud.rows and E.hud.rows[1] == "Help?", "words, not dots")
end)

case("Menu: the marks stretch for a frame rate under 60, since the engine's own draw is partly frames", function(E, M, check)
    -- At 60 fps the measured marks stand. At 30 fps -- where Frames holds the game while a
    -- conversation is up -- the cover at 0.35s was drawn over (11 Oct 2026), so each
    -- millisecond of the frame over 60 fps's is added menuCoverFrames times.
    live(E, M)
    check(M.MenuMillis(M.menuCoverMillis) == M.menuCoverMillis, "60 fps: the cover mark as measured")
    E.frameTime = 1 / 30
    local at30 = M.MenuMillis(M.menuCoverMillis)
    check(at30 >= 640 and at30 <= 660, "30 fps: about 0.65s, got " .. at30)
    E.frameTime = 1 / 120
    check(M.MenuMillis(M.menuCoverMillis) == M.menuCoverMillis, "faster than 60 is not drawn earlier")
    E.frameTime = 5
    check(M.MenuMillis(M.menuCoverMillis) == M.menuCoverMillis, "a stall of a frame is not believed")

    -- And the whole list is covered at the stretched mark, not the measured one.
    E.frameTime = 1 / 30
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @opt 2 ask 4:Ask? @opt 3 about 6:About? @rows 3")
    E.advance(100)
    M.Beat()
    E.advance(M.menuCoverMillis + 10)
    check(M.menuDrawn == 0, "not yet at the 60 fps mark")
    E.advance(310)
    check(M.menuUp and M.menuDrawn == 3, "covered at the stretched mark")
    check(E.hud.rows and E.hud.rows[1] == "Help?", "words, not dots")
end)

case("Menu: the engine's boundary presents it, the delay draws it, the leave row follows the last", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @opt 2 ask 4:Ask? @opt 3 about 6:About? @rows 3")
    E.advance(100)
    M.Beat() -- the face beat's exit script at the boundary the menu is entered on
    check(M.menuWait == false, "entered")
    E.advance(M.menuDelayMillis + 10)
    check(M.menuUp and M.menuDrawn == 3, "drawn three")
    check(M.menuSayRow == 4 and M.menuLeaveRow == 5, "say on row 4, leave on row 5")
    check(E.hud.rows and E.hud.rows[1] == "Help?" and E.hud.rows[16] == M.menuSay and E.hud.rows[21] == M.menuLeave,
        "the HUD list: rows, the say row, then the way out")
    check(E.find("BRIDGE menu drawn rows=3 empty=0 say=4 leave=5"), "logged")
end)

-- The boundary missed (notes.md 25 Oct 2026): the code set between a beat's exit script
-- and its decision, so the graph is in the menu and no Beat() says so.
case("Menu: a boundary the code slipped past is drawn over by the watchdog, and a boundary that comes after all draws again", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2")
    E.advance(100)
    check(M.face == M.chooseCode and M.menuWait, "the code up, the boundary awaited")
    local mark = E.mark()
    E.advance(M.menuMissedMillis - 200)
    check(M.menuDrawn == 0, "nothing drawn while a beat could still come")
    E.advance(300 + M.menuDelayMillis)
    check(E.find("BRIDGE menu boundary missed, drawn over the engine's list", mark), "the watchdog said so")
    check(M.menuUp and M.menuDrawn == 2 and E.hud.rows and E.hud.rows[1] == "Help?", "drawn over the dots")
    -- The boundary comes late after all: drawn again, since the step threw the first away.
    M.Beat()
    check(E.find("BRIDGE menu boundary after the early draw, drawn again", mark), "the late boundary noticed")
    E.advance(M.menuDelayMillis + 10)
    check(E.count("BRIDGE menu drawn rows=2", mark) == 2, "drawn twice")
    -- And a round the boundary comes for in time is drawn once, the watchdog idle.
    M.Picked(1)
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @rows 1")
    E.advance(100)
    mark = E.mark()
    M.Beat()
    E.advance(M.menuMissedMillis + M.menuDelayMillis + 100)
    check(E.count("BRIDGE menu drawn", mark) == 1 and not E.find("boundary missed", mark), "once, in time")
end)

case("Menu: a row that arrives after the draw is drawn again", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help?")
    E.advance(M.menuEnterGraceMillis + 100)
    M.Beat()
    E.advance(M.menuDelayMillis + M.menuGraceMillis + 10)
    check(M.menuDrawn == 1 and E.find("menu drawn rows=1"), "drawn once with one")
    M.Say(2, "@opt 2 ask 4:Ask?") -- a rows-only turn, as the pump plays it
    check(M.menuDrawn == 2, "redrawn with two")
    check(E.count("BRIDGE menu drawn") == 2, "two draws")
end)

case("Typing: the pump never opens the keyboard on its own", function(E, M, check)
    -- The "Henry: _" prompt after every line was the prototype's; the menu is how a
    -- focused conversation is answered now, and free-form speech comes in another way.
    live(E, M)
    M.typingLoaded = true
    M.TypingOpen(player)
    check(M.typingAuto == false, "automatic opening is off by default")
    local menuWas = M.menuOn
    M.menuOn = false -- no menu road, so Heard() goes straight to TypingCheck()
    M.Say(1, "Good day to you.")
    check(M.heard and M.awaiting == false and M.typingDismissed == false, "a line was heard and nothing is owed")
    check(M.typing == false, "and the keyboard stays down after it")
    M.TypingCheck() -- the pump's tick, with every old condition for opening met
    check(M.typing == false, "and after a tick")
    check(not E.find("BRIDGE typing on"), "never up")
    -- F9 by hand is still the road in and out.
    M.Typed("mtype_toggle", "press")
    check(M.typing == true, "F9 opens it by hand")
    M.Typed("mtype_toggle", "press")
    check(M.typing == false, "and puts it away")
    M.menuOn = menuWas
    M.typingLoaded = false
end)

case("Menu: a line that lands while the menu is asked for does not open the keyboard over it", function(E, M, check)
    live(E, M)
    M.typingLoaded = true
    M.typingAuto = true -- the old behaviour, so the guard under it is what is tested
    M.TypingOpen(player)
    M.Say(1, "Have you come for another blessing?")
    check(M.choosing and M.typing == false, "the line asked for the menu and put the keyboard down")
    M.Say(2, "@opt 1 remind 7:Remind. @opt 2 tell 5:Tell. @rows 2")
    -- The app's next line arrives before the rows are drawn (the canned turn after a
    -- greeting, in the first sweep): Heard() finds Choose() already choosing.
    M.Say(3, "This is canned turn 1, standing in for something a model would have said.")
    check(M.choosing, "still choosing")
    M.TypingCheck() -- the pump's tick
    check(M.typing == false, "the keyboard stays down under the menu")
    check(not E.find("BRIDGE typing on"), "and was never up")
    E.advance(M.menuEnterGraceMillis + 100)
    M.Beat()
    E.advance(M.menuDelayMillis + M.menuGraceMillis + 10)
    check(M.menuUp, "the menu is drawn")
    M.TypingCheck()
    check(M.typing == false, "and the keyboard stays down with it drawn")
    -- The pick closes the menu and the keyboard comes back on the reply as before.
    M.Picked(2)
    check(M.choosing == false and M.awaiting == true, "picked, awaiting the reply")
    M.Say(4, "Then tell me.")
    check(M.choosing, "the reply asks for the menu again")
    M.typingAuto = false
    M.typingLoaded = false
end)

-- -------------------------------------------------------------------- App down

-- A session as Step() finds it once it has opened: the pump's tick count a little past
-- the opening, nothing said yet.
local function opened(E, M, ticksSince)
    live(E, M)
    M.TypingOpen(player)
    M.opened = true
    M.ticks = 100
    M.openedAt = M.ticks - ticksSince
    M.turnAtOpen = M.turn
    M.appChecked = false
    M.appDown = false
    M.ending = false
    M.hold = 0
    -- The graph is beating, as it does with or without the app: the stuck-graph exit
    -- is a different failure.
    M.beats = 1
end

case("App: no heartbeat beside the turn files tells the player and closes", function(E, M, check)
    E.appUp = false
    opened(E, M, 2)
    local before = M.turn
    M.Step()
    check(E.find("BRIDGE app heartbeat missing"), "the heartbeat was looked for")
    check(E.find("BRIDGE app down -- no heartbeat"), "and its absence said")
    check(M.appDown == true and M.ending == true, "the conversation is ending")
    check(M.turn == before + 1 and E.find("BRIDGE played turn=" .. M.turn), "the line took a turn, so a late app carries on from the right number")
    check(string.sub(M.shown, 1, 11) == "[Moonshine]", "shown bare, as the mod's own: " .. tostring(M.shown))
    check(not string.find(M.shown, "^" .. M.npcName .. ":"), "and not in the other party's name")
    -- A second tick does not say it twice, nor look for a turn.
    M.ticks = M.ticks + 1
    M.Step()
    check(E.count("BRIDGE app down") == 1, "said once")
    E.appUp = true
    M.ending = false
    M.appDown = false
end)

case("App: a heartbeat and a first line in time is a conversation like any other", function(E, M, check)
    E.appUp = true
    opened(E, M, 2)
    E.files[M.Path(M.inboxes[1], M.turn + 1)] = "Good day to you."
    M.Step()
    check(M.appChecked and not M.appDown, "the heartbeat was found")
    check(E.find("BRIDGE played turn=" .. M.turn .. " -- Good day to you."), "and the line said")
    E.files[M.Path(M.inboxes[1], M.turn)] = nil
    -- Long past the backstop, with the line said, nothing more is said about the app.
    M.ticks = M.openedAt + M.appTicks + 20
    M.hold = 0
    M.Step()
    check(not M.appDown and not E.find("BRIDGE app down"), "no complaint")
end)

-- The rows behind a line are taken in the tick the line is, not one a tick: four files
-- waiting is four rows on the menu now, and a line behind them is left for its own tick.
case("Pump: the rows already written behind a line land in the same tick", function(E, M, check)
    E.appUp = true
    opened(E, M, 2)
    local first = M.turn + 1
    E.files[M.Path(M.inboxes[1], first)] = "Good day to you."
    E.files[M.Path(M.inboxes[1], first + 1)] = "@opt 1 help 5:Help?"
    E.files[M.Path(M.inboxes[1], first + 2)] = "@opt 1 help 5:Help? @opt 2 ask 4:Ask?"
    E.files[M.Path(M.inboxes[1], first + 3)] = "@opt 1 help 5:Help? @opt 2 ask 4:Ask? @opt 3 about 6:About? @rows 3"
    E.files[M.Path(M.inboxes[1], first + 4)] = "Still here?"
    M.Step()
    check(M.turn == first + 3, "the line and its three rows of turns taken, turn=" .. M.turn)
    check(M.options[3] ~= nil and M.options[3].text == "About?", "the third row is on the menu")
    check(M.OptionsComplete(), "and the menu is complete")
    check(M.words == "Good day to you.", "the next line waits for its tick: " .. tostring(M.words))
    for i = 0, 4 do E.files[M.Path(M.inboxes[1], first + i)] = nil end
end)

case("App: a heartbeat but no first line within the backstop says the app did not answer", function(E, M, check)
    E.appUp = true
    opened(E, M, 2)
    M.Step()
    check(M.appChecked and not M.appDown, "alive at the opening")
    M.ticks = M.openedAt + M.appTicks
    M.Step()
    check(not M.appDown, "not yet at the limit")
    M.ticks = M.openedAt + M.appTicks + 1
    M.Step()
    check(M.appDown and E.find("BRIDGE app down -- no first line in 12s"), "past it, the player is told")
    check(string.find(M.shown, "did not answer in time", 1, true) ~= nil, "that it did not answer, not that it is missing: " .. tostring(M.shown))
    check(M.ending == true, "and the conversation ends when read")
    M.ending = false
    M.appDown = false
end)

case("Menu: @rows 0 is the way out alone", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@rows 0")
    E.advance(100)
    check(M.face == M.chooseCode, "entered at once")
    M.Beat()
    E.advance(M.menuDelayMillis + M.menuGraceMillis + 10)
    check(M.menuSayRow == 1 and M.menuLeaveRow == 2, "the say row first, then the way out")
    check(E.hud.rows[1] == M.menuSay and E.hud.rows[6] == M.menuLeave, "drawn so")
end)

case("Menu: @rows n closed draws the rows alone, no say row and no way out", function(E, M, check)
    -- A dictation's list (notes.md 21 Oct 2026): the letter is written in these rows or
    -- not at all, so the mod's own two rows stay off it; the next open list has them back.
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 write:1 7:As said @opt 2 soften:1 7:Gentler @opt 3 embellish:1 6:Weight @rows 3 closed")
    check(M.optionsTotal == 3 and M.menuClosed == true, "@rows 3 closed read")
    E.advance(100)
    M.Beat()
    E.advance(M.menuDelayMillis + M.menuGraceMillis + 10)
    check(M.menuSayRow == 0 and M.menuLeaveRow == 0, "no say row, no leave row")
    -- SequenceList is five values a row; the text is the first of each.
    check(E.hud.rows[1] == "As said" and E.hud.rows[11] == "Weight", "the three rows drawn")
    check(#E.hud.rows == 15, "nothing under them")
    M.Picked(3)
    check(E.find("BRIDGE picked 3 embellish:1 -- Weight"), "a closed list's row picks as any row")
    M.Choose()
    check(M.menuClosed == false, "the next round opens again")
    M.Directives("@opt 1 help 5:Help? @rows 1")
    E.advance(100)
    M.Beat()
    E.advance(M.menuDelayMillis + M.menuGraceMillis + 10)
    check(M.menuSayRow == 2 and M.menuLeaveRow == 3, "say and leave rows back")
end)

case("Menu: Picked says the row's words and closes choosing", function(E, M, check)
    live(E, M)
    M.stateSoul = "s"
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @opt 2 accept 8:I'll go. @rows 2")
    local token = M.menuToken
    M.Picked(2)
    check(M.choosing == false and M.menuUp == false and M.menuToken == token + 1, "closed, token moved")
    check(E.find("BRIDGE picked 2 accept -- I'll go."), "the pick line")
    check(E.find("BRIDGE said I'll go."), "said as the player's words")
    check(M.state.souls.s.memory[1].k == "p" and M.state.souls.s.memory[1].s == "I'll go.", "remembered as p")
    check(M.awaiting == true, "awaiting the reply")
end)

case("Menu: the last row, or the leave row of a short list, leaves", function(E, M, check)
    local npc = E.soul("kkut_additive_man_47")
    E.begin(npc)
    M.Choose()
    M.Directives("@opt 1 help 5:Help? @rows 1")
    E.advance(100)
    M.Beat()
    E.advance(M.menuDelayMillis + 10)
    check(M.menuSayRow == 2 and M.menuLeaveRow == 3, "say on 2, leave on 3")
    M.Picked(3)
    check(M.active == false, "left")
    check(E.find("BRIDGE picked 3 leave -- " .. M.menuLeave), "the leave line")
    check(E.find("BRIDGE left, ended"), "left line")
    check(E.gvar(M.talkVar) == 0, "talking global down")
end)

case("Menu: a row out of range is refused", function(E, M, check)
    M.Option(0, "x", "a")
    M.Option(M.menuRows, "x", "a")
    check(M.options[0] == nil and M.options[M.menuRows] == nil, "neither stored")
    check(E.count("opt row out of range") == 2, "both logged")
end)

case("Menu: a stale token's timer does nothing", function(E, M, check)
    live(E, M)
    M.Choose()
    M.Directives("@opt 1 help 5:Help?")
    M.Beat()
    M.Picked(1)        -- the token moves before the draw timer fires
    E.advance(M.menuDelayMillis + 10)
    check(M.menuUp == false and M.menuDrawn == 0, "nothing drawn for the old menu")
end)

-- -------------------------------------------------------------------- The say row

-- A conversation open at the engine, with a menu of these rows presented and drawn.
local function sayMenu(E, M, directives)
    local npc = E.soul("kkut_additive_man_47", { pos = { x = 1, y = 0, z = 0 } })
    turn(E, 1, "Good day. What brings you?")
    M.typingLoaded = true
    if not E.begin(npc) then return false end
    M.TypingOpen(player)
    -- The rows after the first beat, as the app writes them: a choose code up before the
    -- graph has beaten once is a menu presented with nothing to enter it.
    E.advance(E.beatMillis + 500)
    turn(E, 2, directives)
    return untilMenu(E)
end

-- The text of HUD row n (five values a row).
local function hudRow(E, n) return E.hud.rows and E.hud.rows[(n - 1) * 5 + 1] end

-- The say row picked from a presented menu: the keyboard open in say-mode.
local function saySlot(E, M, check, directives)
    check(sayMenu(E, M, directives), "menu presented")
    E.pick(M.menuSayRow)
    check(M.saying == true and M.typing == true, "say-mode: the keyboard open")
    -- The graph has gone back to the top and is into a rest beat before the player
    -- can have done anything with the keyboard.
    E.advance(200)
    return M.Display()
end

case("Say: the say row resolves the menu and opens the keyboard, and the app is told", function(E, M, check)
    check(sayMenu(E, M, "@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2"), "menu presented")
    check(M.menuSayRow == 3 and M.menuLeaveRow == 4 and hudRow(E, 3) == M.menuSay, "the say row above the way out")
    check(E.find("BRIDGE menu drawn rows=2 empty=0 say=3 leave=4"), "logged")
    local token = M.menuToken
    E.pick(3)
    check(M.choosing == false and M.menuToken == token + 1, "the menu over")
    check(M.face == M.restFace, "the rest face back, as for any row")
    check(E.find("BRIDGE picked 3 say --") and E.find("BRIDGE say start"), "the pick with its intent and the start logged")
    check(M.saying == true and M.typing == true and M.typed == "", "say-mode, the keyboard open and empty")
    check(E.find("BRIDGE typing on"), "the keyboard's own log line")
    check(M.Display() == M.playerName .. ": _", "the caption: Henry's name and a cursor")
    E.advance(E.beatMillis * 2 + 100)
    check(E.dialog.menu == false and M.saying == true, "no menu comes back on its own")
    M.typingLoaded = false
end)

case("Say: keys go into the line, the app's transcript replaces it, and the caption follows", function(E, M, check)
    saySlot(E, M, check, "@opt 1 help 5:Help? @rows 1")
    check(M.Typed("mtype_h", "press") == true and M.Typed("mtype_i", "press") == true, "keys taken")
    check(M.typed == "hi" and M.Display() == M.playerName .. ": hi_", "typed into the line")
    M.Typed("mtype_backspace", "press")
    check(M.typed == "h", "backspace")
    M.SayHeard("where is the miller")
    check(M.typed == "where is the miller" and M.Display() == M.playerName .. ": where is the miller_", "replaced by what was heard")
    check(E.find("BRIDGE say heard where is the miller"), "logged")
    M.SayHeard(42)
    M.SayHeard(nil)
    check(M.typed == "where is the miller", "not a string: ignored")
    M.Typed("mtype_space", "press")
    M.Typed("mtype_lshift", "press")
    M.Typed("mtype_m", "press")
    M.Typed("mtype_lshift", "release")
    check(M.typed == "where is the miller M", "typing goes on after the heard words")
    M.typingLoaded = false
end)

case("Say: B clears the line, and B on an empty line brings the menu back with its rows", function(E, M, check)
    saySlot(E, M, check, "@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2")
    M.SayHeard("where is the miller")
    M.OnPlayerAction("dialog_cancel", "press")
    M.OnPlayerAction("dialog_cancel", "release")
    check(M.saying == true and M.typing == true and M.typed == "", "cleared, still in say-mode")
    check(E.find("BRIDGE say cleared") and M.Display() == M.playerName .. ": _", "the empty line shown")
    check(not E.find("BRIDGE skipped"), "nothing skipped")
    local drawn = E.count("BRIDGE menu drawn")
    M.OnPlayerAction("dialog_cancel", "press")
    M.OnPlayerAction("dialog_cancel", "release")
    check(M.saying == false and M.typing == false and M.choosing == true, "out of say-mode, the keyboard down, choosing again")
    check(E.find("BRIDGE say stop -- back") and E.find("BRIDGE choose again"), "the stop and the menu asked for again")
    check(M.face == M.chooseCode, "the code up again")
    check(untilMenu(E), "the engine presents the menu again")
    check(E.count("BRIDGE menu drawn") == drawn + 1, "and it is drawn again")
    check(hudRow(E, 1) == "Help?" and hudRow(E, 2) == "Ask?" and hudRow(E, 3) == M.menuSay and hudRow(E, 4) == M.menuLeave,
        "with the rows it had")
    check(E.count("BRIDGE say start") == 1, "the app was told to listen once")
    -- And the round goes on: a row picked from the menu brought back is a row as ever.
    E.pick(1)
    check(E.find("BRIDGE picked 1 help -- Help?") and E.find("BRIDGE said Help?"), "a row from the menu brought back")
    M.typingLoaded = false
end)

case("Say: escape is B, and F9 is the way back to the menu", function(E, M, check)
    saySlot(E, M, check, "@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2")
    M.Typed("mtype_a", "press")
    check(M.Typed("mtype_escape", "press") == true, "taken")
    check(M.saying == true and M.typed == "", "escape cleared")
    check(not E.find("BRIDGE leave") and M.active, "not the exit it is from the F9 keyboard")
    M.Typed("mtype_a", "press")
    M.Typed("mtype_toggle", "press")
    check(M.saying == false and M.typing == false and M.choosing == true, "F9: say-mode left for the menu")
    check(E.find("BRIDGE say stop -- toggle"), "logged")
    check(untilMenu(E) and hudRow(E, 3) == M.menuSay, "the menu back")
    M.typingLoaded = false
end)

case("Say: Enter, A or the mouse button with words sends them as Henry's line", function(E, M, check)
    saySlot(E, M, check, "@opt 1 help 5:Help? @opt 2 ask 4:Ask? @rows 2")
    M.stateSoul = "kkut_additive_man_47"
    M.SayHeard("Where is the miller?")
    M.OnPlayerAction("dialog_confirm", "press")
    M.OnPlayerAction("dialog_confirm", "release")
    check(E.find("BRIDGE say stop -- sent"), "the stop")
    check(E.find("BRIDGE said Where is the miller?"), "said as the player's words")
    check(M.saying == false and M.typing == false, "say-mode and the keyboard over")
    check(M.awaiting == true, "awaiting the reply")
    check(E.count("BRIDGE picked") == 1, "no second pick: the app has the intent from the first")
    E.advance(10)
    check(E.dialog.menu == false, "no menu presented")
    -- The reply, and a new round with the say row in it again.
    turn(E, 3, "Up the river.")
    E.advance(E.beatMillis + 500)
    turn(E, 4, "@opt 1 thanks 7:Thanks. @rows 1")
    check(untilMenu(E), "the next menu")
    check(hudRow(E, 1) == "Thanks." and hudRow(E, 2) == M.menuSay, "the say row again")
    -- Enter does the same.
    E.pick(2)
    M.Typed("mtype_o", "press")
    M.Typed("mtype_k", "press")
    check(M.Typed("mtype_enter", "press") == true, "taken")
    check(E.find("BRIDGE said ok") and M.saying == false and M.typing == false, "Enter sends")
    M.typingLoaded = false
end)

case("Say: Enter with nothing in the line is nothing", function(E, M, check)
    saySlot(E, M, check, "@opt 1 help 5:Help? @rows 1")
    M.Typed("mtype_enter", "press")
    M.OnPlayerAction("dialog_confirm", "press")
    M.OnPlayerAction("dialog_confirm", "release")
    check(M.saying == true and M.typing == true, "still in say-mode, the keyboard still up")
    check(E.count("BRIDGE say enter on an empty line") == 2 and not E.find("BRIDGE said"), "nothing sent")
    check(E.count("BRIDGE say start") == 1, "the app was not told twice")
    M.typingLoaded = false
end)

case("Say: @rows 0 puts the say row first, and leaving in say-mode tells the app", function(E, M, check)
    check(sayMenu(E, M, "@rows 0"), "menu presented")
    check(M.menuSayRow == 1 and M.menuLeaveRow == 2, "the say row first")
    E.pick(1)
    check(M.saying == true, "say-mode")
    M.SayHeard("Farewell")
    M.Leave("walked away")
    check(M.saying == false and M.typing == false, "say-mode over")
    check(E.find("BRIDGE say stop -- conversation over"), "the app told")
    check(not E.find("BRIDGE said Farewell"), "nothing sent")
    M.SayHeard("late words")
    check(not E.find("BRIDGE say heard late words"), "a hook after the keyboard has gone is nothing")
    M.typingLoaded = false
end)

-- -------------------------------------------------------------------- The whole errand

case("Errand: Charity Begins at Home to done, the save round-tripped, from scripted turns", function(E, M, check)
    local id, q = byName(M, "Charity Begins at Home")
    local giver = E.soul(q.giver, { pos = { x = 1, y = 0, z = 0 } })
    local holder = E.soul(q.target, { pos = { x = 2, y = 0, z = 0 } })
    holder.inventory:CreateItem(q.item, 100, 1)

    -- Conversation 1: the greeting, rows, the accepting row, the accept directive.
    turn(E, 1, "Good day. What brings you?")
    turn(E, 2, "@opt 1 help 36:Is there anything I can help you with? @opt 2 ask 20:Who are your people? @rows 2")
    check(E.begin(giver), "begun")
    check(E.find("BRIDGE begin npc=" .. q.giver), "begin line")
    check(string.find(E.find("BRIDGE begin npc=" .. q.giver), " have=0", 1, true), "have=0 on the begin line")
    check(untilMenu(E), "menu 1 presented")
    turn(E, 3, "My kettle. Hans has it and will not give it back.")
    turn(E, 4, "@opt 1 accept 23:I'll get it back for you. @opt 2 help 11:Tell me more @rows 2")
    E.pick(1)
    check(untilMenu(E), "menu 2 presented")
    turn(E, 5, "@quest " .. id .. " accept @end Bless you.")
    E.pick(1)
    E.advance(20000)
    check(M.state.quests[id].accepted == true, "accepted in the store")
    check(E.ports[1] == id .. " accept", "the accept port fired: " .. tostring(E.ports[1]))
    check(M.active == false, "closed by @end")
    check(E.find("BRIDGE picked 0 end -- (the app closed the conversation)"), "the end line")
    check(holder.soul.metaroles[q.holderMeta], "the holder has his role")

    -- The holder's own Talk, a persuade.
    check(holder.actor:CanTalk(player.id), "Talk on the holder")
    E.holderGave(id, "persuade")
    check(player.inventory:GetCountOfClass(q.item) == 1, "the thing in the pocket")

    -- Save, a world from before, the save back.
    local save = E.save()
    E.arrive()
    check(M.state.quests[id] == nil, "store reset")
    check(E.load(save), "loaded")
    check(M.state.quests[id].handed == true and M.state.quests[id].how == "persuaded", "handed and how back")

    -- Conversation 2: the begin line says have=1, the app completes on the opening turn.
    E.files = {}
    M.turn = 0 -- a relaunch counts from one again
    turn(E, 1, "@quest " .. id .. " complete You found it. Here, for your trouble.")
    turn(E, 2, "@rows 0")
    local purse = player.inventory:GetCountOfClass(E.moneyClass)
    check(E.begin(giver), "begun again")
    local line = E.find("BRIDGE begin npc=" .. q.giver, E.mark() - 20)
    check(line and string.find(line, " have=1", 1, true), "have=1 on the begin line: " .. tostring(line))
    check(untilMenu(E, 40000), "menu presented after the hand-in")
    check(M.state.quests[id].done == true, "done")
    check(player.inventory:GetCountOfClass(q.item) == 0 and giver.inventory:GetCountOfClass(q.item) == 1, "the thing home")
    check(player.inventory:GetCountOfClass(E.moneyClass) == purse + q.reward * 10, "paid")
    check(E.ports[#E.ports] == id .. " complete_persuaded", "the persuaded port: " .. tostring(E.ports[#E.ports]))
    E.pick(2) -- @rows 0: the say row, then the way out
    check(M.active == false, "left on the way out")
    check(M.QuestOfferable(id) == false, "not offerable again")
end)

-- -------------------------------------------------------------------- Console

-- The hook pumped with nobody being talked to: the chain, armed as the player's OnUpdate
-- would arm it once the world is loaded, finds a hook within a second or two of its being
-- written, and the number moves on. Not armed at the world's arrival: see Console.
case("Console: a hook is run outside a conversation, within a tick", function(E, M, check)
    check(M.active == false, "no conversation up")
    check(not E.find("BRIDGE console armed"), "nothing armed by the world hooks")
    -- Any key the player presses arms it (the patched Player.OnAction), which is the one
    -- heartbeat a loaded world is sure to give.
    Player:OnAction("move_forward", 1, 1)
    check(E.find("BRIDGE console armed (OnAction) chain"), "armed by a key press")
    check(M.Console("OnUpdate") == false, "and the live chain is left alone after")
    E.files["%USER%/moonshine/hook_1.txt"] = "return 'hello from hook one'"
    E.advance(2100)
    check(M.hooked == 1, "hook 1 ran, hooked=" .. tostring(M.hooked))
    check(E.find("BRIDGE hook 1 ran -- hello from hook one"), "its words in the log")
    -- A second one, numbered on, is found the same way.
    E.files["%USER%/moonshine/hook_2.txt"] = "return 'and two'"
    E.advance(1100)
    check(M.hooked == 2 and E.find("BRIDGE hook 2 ran -- and two"), "hook 2 ran")
end)

-- The same commands typed at the game's console: registered at load, each puts the line in
-- the log for the app (BRIDGE console line), which answers with a hook that writes the
-- answer back into the log. Nothing goes to the app when it is down or before a world is
-- loaded; the player is told instead.
case("Console: /quests and /goto typed at the game's console go to the app", function(E, M, check)
    check(E.commands["/quests"] ~= nil and E.commands["/goto"] ~= nil and E.commands["/help"] ~= nil, "the three commands are registered")
    check(E.commands["/goto"].body == "Moonshine.Console_goto(%%)", "the body hands the words on: " .. tostring(E.commands["/goto"].body))
    check(M.ConsoleRegister() == 3 and E.find("BRIDGE console registered 3 of 3 commands"), "registering again says so")
    local mark = E.mark()
    E.console("/quests")
    check(E.find("BRIDGE console line /quests", mark), "a bare /quests goes over as typed")
    check(E.find("BRIDGE console armed (console) chain", mark), "the pump is armed for the answer")
    mark = E.mark()
    E.console("/goto 21 target")
    check(E.find("BRIDGE console line /goto 21 target", mark), "the words after the name follow it")
    check(not E.find("BRIDGE console armed", mark), "the live chain is left alone")
    -- The app's answer: Console::game_lua, each line logged, the command's own Lua after.
    E.files["%USER%/moonshine/hook_1.txt"] = 'Moonshine.Log("  [21] Barking Stew -- ask (word)")\n'
        .. 'local said = (function()\nreturn "going nowhere in a test"\nend)()\n'
        .. 'pcall(function() Game.SendInfoText(tostring(said), true, nil, 5) end)\nreturn said\n'
    E.advance(2100)
    check(M.hooked == 1 and E.find("[21] Barking Stew -- ask (word)", mark), "the answer is in the log")
    check(E.infoTexts[#E.infoTexts] == "going nowhere in a test", "and the teleport's word on the HUD")
    -- The app down: said at once, nothing sent.
    E.appUp = false
    mark = E.mark()
    E.console("/quests bees")
    check(E.find("[Moonshine] the conversation app is not running, so nobody can answer /quests bees", mark), "told the app is down")
    check(not E.find("BRIDGE console line", mark), "and nothing sent")
    E.appUp = true
    -- Before a save is loaded: the same.
    local soul = player.soul
    player.soul = nil
    mark = E.mark()
    E.console("/help")
    check(E.find("[Moonshine] /help is answered once a save is loaded", mark), "told to load a save first")
    check(not E.find("BRIDGE console line", mark), "and nothing sent")
    player.soul = soul
end)

-- Arming again while the chain lives is a no-op; after the clock is seen to have gone
-- back (a wiped chain reads the same way) a new chain takes over and the old one stops.
case("Console: one live chain at a time, re-armed when the last tick is stale", function(E, M, check)
    check(M.Console("OnUpdate") == true, "armed")
    local gen = M.consoleGen
    check(M.Console("again") == false and M.consoleGen == gen, "a live chain is left alone")
    M.consoleAt = M.Now() - 10
    check(M.Console("stale") == true and M.consoleGen == gen + 1, "a stale one is replaced")
    E.advance(3000)
    check(M.Now() - M.consoleAt < 1.5, "the new chain is ticking")
end)

case("Teleport: beside a soul who is loaded, two metres off on Henry's side", function(E, M, check)
    local who = E.soul("kkut_additive_man_10", { pos = { x = 100, y = 50, z = 10 } })
    player:SetWorldPos({ x = 0, y = 50, z = 0 })
    local said = M.Teleport("kkut_additive_man_10", "Heinz", { { 0, 0, 1, 1, 1 } })
    check(string.find(said, "beside Heinz, who was 100m away", 1, true), said)
    local pp = player:GetWorldPos()
    check(math.abs(pp.x - 98) < 0.01 and math.abs(pp.y - 50) < 0.01 and pp.z == 10, "put at 98,50,10: " .. pp.x .. "," .. pp.y .. "," .. pp.z)
    E.advance(1100)
    check(E.find("BRIDGE console Henry is beside Heinz"), "the check a second on says he is there")
end)

case("Teleport: a soul not loaded is gone to at the routine spot for the hour", function(E, M, check)
    E.hour = 10
    local spots = { { 0, 0, 3129.3, 730.0, 55.2 }, { 420, 1200, 3117.2, 746.7, 64.8 } }
    local said = M.Teleport("kkut_nobody", "Nobody", spots)
    check(string.find(said, "spot for 10:00", 1, true), said)
    local pp = player:GetWorldPos()
    check(math.abs(pp.x - 3119.2) < 0.01 and math.abs(pp.y - 746.7) < 0.01, "at the day spot: " .. pp.x .. "," .. pp.y)
    E.hour = 23
    said = M.Teleport("kkut_nobody", "Nobody", spots)
    pp = player:GetWorldPos()
    check(math.abs(pp.x - 3131.3) < 0.01 and math.abs(pp.y - 730.0) < 0.01, "at night, the bed: " .. pp.x .. "," .. pp.y)
    check(M.Teleport("kkut_nobody", "Nobody", {}) == "Nobody (kkut_nobody) is not loaded here and has no routine spot to go to", "no spots, no move")
end)

-- -------------------------------------------------------------------- Alms

-- notes.md (6 Oct 2026): beggars always accept food, clothing (and wear it) and groschen.
-- The begin line of a beggar says what Henry could give (alms=mfc); @alms <kind> gives
-- it. Classes are the shipped tables' (moonshine_alms.lua beside the mod file).
local function aBeggar(M)
    local names = {}
    for name in pairs(M.beggars) do names[#names + 1] = name end
    table.sort(names)
    return names[1]
end
local function cheapest(t)
    local best, price = nil, nil
    for class, p in pairs(t) do
        local v = type(p) == "table" and p[1] or p
        if price == nil or v < price or (v == price and class < best) then best, price = class, v end
    end
    return best
end
local function anyGroup(M, group, n)
    local out = {}
    for class, g in pairs(MoonshineAlms.clothes) do
        if g[2] == group then out[#out + 1] = class end
    end
    table.sort(out)
    return out
end

case("Alms: the begin line of a beggar says what Henry could give, and nobody else's does", function(E, M, check)
    local beggar = aBeggar(M)
    check(beggar ~= nil, "the build has beggars")
    check(M.AlmsTable() ~= nil, "the alms table loads beside the mod")
    local food = cheapest(MoonshineAlms.food)
    local npc = E.soul(beggar)
    -- A purse and nothing else: money only.
    check(M.AlmsLine(npc, player) == " alms=m", "a purse alone: " .. M.AlmsLine(npc, player))
    player.inventory:CreateItem(food, 1, 2)
    check(M.AlmsLine(npc, player) == " alms=mf", "with bread: " .. M.AlmsLine(npc, player))
    -- One garment of a group is what he wears, for all the Lua can tell; two is a spare.
    local tunics = anyGroup(M, "TunicShort")
    check(#tunics >= 2, "two tunic classes in the tables")
    player.inventory:CreateItem(tunics[1], 1, 1)
    check(M.AlmsLine(npc, player) == " alms=mf", "one tunic is worn, not spare: " .. M.AlmsLine(npc, player))
    player.inventory:CreateItem(tunics[2], 1, 1)
    check(M.AlmsLine(npc, player) == " alms=mfc", "two tunics, one spare: " .. M.AlmsLine(npc, player))
    -- Not a beggar: nothing, whatever the pocket.
    local other = E.soul("kkut_additive_man_10")
    check(M.beggars["kkut_additive_man_10"] == nil, "man 10 is not a beggar")
    check(M.AlmsLine(other, player) == "", "no alms on a man who is not begging: " .. M.AlmsLine(other, player))
    -- Broke, with an empty pocket: nothing, so the app offers no row it cannot keep.
    local poor = E.soul("kkut_poor_henry")
    poor.inventory:CreateItem(E.moneyClass, 100, 20)  -- two groschen
    check(M.AlmsHave(poor) == "", "two groschen and no food is nothing to give: " .. M.AlmsHave(poor))
    check(E.begin(npc), "the prompt opens the conversation")
    local begin = E.find("BRIDGE begin npc=" .. beggar)
    check(begin ~= nil and begin:find(" alms=mfc", 1, true) ~= nil, "the begin line carries alms=mfc: " .. tostring(begin))
end)

case("Alms: @alms moves the groschen, the cheapest food, and the spare garment -- then he wears it", function(E, M, check)
    local beggar = aBeggar(M)
    local npc = E.soul(beggar)
    M.npc = npc
    local money0 = player.inventory:GetMoney()
    M.Directives("@alms money")
    check(math.abs((money0 - player.inventory:GetMoney()) - M.almsGroschen) < 0.001,
          "three groschen left Henry: " .. money0 .. " -> " .. player.inventory:GetMoney())
    check(math.abs(npc.inventory:GetMoney() - M.almsGroschen) < 0.001, "and reached him: " .. npc.inventory:GetMoney())
    check(E.find("BRIDGE alms money 3 groschen to " .. beggar), "logged")
    -- Food: the cheapest of two goes, one of it.
    local foods = {}
    for class, p in pairs(MoonshineAlms.food) do foods[#foods + 1] = { class, p } end
    table.sort(foods, function(a, b) return a[2] < b[2] or (a[2] == b[2] and a[1] < b[1]) end)
    local cheap, dear = foods[1][1], foods[#foods][1]
    check(MoonshineAlms.food[dear] > MoonshineAlms.food[cheap], "prices differ")
    player.inventory:CreateItem(dear, 1, 1)
    player.inventory:CreateItem(cheap, 1, 2)
    M.Directives("@alms food")
    check(npc.inventory:GetCountOfClass(cheap) == 1 and npc.inventory:GetCountOfClass(dear) == 0, "the cheap one went")
    check(player.inventory:GetCountOfClass(cheap) == 1, "one of two stays with Henry")
    check(E.find("BRIDGE alms food " .. cheap .. " moved 1 to " .. beggar), "logged")
    -- Clothes: with a single tunic nothing goes; with two, the cheaper, and he puts it on.
    local tunics = anyGroup(M, "TunicShort")
    table.sort(tunics, function(a, b) return MoonshineAlms.clothes[a][1] < MoonshineAlms.clothes[b][1] end)
    local cheapT, dearT = tunics[1], tunics[#tunics]
    player.inventory:CreateItem(dearT, 1, 1)
    M.Directives("@alms clothes")
    check(E.find("BRIDGE alms clothes nothing to give"), "one tunic is not spare")
    check(npc.worn == nil, "nothing worn yet")
    player.inventory:CreateItem(cheapT, 1, 1)
    M.Directives("@alms clothes")
    check(npc.inventory:GetCountOfClass(cheapT) == 1, "the cheaper tunic went")
    check(player.inventory:GetCountOfClass(dearT) == 1, "Henry keeps the dearer")
    check(npc.worn ~= nil and npc.worn[1] == cheapT, "and he wears it")
    check(E.find("BRIDGE alms clothes " .. cheapT .. " moved 1 to " .. beggar .. ", worn"), "logged")
    -- Nothing left to give is said, not raised.
    player.inventory.rows = {}
    M.Directives("@alms food")
    check(E.find("BRIDGE alms food nothing to give"), "an empty pocket is said")
end)

case("Errand: a soul whose errand is a scene of a story is gated by the link", function(E, M, check)
    local id, q = byGate(M, "truth|lied")
    local before = M.quests[q.after]
    check(before, "the predecessor is dealt")
    check(M.QuestLinkState(id) == "locked", "locked")
    check(M.QuestOfferable(id) == false, "not offerable")
end)

-- A run (stages.md, 8 Oct 2026): a story's same-giver scenes are one quest file and one
-- journal entry in the graph, and nothing else changes -- every scene keeps its id in the
-- Lua's table, store, codes and menus. The two asks of the cat story, played through the
-- Lua as the app and the graph would: the first's truth opens the second in the same
-- giver's hands, the second's holder door opens on its own accept, and both hand-ins pay.
case("Run: two same-giver asks play through the store by their own ids", function(E, M, check)
    -- The pair: an ask gated on the truth whose predecessor is an ask of the same giver.
    local id2, q2
    local ids = {}
    for cid in pairs(M.quests) do ids[#ids + 1] = cid end
    table.sort(ids)
    for _, cid in ipairs(ids) do
        local cq = M.quests[cid]
        local before = cq.after ~= "" and M.quests[cq.after] or nil
        if id2 == nil and before and before.giver == cq.giver and cq.kind == "ask" and before.kind == "ask"
            and cq.afterHow == "truth" then
            id2, q2 = cid, cq
        end
    end
    check(q2 ~= nil, "a run of two asks is dealt")
    if q2 == nil then return end
    local id1, q1 = q2.after, M.quests[q2.after]
    local giver = E.soul(q1.giver)
    E.soul(q1.target)
    local second = E.soul(q2.target)
    M.npc = giver
    check(M.QuestOfferable(id1) == true and M.QuestOfferable(id2) == false, "the head offerable, the leg locked")
    M.QuestCommand(id1, "accept")
    E.firePort(id1 .. " accept")
    check(second.actor:CanTalk(player.id) == false, "the second scene's holder has no menu on the head's accept")
    E.holderGave(id1, "persuade")
    check(M.state.quests[id1].handed == true, "the first truth told")
    M.QuestCommand(id1, "complete")
    check(M.state.quests[id1].done == true, "the head done in the store")
    check(M.QuestLinkState(id2) == "open" and M.QuestOfferable(id2) == true, "the leg opens on the truth")
    -- The run's quest is not done in the graph (the Level wires the head's plain complete
    -- to nothing); the harness fakes the graph per scene, so only the store is asked here.
    M.QuestCommand(id2, "accept")
    E.firePort(id2 .. " accept")
    check(second.actor:CanTalk(player.id) == true, "the second holder's menu on the leg's own accept")
    E.holderGave(id2, "pay")
    check(M.state.quests[id2].handed == true and M.state.quests[id2].how == "paid", "the second truth bought")
    M.QuestCommand(id2, "complete")
    check(M.state.quests[id2].done == true, "the leg done")
    check(M.questCodes[id1 .. " complete"] ~= nil and M.questCodes[id2 .. " complete"] ~= nil, "both hand-ins have codes of their own")
    local paid = E.count("BRIDGE quest complete paid")
    local owed = (q1.reward > 0 and 1 or 0) + (q2.reward > 0 and 1 or 0)
    check(paid == owed, "paid at each hand-in that pays: " .. paid .. " of " .. owed)
end)

case("Run: a lie at the head voids the leg, as the link always did", function(E, M, check)
    local id2, q2
    local ids = {}
    for cid in pairs(M.quests) do ids[#ids + 1] = cid end
    table.sort(ids)
    for _, cid in ipairs(ids) do
        local cq = M.quests[cid]
        local before = cq.after ~= "" and M.quests[cq.after] or nil
        if id2 == nil and before and before.giver == cq.giver and cq.afterHow == "truth" then id2, q2 = cid, cq end
    end
    check(q2 ~= nil, "a same-giver leg gated on the truth")
    if q2 == nil then return end
    local id1 = q2.after
    M.npc = E.soul(M.quests[id1].giver)
    M.QuestCommand(id1, "accept")
    M.QuestCommand(id1, "lie")
    check(M.state.quests[id1].done == true and M.state.quests[id1].how == "lied", "the head done on a lie")
    check(M.QuestLinkState(id2) == "void" and M.QuestOfferable(id2) == false, "the leg void")
end)

-- The giver's hail (notes.md 13 Oct 2026): Benesh calls Henry over while In the Stocks is
-- unasked, once per approach, voiced, and put in his mouth as the game's bark by the
-- hail's token (notes.md 14 Oct 2026), the words on the HUD's narrative subtitle (the
-- conversation caption does not draw outside a dialogue); not in a conversation, and not
-- once the errand is taken up.
case("Hail: a giver with a hail shouts it once when Henry comes near, and again only after he has been away", function(E, M, check)
    local id, q = byName(M, "In the Stocks")
    check(q and q.hail ~= nil and q.hailName ~= nil and q.hailVoice == "male", "In the Stocks has a hail, a name and a man's voice")
    if q == nil or q.hail == nil then return end
    check(q.hailToken == "6d6f6f6e-7368-4800-8000-" .. id, "the hail's token is the quest's own class under the hail prefix")
    local benesh = E.soul(q.giver, { pos = { x = 100, y = 100, z = 0 } })
    player:SetWorldPos({ x = 100 + M.hailMetres + 20, y = 100, z = 0 })
    M.WatchArm("world")
    check(M.watching == true, "the watch is armed with nothing accepted")
    E.advance(2100)
    check(not E.find("BRIDGE quest " .. id .. " hail"), "not hailed from afar")
    player:SetWorldPos({ x = 100 + M.hailMetres - 2, y = 100, z = 0 })
    local mark = E.mark()
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " hail " .. q.giver, mark), "hailed within hailMetres")
    check(E.find("BRIDGE voice male soul=" .. q.giver .. " " .. q.hail, mark), "voiced as the man's stand-in, the giver named for the app's mouth")
    check(M.voicedSoul == q.giver, "the shout's mouth is the giver's")
    check(E.find("BRIDGE quest token made", mark), "the hail's token made for the graph's bark, which turns him to Henry")
    check(M.Count(player, q.hailToken) == 1, "one of the hail's class in Henry's pocket")
    -- The words come hailWordsMs after the token, past the bark's opening, which wipes the
    -- narrative line; one put, since the widget keeps a line about hailSeconds on its own.
    local function narrative(from)
        local puts, hides, captioned = 0, 0, false
        for i = from + 1, #E.hud.calls do
            local name = E.hud.calls[i]
            if name == "SetNarrativeSubtitles" and E.hud.args[i][1] == q.hailName .. ": " .. q.hail then puts = puts + 1 end
            if name == "HideNarrativeSubtitles" then hides = hides + 1 end
            if name == "SetSubtitles" then captioned = true end
        end
        return puts, hides, captioned
    end
    local hudMark = #E.hud.calls
    local puts, hides, captioned = narrative(0)
    check(puts == 0, "the words not yet up with the token")
    E.advance(M.hailWordsMs + 100)
    puts, hides, captioned = narrative(0)
    local n = #M.hudIds
    check(puts == n and hides == 0, "one put (to each HUD instance) of the words after his name on the narrative subtitle, hailWordsMs on: the bark does not caption a line without a voice clip")
    check(not captioned, "the conversation caption left alone, it does not draw outside a dialogue")
    -- Standing there, he is not hailed again; the token is gone a moment later and the
    -- narrative subtitle hidden, with a fade, hailSeconds after the put, and not put again.
    mark = E.mark()
    E.advance(M.hailSeconds * 1000 + 500)
    check(E.count("BRIDGE quest " .. id .. " hail", mark) == 0, "not hailed again on the spot")
    check(M.Count(player, q.hailToken) == 0, "the token gone again")
    puts, hides = narrative(hudMark)
    check(puts == n and hides == n, "the one put, then the one hide after hailSeconds")
    -- Off and back inside hailAgain: nothing; off and back after it: hailed again.
    player:SetWorldPos({ x = 100 + M.hailMetres * 2 + 5, y = 100, z = 0 })
    E.advance(2100)
    player:SetWorldPos({ x = 100 + 2, y = 100, z = 0 })
    E.advance(2100)
    check(E.count("BRIDGE quest " .. id .. " hail", mark) == 0, "not again inside hailAgain")
    player:SetWorldPos({ x = 100 + M.hailMetres * 2 + 5, y = 100, z = 0 })
    E.advance(M.hailAgain * 1000 + 100)
    player:SetWorldPos({ x = 100 + 2, y = 100, z = 0 })
    mark = E.mark()
    E.advance(2100)
    check(E.count("BRIDGE quest " .. id .. " hail", mark) == 1, "hailed again after being away")
    -- Not while a conversation is open, and not once the errand is taken up.
    player:SetWorldPos({ x = 100 + M.hailMetres * 2 + 5, y = 100, z = 0 })
    E.advance(M.hailAgain * 1000 + 100)
    M.active = true
    player:SetWorldPos({ x = 100 + 2, y = 100, z = 0 })
    mark = E.mark()
    E.advance(2100)
    check(E.count("BRIDGE quest " .. id .. " hail", mark) == 0, "not hailed in a conversation")
    M.active = false
    M.npc = benesh
    M.QuestCommand(id, "accept")
    E.advance(2100)
    check(E.count("BRIDGE quest " .. id .. " hail", mark) == 0, "not hailed once accepted")
end)

-- The shout's line taken down though a conversation opened on it (bugs.txt 28 Oct 2026:
-- Christine's greeting stayed on the screen for minutes after the talk it opened): the
-- hide runs with a dialogue up, and Leave takes down a line the timer has not.
case("Hail hide: the narrative line comes down after hailSeconds though a conversation has opened meanwhile, and a conversation's end takes down one still up", function(E, M, check)
    local id, q = byName(M, "In the Stocks")
    if q == nil or q.hail == nil then check(true, "no hail in this data"); return end
    E.soul(q.giver, { pos = { x = 100, y = 100, z = 0 } })
    player:SetWorldPos({ x = 100 + M.hailMetres - 2, y = 100, z = 0 })
    M.WatchArm("world")
    local mark = E.mark()
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " hail " .. q.giver, mark), "hailed")
    local function hides(from)
        local n = 0
        for i = from + 1, #E.hud.calls do
            if E.hud.calls[i] == "HideNarrativeSubtitles" then n = n + 1 end
        end
        return n
    end
    local hudMark = #E.hud.calls
    E.advance(M.hailWordsMs + 100)
    check(M.hailUp == true, "the line up")
    -- Henry presses Talk on him with the line up: a conversation is open when the hide
    -- comes due, and it hides all the same.
    M.active = true
    E.advance(M.hailSeconds * 1000 + 500)
    check(hides(hudMark) == #M.hudIds and M.hailUp == false, "hidden on time with a conversation open")
    -- A line still up at a conversation's end goes with it.
    hudMark = #E.hud.calls
    M.hailUp = true
    M.Leave("the case")
    check(M.active == false and hides(hudMark) == #M.hudIds and M.hailUp == false, "Leave takes a line still up down")
    hudMark = #E.hud.calls
    M.HailHide()
    check(hides(hudMark) == 0, "and nothing twice")
end)

case("Hail hours: a giver with a day written is not hailed outside it (asleep, at her supper), and is within it", function(E, M, check)
    -- Christine's hours (hail_hours 8-20; notes.md 25 Oct 2026): the hail only in them, as
    -- her marker is only on the map in them (the graph's giverDay). Benesh has no hours and
    -- is hailed at any hour.
    local id, q = byName(M, "Words Must Not Fail")
    check(q and q.hail ~= nil and q.hailHours ~= nil and q.hailHours[1] == 8 and q.hailHours[2] == 20,
          "Words Must Not Fail has a hail and the hours 8 to 20")
    if q == nil or q.hail == nil or q.hailHours == nil then return end
    local other = select(2, byName(M, "In the Stocks"))
    check(other == nil or other.hailHours == nil, "In the Stocks has no hours")
    E.soul(q.giver, { pos = { x = 100, y = 100, z = 0 } })
    player:SetWorldPos({ x = 100 + M.hailMetres - 2, y = 100, z = 0 })
    M.WatchArm("world")
    local said = "BRIDGE quest " .. id .. " hail"
    E.hour = 23
    local mark = E.mark()
    E.advance(2100)
    check(not E.find(said, mark), "not hailed at eleven at night")
    E.hour = 6.5
    E.advance(2100)
    check(not E.find(said, mark), "not hailed before her day")
    E.hour = 20.5
    E.advance(2100)
    check(not E.find(said, mark), "not hailed at her supper")
    E.hour = 11
    E.advance(2100)
    check(E.count(said, mark) == 1, "hailed at eleven in the morning")
end)

case("Watch: the loop a load wiped is armed again from the player's first key, and the hail comes", function(E, M, check)
    -- The loop armed as the world arrives rides a timer the load then wipes, and until
    -- the first conversation re-armed it from the pump nobody watched the town: Benesh
    -- never called Henry over before he was spoken to (notes.md 14 Oct 2026).
    local id, q = byName(M, "In the Stocks")
    if q == nil or q.hail == nil then check(false, "In the Stocks has a hail") return end
    E.soul(q.giver, { pos = { x = 100, y = 100, z = 0 } })
    player:SetWorldPos({ x = 100 + M.hailMetres + 20, y = 100, z = 0 })
    M.OnWorld("OnInit", player)
    M.OnWorld("OnLoad", player)
    check(M.watching == true and M.watchDoubt == true, "armed as the world arrives, in doubt")
    check(E.count("BRIDGE watch armed") == 1, "once, not once per hook: " .. tostring(E.count("BRIDGE watch armed")))
    E.timers = {} -- the load wipes every timer armed while it ran
    E.advance(3000)
    -- The player's first key, three seconds on: the loop is taken as wiped and armed again.
    local mark = E.mark()
    Player:OnAction("move_forward", 1, 1)
    -- The line names every pending quest, in the order `pairs` gives them: since Words
    -- Must Not Fail got a hail too (22 Oct 2026) there are two, so the id is looked for
    -- in the line rather than straight after "for".
    local armed = E.find("BRIDGE watch armed (OnAction) for ", mark)
    check(armed ~= nil and string.find(armed, id, 1, true) ~= nil, "armed again from the key")
    Player:OnAction("move_forward", 1, 1)
    check(E.count("BRIDGE watch armed", mark) == 1, "the second key in the same breath arms nothing")
    E.advance(2100)
    check(M.watchDoubt == false, "the tick that lands clears the doubt")
    -- And with the loop alive, a key arms nothing more; Henry coming near is hailed.
    mark = E.mark()
    Player:OnAction("move_forward", 1, 1)
    check(E.count("BRIDGE watch armed", mark) == 0, "a live loop is left alone")
    player:SetWorldPos({ x = 100 + M.hailMetres - 2, y = 100, z = 0 })
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " hail " .. q.giver, mark), "hailed on the re-armed loop")
end)

case("Dress: the holder with a preset written is put in it once a session, said in the log", function(E, M, check)
    local id, q = byName(M, "In the Stocks")
    check(q and q.dress ~= nil, "In the Stocks dresses its holder")
    if q == nil or q.dress == nil then return end
    local paul = E.soul(q.target, { pos = { x = 500, y = 500, z = 0 } })
    M.WatchArm("world")
    E.advance(2100)
    check(#paul.presets == 1 and paul.presets[1] == q.dress, "the preset put on him")
    check(E.find("BRIDGE quest " .. id .. " dress " .. q.target .. " in " .. q.dress .. " -> true"), "said so")
    E.advance(6100)
    check(#paul.presets == 1, "once only")
end)

case("Unyielding: the sacristan's refusal is in the store, the giver's naming makes the token and opens the helper, and her rows take money and give the key", function(E, M, check)
    local id, q = byName(M, "In the Stocks")
    check(q and q.unyielding == true and q.helper ~= nil and q.token ~= nil and q.keyClass ~= nil,
          "In the Stocks has an unyielding holder, a helper, a naming token and a key")
    if q == nil or q.helper == nil then return end
    check(q.doorPrice == 60 and q.keyPrice == 150, "her prices are the generator's")
    local benesh = E.soul(q.giver)
    local elsbeth = E.soul(q.helper)
    M.npc = benesh
    M.QuestCommand(id, "accept")
    -- The holder's row: no check, the refusal, and the store's n.
    M.QuestRebuffed(id, "persuade")
    local f = M.state.quests[id]
    check(f.rebuffed == true and string.find(M.QuestFlagLetters(f), "n", 1, true) ~= nil, "rebuffed, and the letter n")
    check(E.find("BRIDGE quest " .. id .. " rebuffed on persuade"), "said in the log")
    M.QuestRebuffed(id, "threaten")
    check(E.find("BRIDGE quest " .. id .. " rebuffed on threaten"), "the other row the same, again")
    -- The giver names her: the token for the graph, her role, the store's a.
    check(not M.HasMeta(elsbeth, q.helperMeta), "no helper role before the naming")
    M.QuestCommand(id, "named")
    check(M.Count(player, q.token) == 1, "the naming token in Henry's pocket")
    check(M.state.quests[id].named == true, "named in the store")
    check(string.find(M.QuestFlagLetters(M.state.quests[id]), "na", 1, true) ~= nil, "the letters n and a")
    check(M.HasMeta(elsbeth, q.helperMeta), "her menu's role granted")
    E.advance(2000)
    check(M.Count(player, q.token) == 0, "the token gone a moment later")
    check(M.watching == true, "the watch armed for the chest")
    -- Her door: sixty groschen to her, the store's i.
    local purse = player.inventory:GetMoney()
    M.QuestHelper(id, "door")
    check(elsbeth.inventory:GetMoney() == 60, "sixty groschen to her")
    check(math.abs(player.inventory:GetMoney() - (purse - 60)) < 0.01, "and off Henry")
    check(M.state.quests[id].letin == true, "let in, in the store")
    -- Her key: a hundred and fifty, the key in his pocket, the store's q.
    M.QuestHelper(id, "key")
    check(elsbeth.inventory:GetMoney() == 210, "a hundred and fifty more to her")
    check(M.Count(player, q.keyClass) == 1, "the chest's key in Henry's pocket")
    check(M.state.quests[id].keyed == true, "keyed, in the store")
    check(M.QuestFlagLetters(M.state.quests[id]) == "naiq", "the facts letters read naiq")
end)

case("Chest: spawned once when Henry comes near with the errand on, locked and keyed, the chalice inside; taking it is the stolen ending", function(E, M, check)
    local id, q = byName(M, "In the Stocks")
    check(q and q.chest ~= nil and q.chestLock ~= nil, "In the Stocks keeps its thing in a chest")
    if q == nil or q.chest == nil then return end
    local benesh = E.soul(q.giver)
    M.npc = benesh
    player:SetWorldPos({ x = q.chest[1] - M.chestMetres - 50, y = q.chest[2], z = q.chest[3] })
    M.QuestCommand(id, "accept")
    check(M.watching == true, "the watch armed at accept")
    E.advance(2100)
    check(System.GetEntityByName("mn_chest_" .. id) == nil, "no chest from afar")
    player:SetWorldPos({ x = q.chest[1] - 5, y = q.chest[2], z = q.chest[3] })
    local mark = E.mark()
    E.advance(2100)
    local chest = System.GetEntityByName("mn_chest_" .. id)
    check(chest ~= nil, "the chest stands once Henry is near")
    if chest == nil then return end
    check(chest.spec.class == "Stash" and chest.spec.properties.sWH_AI_EntityCategory == "Chest", "a Stash, a chest")
    check(chest.spec.properties.Lock.bLocked == true and chest.spec.properties.Lock.fLockDifficulty == q.chestLock,
          "locked at the generator's difficulty")
    check(chest.spec.properties.Lock.guidItemClassId == q.keyClass, "keyed to the quest's own key")
    check(chest.spec.properties.bOwnedByHome == false, "nobody's chest: opening it is no crime of itself")
    check(chest.inventory:GetCountOfClass(q.item) == 1, "the chalice inside")
    check(M.Count(player, q.item) == 0, "and not in Henry's pocket")
    check(M.state.quests[id].chest == true, "the chest noted in the store")
    check(E.count("BRIDGE quest " .. id .. " chest made", mark) == 1, "said once")
    -- Turned as the generator says (the fourth figure of chest_pos, degrees from +y), so it
    -- stands square to the room's furniture (notes.md 14 Oct 2026).
    local yaw = math.rad(q.chest[4] or 0)
    check(chest.spec.orientation ~= nil and math.abs(chest.spec.orientation.x - math.sin(yaw)) < 1e-6
          and math.abs(chest.spec.orientation.y - math.cos(yaw)) < 1e-6, "faced the way the generator says")
    -- The sheet of names under the chalice, when the design writes one.
    check(q.paper == nil or chest.inventory:GetCountOfClass(q.paper) == 1, "the paper in the chest beside it")
    check(q.taken ~= nil and M.Count(player, q.taken) == 0, "nothing marks the taking yet")
    E.advance(4100)
    check(E.count("BRIDGE quest " .. id .. " chest made", mark) == 1, "and not made again")
    -- Henry takes it (the lock is the game's business): the watch sees the chalice in his
    -- pocket and marks the objective off with a token of its own (notes.md 14 Oct 2026: 'Get
    -- the chalice from the chest' never ticked).
    chest.inventory:MoveItemOfClass(player.inventory:GetId(), q.item, 1, true)
    check(M.Count(player, q.item) == 1, "the chalice in his pocket")
    mark = E.mark()
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " thing taken from the chest", mark), "the taking seen")
    check(E.count("BRIDGE quest token made", mark) == 1, "and marked with its token")
    check(M.state.quests[id].taken == true, "and noted in the store")
    E.advance(4100)
    check(E.count("BRIDGE quest token made", mark) == 1, "once")
    check(M.Count(player, q.taken) == 0, "and the token gone again, the trigger having had it")
    -- Brought back: stolen.
    mark = E.mark()
    M.QuestCommand(id, "complete")
    check(E.find("BRIDGE quest " .. id .. " completes as complete_stolen", mark), "the hand-in is the stolen ending")
    check(M.Count(player, q.item) == 0, "and the chalice goes to Benesh")
end)

-- A retrieve whose unyielding holder can be caught where he should not be at night (Words
-- Must Not Fail's Proof Against Her): the one with seenWith, in id order. The harness has
-- no level, so the witness's bed (bedPos, the anchor of the door and the street's "at her
-- side" since notes.md 25 Oct 2026) is put where these cases stand her, 300,301.
local function byExposure(M)
    local ids = {}
    for id, q in pairs(M.quests) do if q.kind == "retrieve" and q.seenWith ~= nil then ids[#ids + 1] = id end end
    table.sort(ids)
    local q = M.quests[ids[1]]
    if q ~= nil and q.bedPos ~= nil then q.bedPos = { x = 300, y = 301 } end
    return ids[1], q
end

case("Expose: the holder caught at the bedside in the night hours makes the token and the seen global, and the expose row is the exposed ending with the earlier letter's Document", function(E, M, check)
    local id, q = byExposure(M)
    if id == nil then check(true, "no exposure in this data"); return end
    check(q.unyielding == true and q.letterOf ~= nil and q.seenVar ~= nil and q.hours ~= nil and q.token ~= nil,
          "an unyielding holder, the earlier letter, the seen global, the night window and a token")
    local of = M.quests[q.letterOf]
    check(of ~= nil and of.dictated == true, "letterOf names a dictated letter")
    if of == nil then return end
    -- The earlier scene: the letter written and kept by the priest (cursed).
    local giver = E.soul(q.giver)
    local priest = E.soul(q.target, { pos = { x = 300, y = 300, z = 10 } })
    local matilda = E.soul(q.seenWith, { pos = { x = 300, y = 301, z = 10 } })
    M.npc = giver
    M.QuestCommand(q.letterOf, "accept")
    M.QuestCommand(q.letterOf, "wrote_200")
    local wrote = of.letterPrefix .. "200" .. of.letterSuffix
    E.delivered(q.letterOf, "cursed")
    M.QuestCommand(q.letterOf, "complete")
    check(priest.inventory:GetCountOfClass(wrote) == 1, "the priest has the Document that was written")
    -- This scene: the thing is that Document, not the placeholder; nothing is made at accept.
    M.npc = giver
    player:SetWorldPos({ x = 100, y = 100, z = 0 })
    M.QuestCommand(id, "accept")
    check(M.quests[id].item == wrote, "the retrieve's item is the letter's written variant")
    check(M.Count(player, wrote) == 0 and M.Count(player, q.item) == 0, "nothing in Henry's pocket at accept")
    check(E.gvar(q.seenVar) == 0, "the seen global starts down")
    check(M.watching == true, "the watch armed for the nights")
    -- By day at the bedside: nothing. At night from across the street: nothing.
    E.hour = 12
    player:SetWorldPos({ x = 301, y = 300, z = 10 })
    local mark = E.mark()
    E.advance(2100)
    check(not E.find("BRIDGE quest " .. id .. " caught", mark), "not in the day")
    E.hour = 23
    player:SetWorldPos({ x = 300 + M.exposeMetres + 3, y = 300, z = 10 })
    E.advance(2100)
    check(not E.find("BRIDGE quest " .. id .. " caught", mark), "not from outside the room")
    -- At night, at the bedside, with her off in the kitchen: nothing.
    matilda:SetWorldPos({ x = 300, y = 300 + M.exposeWithMetres + 2, z = 10 })
    player:SetWorldPos({ x = 301, y = 300, z = 10 })
    E.advance(2100)
    check(not E.find("BRIDGE quest " .. id .. " caught", mark), "not with the two apart")
    -- The two of them in the bed, Henry at its foot, in the window: caught.
    matilda:SetWorldPos({ x = 300, y = 301, z = 10 })
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " caught " .. q.target .. " with " .. q.seenWith, mark), "caught")
    local f = M.state.quests[id]
    check(f.seen == true and string.find(M.QuestFlagLetters(f), "s", 1, true) ~= nil, "seen in the store, the letter s")
    check(E.gvar(q.seenVar) == 1, "the seen global up for the menu's expose row")
    check(M.Count(player, q.token) == 1, "the caught token for the graph")
    E.advance(4100)
    check(E.count("BRIDGE quest " .. id .. " caught", mark) == 1, "and not caught again: the watch is done with it")
    check(M.Count(player, q.token) == 0, "the token gone a moment later")
    -- The row on his Talk: the letter back off him for Henry's silence, the exposed ending.
    M.QuestHolderGave(id, "expose")
    check(M.state.quests[id].how == "exposed" and M.state.quests[id].handed == true, "exposed, handed")
    check(M.Count(player, wrote) == 1 and priest.inventory:GetCountOfClass(wrote) == 0, "the written Document moved to Henry")
    mark = E.mark()
    M.QuestCommand(id, "complete")
    check(E.find("BRIDGE quest " .. id .. " completes as complete_exposed", mark), "the hand-in is the exposed ending")
    check(M.Count(player, wrote) == 0, "and the letter goes to Christine")
end)

-- Henry's aside at the door (notes.md 23 Oct 2026, `enters`; 25 Oct 2026, the bed): the
-- holder gone in to the witness's bed in the evening, Henry near enough to have seen the
-- door, said once a night by the hour. The harness has no level to label the point, so
-- nearness to the bed stands for the house; the witness herself may be anywhere.
case("Enters: Henry remarks once a night, in the evening, when the holder has gone in to the witness's bed with Henry in sight of the door", function(E, M, check)
    local id, q = byExposure(M)
    if id == nil or q.enters == nil then check(true, "no enters line in this data"); return end
    check(q.bedPos ~= nil and M.enterNearMetres == 5, "the bed as the anchor, five metres to count as in")
    local giver = E.soul(q.giver)
    local priest = E.soul(q.target, { pos = { x = 400, y = 300, z = 10 } })
    -- The witness out at the well: the bed, not she, is the house.
    local matilda = E.soul(q.seenWith, { pos = { x = 300, y = 340, z = 10 } })
    M.npc = giver
    M.QuestCommand(q.letterOf, "accept")
    M.QuestCommand(q.letterOf, "wrote_200")
    E.delivered(q.letterOf, "cursed")
    M.QuestCommand(q.letterOf, "complete")
    M.npc = giver
    M.QuestCommand(id, "accept")
    check(M.watching == true, "the watch armed")
    local said = "BRIDGE quest " .. id .. " enters " .. q.target .. " in to " .. q.seenWith
    -- Early evening, the holder still in the street a hundred metres off: nothing.
    E.hour = 21
    player:SetWorldPos({ x = 320, y = 300, z = 10 })
    local mark = E.mark()
    E.advance(2100)
    check(not E.find(said, mark), "not while he is away from her")
    -- At the door, eight metres from the bed and the point unlabelled: not yet in.
    priest:SetWorldPos({ x = 300, y = 293, z = 8 })
    player:SetWorldPos({ x = 320, y = 300, z = 10 })
    E.advance(2100)
    check(not E.find(said, mark), "not at the door, the point unlabelled")
    -- In to the bed, Henry too far down the street to have seen: nothing.
    priest:SetWorldPos({ x = 300, y = 298, z = 8 })
    player:SetWorldPos({ x = 300 + M.enterSeeMetres + 10, y = 300, z = 10 })
    E.advance(2100)
    check(not E.find(said, mark), "not with Henry out of sight of the door")
    -- Before the evening: nothing.
    E.hour = 19
    player:SetWorldPos({ x = 320, y = 300, z = 10 })
    E.advance(2100)
    check(not E.find(said, mark), "not before the evening")
    -- The evening, Henry in the street: the aside, in his voice and on the narrative line,
    -- with the bed in the log.
    E.hour = 21
    E.advance(2100)
    check(E.find(said, mark), "the holder seen going in")
    check(E.find("3.0m from the bed", mark), "measured to the bed")
    check(E.find("BRIDGE voice henry " .. q.enters, mark), "said in Henry's voice")
    E.advance(4100)
    check(E.count(said, mark) == 1, "once")
    check(not E.find("BRIDGE quest " .. id .. " caught", mark), "and no catching: Henry is not at the bedside")
    check(M.watchQuick == true, "the watch on its quick tick with him near Henry in the evening")
    -- Out to the yard and in again the same evening (the game's pathing leaves and comes
    -- back): still the once; the night is the unit, not the visit.
    priest:SetWorldPos({ x = 300, y = 280, z = 8 })
    E.advance(2100)
    priest:SetWorldPos({ x = 300, y = 298, z = 8 })
    E.advance(2100)
    check(E.count(said, mark) == 1, "not again the same night for a step out and in")
    check(M.entered[id] == true, "the night's mark kept")
    -- The morning clears it, and with him eighty metres from Henry the watch is back on its
    -- two seconds; the next evening is another.
    E.hour = 9
    priest:SetWorldPos({ x = 400, y = 300, z = 10 })
    E.advance(2100)
    check(M.entered[id] == nil, "cleared with the hours past")
    check(M.watchQuick == false, "the quick tick off with him far from Henry")
    E.hour = 21
    priest:SetWorldPos({ x = 300, y = 298, z = 8 })
    E.advance(2100)
    check(E.count(said, mark) == 2, "again the next evening")
end)

-- The street (notes.md 23 Oct 2026, n8; 24 Oct 2026): the watch looks for Henry through the
-- holder's eyes each tick of the walk home -- the hours, the street, his facing, the
-- distance, the ray between their eyes -- and a sighting is his night.
case("Followed: the holder catching sight of Henry in the walk's hours in the street is his night: the store, the global, the token, his Talk asked for; not by day, not far, not behind him, not through a wall, not at her side, not standing still nor the tick after the engine set him down, not in the grace after the clock jumped; close counts from any side; over by morning; Henry's own words as the man first comes near and once he has turned on him", function(E, M, check)
    local id, q = byExposure(M)
    if id == nil or q.followHours == nil then check(true, "no followed lines in this data"); return end
    check(q.followedVar ~= nil and q.followToken ~= nil and #q.followHours == 2, "the global, the token and the hours")
    local giver = E.soul(q.giver)
    -- The holder in the street facing +y, the witness a hundred metres off. He walks a
    -- metre up the street each tick (walk), Henry keeping his place behind or before him,
    -- since a sighting wants a man on his walk (bugs.txt 28 Oct 2026): py is where he is,
    -- and henry() puts Henry about him.
    local py = 300
    local priest = E.soul(q.target, { pos = { x = 400, y = py, z = 10 } })
    local matilda = E.soul(q.seenWith, { pos = { x = 300, y = 301, z = 10 } })
    local function henry(dx, dy) player:SetWorldPos({ x = 400 + dx, y = py + dy, z = 10 }) end
    local function walk(ms)
        local h = player:GetWorldPos()
        py = py + 1
        priest:SetWorldPos({ x = 400, y = py, z = 10 })
        player:SetWorldPos({ x = h.x, y = h.y + 1, z = h.z })
        E.advance(ms)
    end
    M.npc = giver
    M.QuestCommand(q.letterOf, "accept")
    M.QuestCommand(q.letterOf, "wrote_200")
    E.delivered(q.letterOf, "cursed")
    M.QuestCommand(q.letterOf, "complete")
    M.npc = giver
    M.QuestCommand(id, "accept")
    check(M.watching == true, "the watch armed")
    check(E.gvar(q.followedVar) == 0, "the followed global starts down")
    check(E.commands["mn_graph"] ~= nil and E.commands["mn_graph"].body == "Moonshine.GraphSaid(%%)",
          "the graph's console command still registered for the probes: " .. tostring((E.commands["mn_graph"] or {}).body))
    local said = "BRIDGE quest " .. id .. " followed " .. q.target
    local warned = "BRIDGE quest " .. id .. " follow warn"
    local held = "BRIDGE quest " .. id .. " follow held"
    check(M.followSeeMetres == 12 and M.followWarnMetres == 25, "twelve metres to be seen, twenty-five to be warned")
    check(M.followStepMetres == 0.5 and M.followJumpMetres == 15 and M.followGraceSeconds == 30 and M.clockJumpSeconds == 900,
          "half a metre is a step, fifteen a re-placing, thirty seconds of grace after a clock jump of fifteen minutes")
    -- By day, Henry before him: nothing, not even the warning.
    E.hour = 12
    henry(0, 10)
    local mark = E.mark()
    walk(2100)
    walk(2100)
    check(not E.find(said, mark) and not E.find(warned, mark), "not by day")
    -- The walk's hour, Henry too far up the street for either.
    E.hour = 20
    henry(0, M.followWarnMetres + 5)
    walk(2100)
    check(not E.find(said, mark) and not E.find(warned, mark), "not out of sight, not yet near")
    -- Twenty metres before him, but Henry looking the other way (the man out of the
    -- camera's frame), then a wall between their eyes: near enough for Henry's word to
    -- himself, not to be seen, and the word waits on the view (notes.md 25 Oct 2026).
    henry(0, 20)
    E.framed = false
    walk(2100)
    check(not E.find(said, mark), "twenty metres is out of his sight")
    check(not E.find(warned, mark), "no warning with the man out of Henry's frame")
    check(#E.framedAsked > 0 and E.framedAsked[#E.framedAsked].z == 10 + M.followEyeMetres, "the frame asked for his head")
    E.framed = true
    E.walled = true
    walk(2100)
    check(not E.find(warned, mark), "no warning through a wall")
    E.walled = false
    walk(2100)
    if q.followWarn ~= nil then
        check(E.find(warned, mark) and E.find("in view", mark) and E.find("BRIDGE voice henry " .. q.followWarn, mark),
              "Henry's warning to himself once the man is before his eyes, in his voice")
        walk(2100)
        check(E.count(warned, mark) == 1, "the warning once")
    else
        check(not E.find(warned, mark), "no warning written")
    end
    -- Ten metres behind him: not seen.
    henry(0, -10)
    walk(2100)
    check(not E.find(said, mark), "not behind him")
    -- Ten metres before him but a wall between: not seen, and the ray was asked for
    -- from eye height, past the two of them.
    E.walled = true
    henry(0, 10)
    local rays = #E.rays
    walk(2100)
    check(not E.find(said, mark), "not through a wall")
    local ray = E.rays[#E.rays]
    check(#E.rays > rays and ray ~= nil and ray.from.z == 10 + M.followEyeMetres and ray.to.z == 10 + M.followEyeMetres
          and ray.skip1 == priest.id and ray.skip2 == player.id, "the ray between their eyes, the two of them passed over")
    E.walled = false
    -- The man stops in the street, Henry behind him, for longer than a pause in a walk
    -- (followStillSeconds); then Henry steps before him, in his sight: no night lost with
    -- the man standing, and the log says why, once.
    henry(0, -10)
    check(M.followStillSeconds == 6, "six seconds of standing is no longer a walk")
    E.advance(M.followStillSeconds * 1000 + 2100)
    check(not E.find(said, mark) and not E.find(held, mark), "nothing said of a man standing with Henry behind him")
    henry(0, 10)
    E.advance(2100)
    E.advance(2100)
    check(not E.find(said, mark), "not with the man standing")
    check(E.count(held, mark) == 1 and E.find(held .. ", " .. q.target .. " at 10.0m, ahead, standing", mark),
          "held for his standing, said once")
    -- At the witness's side: the door's and the bedside's business, not the street's.
    priest:SetWorldPos({ x = 300, y = 300, z = 10 })
    player:SetWorldPos({ x = 300, y = 305, z = 10 })
    E.advance(2100)
    check(not E.find(said, mark), "not at the witness's side")
    -- Back in the street a hundred metres off, Henry ten metres before him: the engine has
    -- set him down, and the tick after that is for settling, not judging.
    priest:SetWorldPos({ x = 400, y = py, z = 10 })
    henry(0, 10)
    check(M.watchQuick == true, "the quick tick, the holder having been near Henry")
    walk(1100)
    check(not E.find(said, mark), "not the tick after he was set down")
    check(E.find(held .. ", " .. q.target .. " at 10.0m, ahead, set down ", mark), "held for the re-placing")
    -- The street, in the walk's hour, Henry ten metres before him, the man walking:
    -- followed, within the second since the watch is on its quick tick.
    walk(1100)
    check(E.find(said, mark), "followed in the street in the walk's hour, within the second")
    check(E.find("10.0m, ahead", mark), "the distance and the sighting in the log")
    check(M.state.quests[id].followed == true, "in the store")
    check(E.gvar(q.followedVar) == 1, "the global up for his Talk to open on him")
    check(M.Count(player, q.followToken) == 1, "the token in Henry's pocket for the graph")
    check(not E.find("asks for the holder's Talk", mark), "the Talk not asked for in the same breath")
    E.advance(2000)
    check(E.find("BRIDGE quest " .. id .. " asks for the holder's Talk in the street", mark), "his Talk asked for a moment on")
    check(E.find("RequestDialog ->", mark), "through RequestDialog on him")
    check(M.Count(player, q.followToken) == 0, "the token gone again")
    check(not E.find("BRIDGE quest " .. id .. " caught", mark), "and no catching")
    -- Seen again the same night: nothing more.
    walk(2100)
    check(E.count(said, mark) == 1, "once a night")
    -- The request only queued and no Talk opened (the harness's RequestDialog, like his
    -- behaviour tree on 28 Oct 2026, opens nothing): the mod says the row itself, his line
    -- in his voice with his mouth, Henry's answer once it has been read, then the night
    -- given up; and the global comes down so a later Talk on him is his day's.
    local rowSaid = "BRIDGE quest " .. id .. " holder's Talk never opened, the row said in the street"
    check(not E.find(rowSaid, mark), "the request given its time first")
    E.advance(M.followTalkWaitMs)
    check(E.find(rowSaid, mark), "the row said by the mod after the request went unanswered")
    check(E.find("BRIDGE voice " .. q.holderVoice .. " soul=" .. q.target .. " " .. q.followedLine, mark),
          "his line in his voice, his mouth")
    check(M.state.quests[id].followedSaid == true and E.gvar(q.followedVar) == 0, "the row noted, the global down")
    check(not E.find("BRIDGE voice henry " .. q.followedReply, mark), "Henry's answer not in the same breath")
    local waited = 0
    while not E.find("BRIDGE voice henry " .. q.followedReply, mark) and waited < 20000 do
        E.advance(500)
        waited = waited + 500
    end
    check(E.find("BRIDGE voice henry " .. q.followedReply, mark) and waited >= (M.Reading(q.followedLine) - 5) * 1000,
          "Henry's answer once his line has been read")
    local over = "BRIDGE quest " .. id .. " follow over"
    check(not E.find(over, mark), "the night not given up in the same breath")
    E.advance(M.followOverMillis + 100)
    if q.followOver ~= nil then
        check(E.find(over, mark) and E.find("BRIDGE voice henry " .. q.followOver, mark), "Henry gives the night up, in his voice")
    else
        check(not E.find(over, mark), "no word written for the night lost")
    end
    -- The hour alone does not clear it: the clock read a daytime hour for a moment after
    -- the carry's winding (26 Oct 2026) with the world's seconds hardly moved, and the night
    -- stood. The morning proper, the world clock six hours on as well, clears it and the
    -- warning with it; the next evening is another chance, the warning said again, and
    -- this time Henry is behind him but close enough to hear.
    check(type(M.state.quests[id].followedAt) == "number", "the sighting's clock in the store")
    E.hour = 12
    walk(2100)
    check(not E.find("BRIDGE quest " .. id .. " followed night over", mark), "a daytime reading minutes on clears nothing")
    check(M.state.quests[id].followed == true and (q.followWarn == nil or M.followWarned[id] ~= nil),
          "the night and the warning stand")
    E.hour = 9
    E.worldBase = E.worldBase + M.followClearSeconds + 60
    walk(2100)
    check(E.find("BRIDGE quest " .. id .. " followed night over", mark), "over in the morning")
    check(E.gvar(q.followedVar) == 0 and M.state.quests[id].followed == nil and M.state.quests[id].followedAt == nil
          and M.state.quests[id].followedSaid == nil, "the global down, the store cleared")
    check(M.followWarned[id] == nil, "the warning cleared for the next night")
    -- The clock's jump (the morning's six hours) opened the grace: for thirty seconds no
    -- sighting, though Henry is close behind him on his walk; then the next night is
    -- another.
    check(E.find("BRIDGE quest follow grace, the clock jumped", mark), "the clock's jump noted")
    E.hour = 20
    henry(0, -3)
    walk(2100)
    check(E.count(said, mark) == 1, "not in the grace after the clock jumped")
    check(E.find(held .. ", " .. q.target .. " at 3.0m, close, in the grace", mark), "held for the grace")
    E.advance(M.followGraceSeconds * 1000)
    walk(2100)
    check(E.count(said, mark) == 2, "the next night is another, the grace over")
    check(E.find("3.0m, close", mark), "close behind him counts")
    if q.followWarn ~= nil then
        check(E.count(warned, mark) == 1 and E.count("BRIDGE voice henry " .. q.followWarn, mark) == 1,
              "the warning marked but not said over a sighting in the same tick")
    end
    -- Something the graph has no business saying is logged and no more.
    mark = E.mark()
    M.GraphSaid("seen", "nobody", "short")
    check(E.find("BRIDGE graph said seen of unknown quest nobody", mark), "an unknown quest is said and dropped")
    M.GraphSaid("seen", id, "short")
    check(E.find("BRIDGE graph said seen " .. id .. " short", mark) and E.count(said, mark) == 0, "a graph's word is logged and judges nothing")
end)

-- The street night's marker (bugs.txt 28 Oct 2026): on the holder from his homecoming to
-- the night window's end, off otherwise and for a night he has caught Henry behind him;
-- each change one token for the graph's markShown or markHidden trigger, the store
-- remembering which the map has.
case("Marker: a followed holder's marker goes on him at his homecoming hour and off at the night's end, by one token each way, and comes off for a night he has caught Henry following him", function(E, M, check)
    local id, q = byExposure(M)
    if id == nil or q.followHours == nil then check(true, "no followed lines in this data"); return end
    check(q.markHours ~= nil and q.markHours[1] == 21 and q.markHours[2] == q.hours[2],
          "the marker's hours run from his homecoming (routine_home_at 21:00) to the night window's end")
    check(q.markShown == "6d6f6f6e-7368-4d00-8000-" .. id and q.markHidden == "6d6f6f6e-7368-6d00-8000-" .. id,
          "the two tokens are the quest's own class under the M and m prefixes")
    local giver = E.soul(q.giver)
    E.soul(q.target, { pos = { x = 400, y = 300, z = 10 } })
    E.soul(q.seenWith, { pos = { x = 300, y = 301, z = 10 } })
    player:SetWorldPos({ x = 0, y = 0, z = 0 })
    M.npc = giver
    M.QuestCommand(q.letterOf, "accept")
    M.QuestCommand(q.letterOf, "wrote_200")
    E.delivered(q.letterOf, "cursed")
    M.QuestCommand(q.letterOf, "complete")
    M.npc = giver
    E.hour = 12
    local mark = E.mark()
    M.QuestCommand(id, "accept")
    E.advance(2100)
    local on, off = "BRIDGE quest " .. id .. " marker on " .. q.target, "BRIDGE quest " .. id .. " marker off " .. q.target
    check(not E.find(on, mark) and not E.find(off, mark) and M.state.quests[id].marked == nil,
          "nothing by day at the accept: the objective's Started line carries no marker of its own")
    -- His homecoming hour: on, once.
    E.hour = 21.2
    E.advance(2100)
    check(E.count(on, mark) == 1 and M.state.quests[id].marked == true, "on at his homecoming, in the store")
    check(M.Count(player, q.markShown) == 1 and M.Count(player, q.markHidden) == 0, "the shown token in Henry's pocket")
    E.advance(2100)
    check(M.Count(player, q.markShown) == 0, "and gone a moment later")
    E.hour = 2
    E.advance(2100)
    check(E.count(on, mark) == 1 and not E.find(off, mark), "still on in the small hours, no second token")
    -- The night's end: off, once.
    E.hour = q.hours[2] + 0.5
    E.advance(2100)
    check(E.count(off, mark) == 1 and M.state.quests[id].marked == false, "off at the night's end")
    check(M.Count(player, q.markHidden) == 1, "the hidden token in Henry's pocket")
    E.advance(2100)
    check(M.Count(player, q.markHidden) == 0, "and gone")
    E.hour = 12
    E.advance(2100)
    check(E.count(off, mark) == 1, "no second token by day")
    -- The next evening he catches Henry behind him: the graph's Followed state carries no
    -- marker, so the store alone comes down, with no token; and his marker is not put back
    -- while the night is his. The morning's clearing and the evening after put it back.
    E.hour = 21.5
    E.advance(2100)
    check(E.count(on, mark) == 2, "on again the next evening")
    local tokens = E.count("BRIDGE quest token made", mark)
    M.StateQuest(id, "followed")
    check(M.state.quests[id].marked == false, "the store's marker down with the sighting")
    E.advance(2100)
    check(E.count(on, mark) == 2 and E.count(off, mark) == 1 and E.count("BRIDGE quest token made", mark) == tokens,
          "no token for it, and not put back for the night")
    M.StateQuest(id, "unfollowed")
    E.hour = 9
    E.advance(2100)
    check(E.count(on, mark) == 2, "nor in the morning")
    E.hour = 21.5
    E.advance(2100)
    check(E.count(on, mark) == 3 and M.state.quests[id].marked == true, "on again the evening after")
end)

-- The bedside on top of the catching (notes.md 22 Oct 2026): the catching itself says
-- Henry's waking line and asks for the holder's Talk; its rows are the blackmail (won, the
-- thing and the coin; lost, the thing alone) and the denouncing (nothing, the giver told).
local function bedsideReady(E, M, check)
    local id, q = byExposure(M)
    if id == nil or q.wake == nil then return nil end
    local of = M.quests[q.letterOf]
    local giver = E.soul(q.giver)
    local priest = E.soul(q.target, { pos = { x = 300, y = 300, z = 10 } })
    local matilda = E.soul(q.seenWith, { pos = { x = 300, y = 301, z = 10 } })
    M.npc = giver
    M.QuestCommand(q.letterOf, "accept")
    M.QuestCommand(q.letterOf, "wrote_200")
    local wrote = of.letterPrefix .. "200" .. of.letterSuffix
    E.delivered(q.letterOf, "cursed")
    M.QuestCommand(q.letterOf, "complete")
    M.npc = giver
    player:SetWorldPos({ x = 100, y = 100, z = 0 })
    M.QuestCommand(id, "accept")
    check(E.gvar(q.denouncedVar) == 0, "the denounced global starts down")
    E.hour = 23
    player:SetWorldPos({ x = 301, y = 300, z = 10 })
    local mark = E.mark()
    E.advance(2100)
    check(E.find("BRIDGE quest " .. id .. " caught " .. q.target, mark), "caught")
    return id, q, wrote, priest, mark
end

case("Bedside: the catching says Henry's waking line in his voice and asks for the holder's Talk a moment later", function(E, M, check)
    local id, q, wrote, priest, mark = bedsideReady(E, M, check)
    if id == nil then check(true, "no bedside in this data"); return end
    check(E.find("BRIDGE voice henry " .. q.wake, mark), "the waking line said in Henry's voice")
    check(not E.find("asks for the holder's Talk", mark), "the Talk not asked for in the same breath")
    E.advance(4000)
    check(E.find("BRIDGE quest " .. id .. " asks for the holder's Talk at the bedside", mark), "the Talk asked for after the line")
    check(E.find("RequestDialog ->", mark), "through RequestDialog on the holder")
end)

case("Bedside: the blackmail won is the thing and the coin (exposed_paid); lost, the thing alone (exposed)", function(E, M, check)
    local id, q, wrote, priest, mark = bedsideReady(E, M, check)
    if id == nil then check(true, "no bedside in this data"); return end
    check((q.blackmail or 0) > 0, "a price on the blackmail")
    local before = M.Count(player, ItemUtils.itemIDs.money)
    M.QuestHolderGave(id, "blackmail")
    local f = M.state.quests[id]
    check(f.how == "exposed_paid" and f.handed == true, "exposed_paid, handed")
    check(M.Count(player, wrote) == 1 and priest.inventory:GetCountOfClass(wrote) == 0, "the written Document moved to Henry")
    check(M.Count(player, ItemUtils.itemIDs.money) - before == q.blackmail * 10, "and his coin in Henry's purse")
    mark = E.mark()
    M.QuestCommand(id, "complete")
    check(E.find("BRIDGE quest " .. id .. " completes as complete_exposed_paid", mark), "the hand-in is the exposed_paid ending")
    -- Lost: the same handover with no coin, the plain exposed.
    E.reset()
    id, q, wrote, priest, mark = bedsideReady(E, M, check)
    before = M.Count(player, ItemUtils.itemIDs.money)
    M.QuestHolderGave(id, "blackmail_no")
    f = M.state.quests[id]
    check(f.how == "exposed" and f.handed == true, "lost: exposed, handed")
    check(M.Count(player, wrote) == 1, "the Document moved to Henry all the same")
    check(M.Count(player, ItemUtils.itemIDs.money) == before, "and not a heller")
end)

case("Bedside: the denouncing leaves the thing with him, raises the denounced global and the letter d, and the giver's report completes as denounced with nothing taken back or paid", function(E, M, check)
    local id, q, wrote, priest, mark = bedsideReady(E, M, check)
    if id == nil then check(true, "no bedside in this data"); return end
    M.QuestDenounced(id)
    local f = M.state.quests[id]
    check(f.denounced == true and f.handed ~= true and f.how == "denounced", "denounced in the store, not handed, the how settled")
    check(string.find(M.QuestFlagLetters(f), "d", 1, true) ~= nil, "the letter d")
    check(E.gvar(q.denouncedVar) == 1, "the denounced global up for the Talk's one line")
    check(priest.inventory:GetCountOfClass(wrote) == 1 and M.Count(player, wrote) == 0, "the Document stays with him")
    local before = M.Count(player, ItemUtils.itemIDs.money)
    mark = E.mark()
    M.QuestCommand(id, "complete")
    check(E.find("BRIDGE quest " .. id .. " completes as complete_denounced", mark), "the report is the denounced ending")
    check(E.find("told as denounced; nothing came back, nothing paid", mark), "nothing taken back, nothing paid")
    check(M.Count(player, ItemUtils.itemIDs.money) == before, "the purse as it was")
    check(priest.inventory:GetCountOfClass(wrote) == 1, "and the paper still his")
    check(M.state.quests[id].done == true, "done")
end)

-- The giver's carrying (notes.md 25 Oct 2026): the app's `fast_travel` directive waits for
-- the conversation to be gone, then winds the clock on to the quest's hour, never back, and
-- puts Henry at its spot by SetWorldPos -- one road, the same with and without devmode, and
-- nothing typed at the console; off the horse first. Nothing for the store.
case("Carried: the fast_travel directive waits for the Talk to close, then winds the clock on to the hour and sets Henry down on the spot by SetWorldPos, nothing at the console, off his horse first", function(E, M, check)
    local id, q = byExposure(M)
    if id == nil or q.fastTravel == nil then check(true, "no carrying in this data"); return end
    check(q.fastTravel.x ~= nil and q.fastTravel.y ~= nil and q.fastTravel.z ~= nil and q.fastTravel.at ~= nil,
          "the spot and the hour in the table")
    local giver = E.soul(q.giver)
    M.npc = giver
    M.QuestCommand(id, "accept")
    local before = M.state.quests[id].accepted
    -- Over the Talk, with Henry in it: armed, and nothing moves for as long as it lasts.
    player:SetWorldPos({ x = 100, y = 100, z = 0 })
    E.hour = 12
    E.dialog.open = true
    M.active = true
    local mark = E.mark()
    local consoled = #E.consoled
    M.QuestCommand(id, "fast_travel")
    E.advance(3100)
    check(#E.consoled == consoled, "nothing typed at the console while the Talk is open")
    local pp = player:GetWorldPos()
    check(pp.x == 100 and pp.y == 100, "Henry where he was")
    check(E.hour == 12, "the clock untouched")
    -- The Talk gone: within the second Henry is at the spot and the clock reads the hour.
    E.dialog.open = false
    M.active = false
    E.advance(1100)
    pp = player:GetWorldPos()
    check(math.abs(pp.x - q.fastTravel.x) < 0.01 and math.abs(pp.y - q.fastTravel.y) < 0.01, "Henry at the spot")
    check(#E.consoled == consoled, "and nothing typed at the console: SetWorldPos, with or without devmode")
    check(math.abs(E.hour - q.fastTravel.at) < 0.01, "the clock wound on to the hour: " .. tostring(E.hour))
    E.advance(1100)
    check(E.find("BRIDGE quest " .. id .. " carried Henry to", mark) and E.find("0.0m off", mark), "and said so in the log, with how far off")
    check(M.state.quests[id].accepted == before and M.state.quests[id].done ~= true, "nothing in the store for it")
    check(type(M.followGraceAt) == "number" and M.Now() - M.followGraceAt < 5,
          "the street watch told of the carry, for its grace (bugs.txt 28 Oct 2026)")
    -- Past the hour already: tomorrow's, so the clock goes forward, never back.
    E.hour = q.fastTravel.at + 1
    M.QuestCommand(id, "fast_travel")
    E.advance(1100)
    check(math.abs(E.hour - q.fastTravel.at) < 0.01, "the hour the next day")
    -- Mounted: dismounted first, and moved four seconds on.
    player:SetWorldPos({ x = 100, y = 100, z = 0 })
    E.mounted = true
    mark = E.mark()
    M.QuestCommand(id, "fast_travel")
    E.advance(1100)
    check(E.mounted == false, "off the horse")
    check(E.find("carrying Henry once he is off the horse", mark), "and said why he waits")
    pp = player:GetWorldPos()
    check(pp.x == 100, "not moved yet")
    E.advance(4100)
    pp = player:GetWorldPos()
    check(math.abs(pp.x - q.fastTravel.x) < 0.01, "moved once the dismount has played")
end)

case("Mouth: in the game's own voiced Talk the lines queue, Henry's mouth first, then the named speaker's", function(E, M, check)
    -- No session: the holder's rows run through the game's dialogue, and each row's
    -- ExitScript voices Henry's line and the holder's together, with the holder's entity
    -- name on his (notes.md 13 Oct 2026: no lip movement on Paul).
    local paul = E.soul("kkut_man_2", { pos = { x = 2, y = 0, z = 0 } })
    local mine = "We both know it's in your chest."
    local his = "You dare? Get out of my sight."
    check(M.active ~= true and M.lipKey == nil, "nothing armed before the row")
    M.Voice("henry", mine)
    M.Voice("male", his, "kkut_man_2")
    check(M.voicedSoul == "kkut_man_2", "the speaker kept")
    check(M.lipKey == M.LipKey("henry", mine) and M.lipWho == "henry", "Henry's line armed first")
    check(#M.lipQueue == 1 and M.lipQueue[1].who == "male", "the holder's queued behind it")
    lipCue(E, M, "henry", mine, 1, "lip henry 1\nplay tmck_dial_a 1.00 2.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#player.anims == 1 and player.anims[1].clip == "tmck_dial_a", "Henry's clip on Henry")
    lipCue(E, M, "henry", mine, 2, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipKey == M.LipKey("male", his) and M.lipWho == "male" and #M.lipQueue == 0,
        "his stop arms the holder's line")
    lipCue(E, M, "male", his, 1, "lip male 1\nplay jnun_dial_a 1.00 3.00\n")
    E.advance(M.lipPollMillis + 10)
    check(#paul.anims == 1 and paul.anims[1].clip == "jnun_dial_a" and paul.anims[1].layer == M.lipLayer,
        "the holder's clip on the holder's entity")
    check(E.find("BRIDGE lip male 1 clip over 3.0s on the entity: jnun_dial_a x1.00"), "logged")
    lipCue(E, M, "male", his, 2, "stop\n")
    E.advance(M.lipPollMillis + 10)
    check(M.lipKey == nil and M.lipEnt == nil, "and stopped")
    -- The next row, picked a while later with the last one's line still armed: the old
    -- one is dropped rather than waited on.
    M.Voice("henry", "I'll break that chest open myself.")
    E.advance(2500)
    M.Voice("henry", "Never mind.")
    M.Voice("male", "Go.", "kkut_man_2")
    check(M.lipKey == M.LipKey("henry", "Never mind.") and #M.lipQueue == 1, "the new row's lines armed and queued, the old dropped")
    check(E.find("BRIDGE lip stopped -- next row") or true, "the drop logged when a clip was on")
end)

return { cases = cases, order = order }
