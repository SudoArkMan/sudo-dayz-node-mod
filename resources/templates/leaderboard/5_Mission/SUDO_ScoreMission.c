// The board's boot, its timer, and the only way anybody sees it.
//
// Chat, not a menu. A UIScriptedMenu needs a globally unique integer with no
// registry anywhere to allocate it from, and a .layout has no inheritance and
// no partial override, so shipping one means copying a file and owning it
// forever. A leaderboard with a full UI is a mod. This is a template.
//
// No RPC either. DayZ gives a mod one OnRPC hook with a bare int that two mods
// can pick identically and collide on in silence, and the workarounds for that
// are somebody's whole networking layer, not a starting point.
//
// No database, no seasons, no reset schedule, no cross server merge. Each of
// those is a policy, and a template that guesses one is a template people fight
// rather than edit.

modded class MissionServer
{
	void MissionServer()
	{
		// Both halves of the timer, always. CALL_CATEGORY_GAMEPLAY is processed
		// only during a mission and only while the game is not paused, which is
		// what a leaderboard wants.
		GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY).CallLater(this.SudoRebuildTop, 300000, true);
	}

	void ~MissionServer()
	{
		GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY).Remove(this.SudoRebuildTop);
	}

	override void OnInit()
	{
		super.OnInit();

		if (!GetGame().IsServer())
			return;

		// Reads the file and holds the board for the rest of the session.
		SUDO_ScoreStore.Get();
	}

	override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
	{
		super.InvokeOnConnect(player, identity);

		if (!player)
			return;

		SUDO_ScoreStore store = SUDO_ScoreStore.Get();
		array<ref SUDO_PlayerScore> top = store.Top(10);

		if (top.Count() < 1)
		{
			player.MessageImportant("No kills on the board yet.");
			return;
		}

		player.MessageImportant("Top " + top.Count() + " by kills:");

		for (int i = 0; i < top.Count(); i++)
		{
			SUDO_PlayerScore row = top.Get(i);
			int place = i + 1;
			player.MessageImportant(place + ". " + row.m_Name + " " + row.m_Kills);
		}
	}

	// Every five minutes: write what changed, so a crash costs one interval
	// rather than the standings. The write is skipped entirely when nothing has
	// changed since the last one.
	void SudoRebuildTop()
	{
		SUDO_ScoreStore store = SUDO_ScoreStore.Get();
		store.Save();
	}
}
