// Counting a kill correctly, which is the whole difficulty of a leaderboard.
//
// Three facts, and every one of them is a wrong number if you skip it.
//
// EEKilled runs on both sides. PlayerBase's own body branches on
// !GetGame().IsDedicatedServer(), so the override needs a server guard. It is
// safe here because the method returns void.
//
// killer can be the dead player. Since 1.20 EEKilled passes the dead entity as
// its own killer for deaths with no attacker, and vanilla's admin log treats
// player == source as "deaths not caused by another object (starvation,
// dehydration)". The signature is fixed, so the correction cannot be
// centralised and every consumer repeats it. First test in the method.
//
// The killer is usually the weapon, not the shooter. Vanilla resolves it by
// taking the hierarchy parent first and falling back to the object itself, and
// that is copied here exactly.
//
// No file IO in here. EEKilled fires from inside EEHitBy, on the damage path.
// Increment in memory and let the mission write on its own timer.

modded class PlayerBase
{
	override void EEKilled(Object killer)
	{
		super.EEKilled(killer);

		if (!GetGame().IsServer())
		{
			return;
		}

		PlayerIdentity victimIdentity = GetIdentity();
		if (victimIdentity)
		{
			SUDO_ScoreStore store = SUDO_ScoreStore.Get();
			store.AddDeath(victimIdentity.GetId(), victimIdentity.GetName());
		}

		// A death, not a kill.
		if (killer == this)
		{
			return;
		}

		if (!killer)
		{
			return;
		}

		// The weapon first, then the object itself. An AI kill or a killer who
		// has already disconnected gives null at one of the two steps, which is
		// why both are checked rather than chained.
		EntityAI source = EntityAI.Cast(killer);
		PlayerBase shooter;
		if (source)
		{
			shooter = PlayerBase.Cast(source.GetHierarchyParent());
		}

		if (!shooter)
		{
			shooter = PlayerBase.Cast(killer);
		}

		if (!shooter || shooter == this)
		{
			return;
		}

		PlayerIdentity killerIdentity = shooter.GetIdentity();
		if (!killerIdentity)
		{
			return;
		}

		SUDO_ScoreStore board = SUDO_ScoreStore.Get();
		board.AddKill(killerIdentity.GetId(), killerIdentity.GetName());

		shooter.MessageImportant("Kill counted.");
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
