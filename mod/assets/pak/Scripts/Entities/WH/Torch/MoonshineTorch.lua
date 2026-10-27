-- A torch that lights more ground. The game's Torch.lua loads only the flame particle;
-- the 8 m light on a torch in Henry's hand is the engine's own, chosen in code from
-- human:GetTorchLightArchetype(), and the item table has no say in it. So a brighter
-- torch is the shipped Torch with a second light of its own loaded on the item entity
-- when it is lit, the way Lamp.lua lights the night watch's lantern: the engine's light
-- stays underneath and this one reaches further. Shadowless, so there is one set of
-- shadows (the engine's), not two.
--
-- Registered by Scripts/Entities/Items/XML/MoonshineTorch.xml; an item row names it in
-- EntityScript (Libs/Tables/item/item__moonshine_names_light.xml). Everything not
-- defined here is Torch's (EntityCommon.DeriveOverride is a non-overwriting deep merge).
Script.ReloadScript("Scripts/Entities/WH/Torch/Torch.lua")

MoonshineTorch =
{
	Properties2 =
	{
		-- The archetype the extra light starts from, and what changes. The engine's own
		-- light on Henry's torch is Light_Torch_playerOnly!!!: 15 m, diffuse 0.02, GI x5, so
		-- an addition has to be well past that to show.
		sExtraLightArchetype = "Lights.Standard.Light_Torch_Wearable",
		fExtraRadius = 24,
		fExtraDiffuseScale = 4.0,
	},
}

-- =============================================================================
function MoonshineTorch:OnActivate()
	Torch.OnActivate(self)
	self:LoadExtraLight()
end

-- =============================================================================
function MoonshineTorch:LoadExtraLight()
	-- A probe may put numbers in MoonshineTorchTune = { radius=, scale= } before the
	-- torch is drawn; otherwise the properties above.
	local tune = MoonshineTorchTune or {}
	local radius = tune.radius or self.Properties2.fExtraRadius
	local scale = tune.scale or self.Properties2.fExtraDiffuseScale
	if (not self.nExtraLightSlot) then
		self.nExtraLightSlot = -1
	end
	local ok, err = pcall(function()
		local lp = System.GetArchetypeProperties(self.Properties2.sExtraLightArchetype)
		local lt = Light.GetLightDesc(lp, self.nExtraLightSlot)
		lt.radius = radius
		lt.diffuse_color = { x = lt.diffuse_color.x * scale, y = lt.diffuse_color.y * scale, z = lt.diffuse_color.z * scale }
		lt.cast_shadow = 0
		self.nExtraLightSlot = self:LoadLight(self.nExtraLightSlot, lt)
		local helper_pos = self:GetSlotHelperPos(0, self.Properties.sHelper, true)
		local helper_dir = self:GetHelperDir(self.Properties.sHelper, true)
		self:SetSlotPosAndDir(self.nExtraLightSlot, helper_pos, helper_dir)
	end)
	System.LogAlways(string.format("MoonshineTorch: extra light ok=%s slot=%s radius=%s scale=%s %s",
		tostring(ok), tostring(self.nExtraLightSlot), tostring(radius), tostring(scale), ok and "" or tostring(err)))
end

-- =============================================================================
function MoonshineTorch:FreeExtraLight()
	if (self.nExtraLightSlot and self.nExtraLightSlot >= 0) then
		self:FreeSlot(self.nExtraLightSlot)
	end
	self.nExtraLightSlot = nil
end

-- =============================================================================
function MoonshineTorch:Cleanup()
	self:FreeExtraLight()
	Torch.Cleanup(self)
end

-- =============================================================================
-- Dropped: the extra light goes out with the flame, and the burn-out glow is Torch's.
function MoonshineTorch:OnBurningOut()
	self:FreeExtraLight()
	Torch.OnBurningOut(self)
end

-- =============================================================================
-- Compose entity
-- =============================================================================
EntityCommon.DeriveOverride(MoonshineTorch, Torch);
