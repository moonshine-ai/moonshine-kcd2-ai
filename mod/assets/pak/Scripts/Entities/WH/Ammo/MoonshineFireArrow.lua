-- The entity script of the fire arrows and bolts (moonshine_arrow_fire, moonshine_bolt_fire
-- in item__moonshine_names_fire.xml; design in data/kcd2/quest-design/flaming-arrows.md).
--
-- The arrow in flight is the engine's pooled Arrow projectile and never runs this script;
-- what does is the PickableItem the game drops where the shot ends, and the one that lands
-- when the stack is dropped from the inventory. So this is the fire on the ground: a small
-- standing flame (moonshine_fire.arrow.ground) loaded into a slot a beat after the entity
-- comes up, freed when it burns out. The lifecycle is Torch.lua's in miniature, without the
-- states: whichever of OnInit / OnSpawn / OnReset / OnLevelLoaded the engine calls first
-- lights it, the rest are no-ops, and a timer puts it out.
--
-- Proven by scripts/autopilot/fire_arrow_probe.lua (24 Oct 2026, six runs): the item system
-- honours EntityScript on an <Ammo> row, OnSpawn is the hook that fires for the dropped
-- arrow, and the flame shows on the grass where it lies. Everything the script does is
-- logged with the MoonshineFireArrow prefix (findstr MoonshineFireArrow kcd.log).

Script.ReloadScript("SCRIPTS/Entities/Items/PickableItem.lua")

MoonshineFireArrow =
{
	Server = {},
	Client = {},

	-- Properties2 as in Torch.lua: Properties is replaced by PickableItem's when the item spawns.
	Properties2 =
	{
		ParticleEffect = "moonshine_fire.arrow.ground",
		fBurnDurationSec = 25.0,
	},

	Properties =
	{
		sWH_AI_EntityCategory = "",
	},

	nFlameSlot = nil,
	bLit = false,
}

local TIMER_LIGHT = 70
local TIMER_BURN_OUT = 71
-- The flame's slot, named: LoadParticleEffect(-1) at OnSpawn took slot 0 (probe run 4), which
-- is where the item system then put the arrow's model, and the flame went with it.
local FLAME_SLOT = 7

-- =============================================================================
function MoonshineFireArrow:Light(why)
	if (self.bLit) then
		return
	end
	self.bLit = true
	-- a beat later, so the model is in its slot and the entity where the arrow stopped
	self.sLitVia = tostring(why)
	self:SetTimer(TIMER_LIGHT, 300)
end

-- =============================================================================
function MoonshineFireArrow:LightNow()
	local ok, err = pcall(function()
		self.nFlameSlot = self:LoadParticleEffect(FLAME_SLOT, self.Properties2.ParticleEffect, {})
		-- the head of the shaft: the model's origin is the nock end, the arrow is about 0.8 m
		self:SetSlotPos(self.nFlameSlot, { x = 0, y = 0.7, z = 0 })
		self:SetTimer(TIMER_BURN_OUT, self.Properties2.fBurnDurationSec * 1000)
	end)
	local p = self:GetWorldPos()
	local sp = nil
	pcall(function() sp = self:GetSlotWorldPos(self.nFlameSlot) end)
	System.LogAlways(string.format("MoonshineFireArrow lit via %s id=%s slot=%s ok=%s err=%s at %.1f %.1f %.1f flame at %s model slot 0 %s",
		tostring(self.sLitVia), tostring(self.id), tostring(self.nFlameSlot), tostring(ok), tostring(err), p.x, p.y, p.z,
		sp and string.format("%.1f %.1f %.1f", sp.x, sp.y, sp.z) or "nil", tostring(select(2, pcall(function() return self:IsSlotValid(0) end)))))
end

-- =============================================================================
function MoonshineFireArrow:PutOut(why)
	if (self.nFlameSlot) then
		pcall(function()
			self:DeleteParticleEmitter(self.nFlameSlot)
			self:FreeSlot(self.nFlameSlot)
		end)
		self.nFlameSlot = nil
	end
	self.bLit = false
	System.LogAlways(string.format("MoonshineFireArrow out via %s id=%s", tostring(why), tostring(self.id)))
end

-- =============================================================================
function MoonshineFireArrow:OnInit()
	self:Light("OnInit")
end

function MoonshineFireArrow:OnSpawn()
	self:Light("OnSpawn")
end

function MoonshineFireArrow:OnReset()
	self:Light("OnReset")
end

function MoonshineFireArrow:OnLevelLoaded()
	PickableItem.OnLevelLoaded(self)
	self:Light("OnLevelLoaded")
end

function MoonshineFireArrow:OnPropertyChange()
	PickableItem.OnPropertyChange(self)
	self:Light("OnPropertyChange")
end

-- Timer events go to the Server and Client tables (ProximityTrigger.lua, DestroyableObject.lua),
-- not the root: a root OnTimer never ran (probe run 5).
function MoonshineFireArrow:OnTimer(timerId, msec)
	if (timerId == TIMER_LIGHT) then
		self:LightNow()
	elseif (timerId == TIMER_BURN_OUT) then
		self:PutOut("timer")
	end
end

function MoonshineFireArrow.Server:OnTimer(timerId, msec)
	self:OnTimer(timerId, msec)
end

function MoonshineFireArrow:OnDestroy()
	self:PutOut("OnDestroy")
end

-- =============================================================================
EntityCommon.DeriveOverride(MoonshineFireArrow, PickableItem);
