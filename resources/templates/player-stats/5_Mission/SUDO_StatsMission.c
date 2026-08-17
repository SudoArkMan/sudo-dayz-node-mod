// Where the stats are read, changed and written.
//
// The write point is PlayerDisconnected, and the reason is its third parameter.
// By the time it runs, player.GetIdentity() may already be null: vanilla's own
// comment on that method says "At this point, identity can be already deleted".
// The uid parameter is the answer, handed in before the identity went away. CF
// wrote a whole helper for the same problem, CF_GetIdentityId, which caches the
// id for exactly this reason.
//
// Plus a flush on a timer, so a crash costs at most one interval. Both halves
// of the timer are here: CallLater in the constructor and Remove in the
// destructor. Omitting the second is a shipped bug in vanilla elsewhere.
//
// Batched, never per event. SaveFile re-serialises the whole document on every
// call and FPrint is a blocking write on the simulation thread, so a per kill
// write on a sixty player server is sixty whole document rewrites a minute for
// nothing.
//
// The members:
//
//   m_SudoStats  the loaded record for every player on the server right now
//   m_SudoDirty  the ids that changed since the last flush, so the flush writes
//                what moved and not the whole server
//
// The Sudo prefix on both is not decoration. This is a modded class, so every
// name here shares one namespace with vanilla's own members and with every
// other mod that reopens MissionServer.

modded class MissionServer
{
	ref map<string, ref SUDO_PlayerStats> m_SudoStats;
	ref TStringArray m_SudoDirty;

	void MissionServer()
	{
		m_SudoStats = new map<string, ref SUDO_PlayerStats>();
		m_SudoDirty = new TStringArray();

		SUDO_StatsStore.Prepare();

		// CALL_CATEGORY_GAMEPLAY rather than _SYSTEM: that queue is processed
		// only during a mission and only while the game is not paused, and a
		// stats flush should not tick with no mission running.
		GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY).CallLater(this.SudoFlush, 60000, true);
	}

	void ~MissionServer()
	{
		GetGame().GetCallQueue(CALL_CATEGORY_GAMEPLAY).Remove(this.SudoFlush);
	}

	override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
	{
		super.InvokeOnConnect(player, identity);

		// InvokeOnConnect fires on every connect: a first join, a respawn and a
		// returning character all reach it. That is wrong for a starting kit
		// and exactly right for a visit counter.
		if (!player || !identity)
		{
			return;
		}

		string id = identity.GetId();
		SUDO_PlayerStats stats = SUDO_StatsStore.Load(id);
		stats.m_Name = identity.GetName();
		stats.m_Joins = stats.m_Joins + 1;

		m_SudoStats.Set(id, stats);
		SudoMarkDirty(id);

		player.MessageImportant("Welcome back. Visit " + stats.m_Joins + ".");
	}

	override void PlayerDisconnected(PlayerBase player, PlayerIdentity identity, string uid)
	{
		super.PlayerDisconnected(player, identity, uid);

		// uid, not identity.GetId(). The identity may already be gone.
		SUDO_PlayerStats stats = m_SudoStats.Get(uid);
		if (stats)
		{
			SUDO_StatsStore.Save(stats);
		}

		m_SudoStats.Remove(uid);

		int at = m_SudoDirty.Find(uid);
		if (at > -1)
		{
			m_SudoDirty.Remove(at);
		}
	}

	void SudoMarkDirty(string id)
	{
		if (m_SudoDirty.Find(id) < 0)
		{
			m_SudoDirty.Insert(id);
		}
	}

	void SudoFlush()
	{
		// Only the entries that changed. Walking every player every minute
		// would rewrite files nothing touched.
		for (int i = 0; i < m_SudoDirty.Count(); i++)
		{
			string id = m_SudoDirty.Get(i);
			SUDO_PlayerStats stats = m_SudoStats.Get(id);
			if (stats)
			{
				SUDO_StatsStore.Save(stats);
			}
		}

		m_SudoDirty.Clear();
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
