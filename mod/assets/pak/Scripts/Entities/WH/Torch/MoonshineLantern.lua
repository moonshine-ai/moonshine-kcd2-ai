-- A lantern Henry can carry. The game's Lamp.lua (lamp_tool, the night watch's lantern)
-- is an NPC tool; this is its light on an item in the torch slot. The light is a
-- projector, as Lights.Standard.Light_Lantern already is (175 degrees through the
-- lantern's pane texture): here narrowed to a beam through a round mask and turned each
-- frame to where Henry looks, which is what makes it a lantern and not a torch.
--
-- Registered by Scripts/Entities/Items/XML/MoonshineLantern.xml; the item row is in
-- Libs/Tables/item/item__moonshine_names_light.xml.
Script.ReloadScript("Scripts/Entities/Items/PickableItem.lua")

MoonshineLantern =
{
	-- cannot be named Properties because when we spawn items properties are replaced by
	-- the ones from PickableItems (Lamp.lua's note)
	Properties2 =
	{
		ParticleEffect = "WH_Particels.fires.candle",
		LightSource = "Lights.Standard.Light_Lantern",
		Helper = "slt_particle",
		-- Light_Lantern is 6 m at diffuse 0.003; the beam wants to show over the engine's
		-- own torch light (15 m at 0.02), which the torch slot gives this too.
		fRadius = 20,
		fBeamFov = 60,
		fDiffuseScale = 20.0,
		sBeamTexture = "textures/projectors/round_01_diff.tif",
		-- Which axis of the light slot the projector throws along. "y" is the slot's dir
		-- as SetSlotWorldTM takes it; "x" turns the slot so its X axis is the beam, for
		-- the case that the engine projects along X. The probe tries both.
		sBeamAxis = "y",
	},

	Properties =
	{
		sWH_AI_EntityCategory = "",
	},
}

-- =============================================================================
function MoonshineLantern:OnLoad(table)
end

-- =============================================================================
function MoonshineLantern:OnSave(table)
end

-- =============================================================================
function MoonshineLantern:OnPropertyChange()
	self.item:Reset()
	if (self.OnReset) then
		self:OnReset()
	end
end

-- =============================================================================
function MoonshineLantern:OnDestroy()
	self:Cleanup()
end

-- =============================================================================
function MoonshineLantern:Cleanup()
	if (self.nParticleSlot) then
		self:DeleteParticleEmitter(self.nParticleSlot)
		self:FreeSlot(self.nParticleSlot)
		self.nParticleSlot = nil
	end
	if (self.nLightSlot) then
		self:FreeSlot(self.nLightSlot)
		self.nLightSlot = nil
	end
	self.bLit = false
end

-- =============================================================================
function MoonshineLantern:OnReset()
	self:Cleanup()
	self:TurnLightOn()
end

-- =============================================================================
function MoonshineLantern:ModelReset()
	self:TurnLightOn()
end

-- =============================================================================
function MoonshineLantern:TurnLightOn()
	-- A probe may put numbers in MoonshineLanternTune = { radius=, fov=, scale=, texture=, axis= }
	-- before the lantern is drawn; otherwise the properties above.
	local tune = MoonshineLanternTune or {}
	local P = self.Properties2
	local radius, fov, scale = tune.radius or P.fRadius, tune.fov or P.fBeamFov, tune.scale or P.fDiffuseScale
	local texture = tune.texture or P.sBeamTexture
	if (not self.nParticleSlot) then
		self.nParticleSlot = -1
	end
	if (not self.nLightSlot) then
		self.nLightSlot = -1
	end
	local ok, err = pcall(function()
		self.nParticleSlot = self:LoadParticleEffect(self.nParticleSlot, P.ParticleEffect, P)
		local lp = System.GetArchetypeProperties(P.LightSource)
		local lt = Light.GetLightDesc(lp, self.nLightSlot)
		lt.radius = radius
		lt.proj_fov = fov
		lt.projector_texture = texture
		lt.diffuse_color = { x = lt.diffuse_color.x * scale, y = lt.diffuse_color.y * scale, z = lt.diffuse_color.z * scale }
		lt.cast_shadow = 0
		self.nLightSlot = self:LoadLight(self.nLightSlot, lt)
		local helper_pos = self:GetSlotHelperPos(0, P.Helper, true)
		local helper_dir = self:GetHelperDir(P.Helper, true)
		self:SetSlotPosAndDir(self.nParticleSlot, helper_pos, helper_dir)
		self:SetSlotPosAndDir(self.nLightSlot, helper_pos, helper_dir)
		self:SetFlags(ENTITY_FLAG_CASTSHADOW, 2)
	end)
	System.LogAlways(string.format("MoonshineLantern: light ok=%s slot=%s particle=%s radius=%s fov=%s scale=%s texture=%s %s",
		tostring(ok), tostring(self.nLightSlot), tostring(self.nParticleSlot), tostring(radius), tostring(fov),
		tostring(scale), tostring(texture), ok and "" or tostring(err)))
	self.bLit = ok
	-- The beam follows the look. Two clocks, either of which is enough: the entity
	-- update, if the engine runs OnUpdate on a script with no states, and a timer that
	-- re-arms itself at 30 Hz while the lantern is lit (Torch.lua's updates all run in
	-- its states; whether a stateless item gets them was the open question).
	self:SetUpdatePolicy(ENTITY_UPDATE_SCRIPT)
	self:Activate(1)
	self.nTicks, self.nUpdates = 0, 0
	self:Tick()
end

-- =============================================================================
function MoonshineLantern:Aim(who)
	if (not self.bLit or not self.nLightSlot or self.nLightSlot < 0) then
		return false
	end
	local tune = MoonshineLanternTune or {}
	local axis = tune.axis or self.Properties2.sBeamAxis
	local ok, err = pcall(function()
		-- Only while held: a put-away lantern is destroyed a frame or two after it leaves
		-- the hand, and a slot call on it in between is not worth the risk.
		local holder = self.item and self.item:GetEntityHoldingInHand()
		if (not holder) then
			self.bLit = false
			return
		end
		-- Henry's lantern looks where he looks; a man's where he faces.
		local dir
		if (holder == player) then
			dir = System.GetViewCameraDir()
		else
			dir = holder:GetDirectionVector()
		end
		local pos = self:GetSlotWorldPos(self.nParticleSlot)
		if (axis == "x") then
			-- CreateRotationVDir(v) has X = (v.y, -v.x, 0) normalised: for X along the
			-- (horizontal) beam, v is the beam turned a quarter left.
			dir = { x = -dir.y, y = dir.x, z = 0 }
		elseif (axis == "nx") then
			dir = { x = dir.y, y = -dir.x, z = 0 }
		elseif (axis == "ny") then
			dir = { x = -dir.x, y = -dir.y, z = -dir.z }
		elseif (axis == "down") then
			dir = { x = 0, y = 0, z = -1 }
		elseif (axis == "up") then
			dir = { x = 0, y = 0, z = 1 }
		end
		self:SetSlotWorldTM(self.nLightSlot, pos, dir)
		if (tune.report and self.sReported ~= axis) then
			self.sReported = axis
			local d = self:GetSlotWorldDir(self.nLightSlot)
			local c = System.GetViewCameraDir()
			System.LogAlways(string.format("MoonshineLantern: axis %s: asked %.2f %.2f %.2f, slot dir now %.2f %.2f %.2f, camera %.2f %.2f %.2f",
				axis, dir.x, dir.y, dir.z, d.x, d.y, d.z, c.x, c.y, c.z))
		end
	end)
	if (not ok and not self.bAimFailed) then
		self.bAimFailed = true
		System.LogAlways("MoonshineLantern: aim from " .. who .. " failed " .. tostring(err))
	end
	return ok
end

-- =============================================================================
function MoonshineLantern:Tick()
	if (not self.bLit) then
		return
	end
	self.nTicks = (self.nTicks or 0) + 1
	if (self.nTicks == 1 or self.nTicks == 150) then
		System.LogAlways(string.format("MoonshineLantern: tick %d, updates so far %d", self.nTicks, self.nUpdates or 0))
	end
	self:Aim("tick")
	local id = self.id
	Script.SetTimer(33, function()
		local e = System.GetEntity(id)
		if (e and e.Tick) then e:Tick() end
	end)
end

-- =============================================================================
function MoonshineLantern:OnUpdate(dt)
	self.nUpdates = (self.nUpdates or 0) + 1
	if (self.nUpdates == 1) then
		System.LogAlways("MoonshineLantern: OnUpdate runs")
	end
	self:Aim("update")
end

-- =============================================================================
MoonshineLantern.FlowEvents =
{
	Inputs =
	{
		ModelReset = { MoonshineLantern.ModelReset, "bool" },
	},
	Outputs =
	{
		ModelReset = "bool",
	},
}

-- =============================================================================
-- Compose entity
-- =============================================================================
EntityCommon.DeriveOverride(MoonshineLantern, PickableItem);
