// The Community Framework module from the CF template, plus detection that
// compiles whether or not Expansion is installed.
//
// This class never names an Expansion type. It asks SUDO_ExpansionBridge, which
// is the only file in the mod that does, and branches on a bool. That is the
// part you will actually edit, and it stays nodes.
//
// The two members are both kept because they can disagree, and the disagreement
// is a real deployment either way round:
//
//   m_BuiltWithExpansion  this PBO was compiled with EXPANSIONMODCORE defined
//   m_ExpansionLoaded     Expansion is on this server right now
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
	bool m_BuiltWithExpansion;
	bool m_ExpansionLoaded;

	override void OnInit()
	{
		super.OnInit();

		// Answered once, here, and read everywhere else. Every Enable call
		// belongs in OnInit too: each one adds this module to a static event
		// list.
		m_BuiltWithExpansion = SUDO_ExpansionBridge.BuiltAgainstExpansion();
		m_ExpansionLoaded = SUDO_ExpansionBridge.ExpansionIsLoaded();

		EnableMissionStart();
		EnableInvokeConnect();
	}

	override bool IsClient()
	{
		// Server only, declared rather than tested. CF turns this into a flag
		// bit at construction and skips the module on the side it does not
		// claim, so there is no runtime branch in any handler below.
		return false;
	}

	override void OnMissionStart(Class sender, CF_EventArgs args)
	{
		super.OnMissionStart(sender, args);

		if (m_ExpansionLoaded)
		{
			Print("[SUDO_Exp] Expansion is running on this server");
		}
		else
		{
			Print("[SUDO_Exp] Expansion is not running on this server");
		}
	}

	override void OnInvokeConnect(Class sender, CF_EventArgs args)
	{
		super.OnInvokeConnect(sender, args);

		// CF deletes the args object after the dispatch, so read what you need
		// here and keep nothing.
		CF_EventPlayerArgs playerArgs = CF_EventPlayerArgs.Cast(args);
		if (!playerArgs)
		{
			return;
		}

		PlayerBase player = playerArgs.Player;
		if (!player)
		{
			return;
		}

		if (m_ExpansionLoaded)
		{
			player.MessageImportant("Expansion features are on.");
		}
		else
		{
			player.MessageImportant("Running without Expansion.");
		}
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
