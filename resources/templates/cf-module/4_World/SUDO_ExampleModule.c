// A Community Framework module, wired the way CF's own CF_ModStorageModule is.
//
// The attribute registers a class name with CF_ModuleCoreManager. CF then
// spawns one instance per game, before any mission, and calls OnInit on it.
// Reach it from anywhere with CF_Modules<SUDO_ExampleModule>.Get().
//
// Every Enable call belongs in OnInit and nowhere else. Each one adds this
// module to a static event list, and CF says so itself on the one method where
// getting it wrong is fatal: RegisterNetSyncVariable, "@note Only call in
// 'OnInit'".
//
// CF_ModuleWorld is declared in JM/CF/Scripts/4_World, so a class extending it
// has to sit in 4_World or later. It is also the base that carries the nine
// connect and disconnect events, which is what anything about players wants.
//
// Server only is declared, not tested. CF turns IsServer() and IsClient() into
// a flag at construction and skips the module on the side it does not claim, so
// this costs nothing per event and there is no runtime branch in any handler.
//
// config.cpp has to carry CF or CF loads after you:
//
//     class CfgPatches
//     {
//         class MT_Scripts
//         {
//             requiredAddons[] = { "DZ_Scripts", "JM_CF_Scripts" };
//         };
//     };
//
// JM_CF_Scripts is CF's own CfgPatches name. The mod's CfgMods dependencies[]
// stays { "Game", "World", "Mission" }, because this module lives in 4_World.

[CF_RegisterModule(SUDO_ExampleModule)]

class SUDO_ExampleModule extends CF_ModuleWorld
{
	int m_Connects;

	override void OnInit()
	{
		super.OnInit();

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

		m_Connects = 0;
		Print("[SUDO_CF] module up");
	}

	override void OnInvokeConnect(Class sender, CF_EventArgs args)
	{
		super.OnInvokeConnect(sender, args);

		// Every world event hands the base args type, so the concrete one has
		// to be cast. OnInvokeConnect gets CF_EventPlayerArgs, OnClientNew gets
		// CF_EventNewPlayerArgs, OnClientDisconnect gets
		// CF_EventPlayerDisconnectedArgs.
		//
		// CF deletes the args object after the dispatch. Copy what you need out
		// of it here and never keep a reference to it.
		CF_EventPlayerArgs playerArgs = CF_EventPlayerArgs.Cast(args);
		if (!playerArgs)
			return;

		PlayerBase player = playerArgs.Player;
		if (!player)
			return;

		m_Connects = m_Connects + 1;
		player.MessageImportant("Connected. That makes " + m_Connects + " this session.");
	}
}
