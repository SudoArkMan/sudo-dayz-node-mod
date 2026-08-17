// The Community Framework module from the CF template, plus detection that
// compiles whether or not Expansion is installed.
//
// This class never names an Expansion type. It asks SUDO_ExpansionBridge, which
// is the only file in the mod that does, and branches on a bool. That is the
// part you will actually edit, and it stays nodes.
//
// config.cpp carries CF and deliberately nothing from Expansion:
//
//     requiredAddons[] = { "DZ_Scripts", "JM_CF_Scripts" };
//
// Listing a DayZExpansion_* addon would make Expansion a hard requirement,
// which is the opposite of the point. DayZ warns about a missing requiredAddons
// target rather than refusing to start, so the omission is safe and the #ifdef
// in the bridge does the work.

[CF_RegisterModule(SUDO_ExpansionModule)]

class SUDO_ExpansionModule extends CF_ModuleWorld
{
	// Answered once at boot and read everywhere else. Both halves are kept
	// because they can disagree: a mod built against Expansion and run without
	// it is a real deployment, and so is the reverse.
	bool m_BuiltWithExpansion;
	bool m_ExpansionLoaded;

	override void OnInit()
	{
		super.OnInit();

		m_BuiltWithExpansion = SUDO_ExpansionBridge.BuiltAgainstExpansion();
		m_ExpansionLoaded = SUDO_ExpansionBridge.ExpansionIsLoaded();

		EnableMissionStart();
		EnableInvokeConnect();
	}

	override bool IsClient()
	{
		return false;
	}

	override void OnMissionStart(Class sender, CF_EventArgs args)
	{
		super.OnMissionStart(sender, args);

		if (m_ExpansionLoaded)
			Print("[SUDO_Exp] Expansion is running on this server");
		else
			Print("[SUDO_Exp] Expansion is not running on this server");
	}

	override void OnInvokeConnect(Class sender, CF_EventArgs args)
	{
		super.OnInvokeConnect(sender, args);

		// CF deletes the args object after the dispatch, so read what you need
		// here and keep nothing.
		CF_EventPlayerArgs playerArgs = CF_EventPlayerArgs.Cast(args);
		if (!playerArgs)
			return;

		PlayerBase player = playerArgs.Player;
		if (!player)
			return;

		if (m_ExpansionLoaded)
			player.MessageImportant("Expansion features are on.");
		else
			player.MessageImportant("Running without Expansion.");
	}
}
