// One line in the server log saying which of the two answers came back.
//
// Optional, and worth keeping: detection that silently picks the wrong branch
// is the failure mode of this whole pattern, and a server owner cannot see a
// bool. Two lines in the log at boot is the cheapest way to make it visible.

modded class MissionServer
{
	override void OnInit()
	{
		super.OnInit();

		if (!GetGame().IsServer())
			return;

		Print("[SUDO_Exp] " + SUDO_ExpansionBridge.Describe());

		if (SUDO_ExpansionBridge.ExpansionIsLoaded())
		{
			Print("[SUDO_Exp] DZ_Expansion_Core_Preload answered, Expansion is loaded");
		}
		else
		{
			Print("[SUDO_Exp] DZ_Expansion_Core_Preload is absent, Expansion is not loaded");
		}
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
