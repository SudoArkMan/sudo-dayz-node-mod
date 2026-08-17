// Everything that knows Expansion exists, kept in one file and behind guards.
//
// Read this first, because it explains a shape that otherwise looks wrong.
//
// Expansion's own canonical guard wraps the whole class:
//
//     #ifdef JM_COT
//     modded class JMPlayerModule { ... }
//     #endif
//
// That is correct Enforce, and it is the only thing that works when a method
// signature names a type that may be absent, because a signature naming a type
// that does not exist does not compile. It is also the one shape this editor
// destroys: a guard outside a class is read as text belonging to the file, the
// class is read as a class, and regenerating writes the text first and the
// class after it, leaving an empty #ifdef followed by an unguarded class. The
// mod then fails to build on every server without Expansion, which is the exact
// thing this template exists to prevent.
//
// So the guards live inside method bodies instead. A method holding a
// preprocessor line is kept as the text it was written as and written back
// unchanged, which is why this class is the one part of the template that is
// not nodes. That is deliberate, not a failure.
//
// The rule that keeps it working: no Expansion type appears in any signature or
// any member declaration in this file. Methods take and return vanilla types
// only. Everything Expansion owns stays inside a body, inside a guard.
//
// Add one method per Expansion feature you touch, shaped like the three below:
// a vanilla signature, the guard inside the body, and a real answer on the
// #else branch rather than a crash.

class SUDO_ExpansionBridge extends Managed
{
	static bool BuiltAgainstExpansion()
	{
		// EXPANSIONMODCORE is defined in
		// 0_DayZExpansion_Core_Preload/Common/DayZExpansion_Core_Defines.c,
		// directly under a comment saying it is published for third party mods
		// that want to know whether Expansion is loaded. Prefer it over the per
		// feature flags (EXPANSIONMODAI, EXPANSIONMODMARKET and the rest)
		// unless you touch that subsystem.
		//
		// It reaches your script because the preload registers its Common
		// folder into all five script modules, and because a define is visible
		// only to files compiled after it and the 0_ prefix sorts that PBO
		// ahead of anything starting with a letter. That ordering is filename
		// sort and nothing else: the preload declares requiredAddons[] = {}, so
		// no dependency edge enforces it. A mod whose own PBO sorted ahead of
		// 0_D would not see the flag.
		#ifdef EXPANSIONMODCORE
		return true;
		#else
		return false;
		#endif
	}

	static bool ExpansionIsLoaded()
	{
		// DZ_Expansion_Core_Preload is the CfgMods class name of that same
		// empty preload PBO, which makes it a capability beacon anything can
		// test for. Expansion uses precisely this shape on itself.
		return GetGame().ConfigIsExisting("CfgMods DZ_Expansion_Core_Preload");
	}

	static string Describe()
	{
		#ifdef EXPANSIONMODCORE
		return "built with Expansion";
		#else
		return "built without Expansion";
		#endif
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
