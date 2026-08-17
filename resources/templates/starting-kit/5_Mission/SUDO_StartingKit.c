// A kit for a fresh character, and nothing at all for one that already exists.
//
// OnClientNewEvent is the hook the engine reaches only when it is creating a
// character: a first join, or a respawn after death. A character loaded out of
// the hive never reaches it. So there is no flag to persist and no duplicate to
// guard against, and "grant on spawn" means what it says.
//
// InvokeOnConnect and PlayerBase::OnConnect are the wrong hooks for this. Both
// fire on every connect, so granting there hands a returning player a second
// kit every login.
//
// StartingEquipSetup is the wrong hook too, for two reasons that are each
// enough on their own. It is reached only from EquipCharacter, which
// OnClientNewEvent skips entirely when the mission ships
// playerSpawnGearPresetFiles. And the stock mission's own CustomMission already
// overrides it without calling super, so a modded class override of the same
// method is shadowed with no error anywhere.
//
// For a one time per account gift instead, override
// InvokeOnConnect(PlayerBase, PlayerIdentity) and keep a set of
// identity.GetId() in a store. The Log player stats to JSON template is that
// store.

modded class MissionServer
{
	override PlayerBase OnClientNewEvent(PlayerIdentity identity, vector pos, ParamsReadContext ctx)
	{
		// Into a local first. super.OnClientNewEvent(...) returned straight out
		// of a return statement can come back null under the Enforce compiler.
		//
		// No server only guard in here. This method returns a PlayerBase, and
		// the guard's early return would hand null back to the connect path.
		// MissionServer is server side already.
		PlayerBase player = super.OnClientNewEvent(identity, pos, ctx);
		if (!player)
			return player;

		GiveStartingKit(player);
		return player;
	}

	void GiveStartingKit(PlayerBase player)
	{
		// Edit this list. These are config classnames, the same spelling
		// types.xml uses.
		TStringArray kit = new TStringArray();
		kit.Insert("TacticalBaconCan");
		kit.Insert("Rag");
		kit.Insert("Roadflare");

		for (int i = 0; i < kit.Count(); i++)
		{
			string type = kit.Get(i);

			// Into the inventory, or on the ground under them when nothing
			// fits. GameInventory.CreateInInventory returns null when there is
			// no free location, and PlayerBase.CreateInInventory returns NULL
			// too: its doc comment promises a ground fallback that its body
			// does not have. SpawnInInventoryOrGroundPos is the call that does
			// what that comment claims, and vanilla uses it eight times in
			// crossbow.c.
			//
			// Create at the destination rather than creating and then moving.
			// TakeToDst returns true and silently does nothing when it is
			// called more than once in a tick.
			EntityAI item = player.SpawnInInventoryOrGroundPos(type, player.GetInventory(), player.GetPosition());
			if (!item)
				Print("[SUDO_Kit] could not spawn " + type);
		}

		player.MessageImportant("Your starting kit is in your inventory.");
	}
}
