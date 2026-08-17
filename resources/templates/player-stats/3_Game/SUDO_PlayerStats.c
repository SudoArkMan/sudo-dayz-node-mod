// One versioned JSON file per player, under the profiles folder, written
// through a temporary so a crash costs one interval rather than the history.
//
// JsonFileLoader<Class T>, not JsonApiStruct. JsonApiStruct is a hand driven
// writer plus twenty parse callbacks and exists for the backend path;
// JsonFileLoader serialises a whole class from its declared fields, so adding a
// field is one line instead of two parallel hand written walks.
//
// LoadFile and SaveFile, not JsonLoadFile and JsonSaveFile. The second pair
// sits under the "DEPRECATED" banner in jsonfileloader.c and returns void, so a
// failure is invisible.
//
// SUDO_PlayerStats extends Managed rather than declaring no base. A bare
// `class X` is its own root in Enforce, and JsonFileLoader<T> rejects a rootless
// class with "Bad type 'JsonFileLoader'".
//
// One file per player, keyed on identity.GetId(). Not GetPlainId(), whose own
// doc line reads "plaintext unique id of player (cannot be used in database or
// logs)". Per player rather than one big file, so a write does not rewrite
// everybody's history and one corrupt file does not take the rest with it.
//
// m_Version is the first member and the reason the migration ladder in Load
// exists. Forty eight vanilla files declare a static const int VERSION with a
// hand written ladder after it; this is that ladder with one rung in it, so the
// first person to add a field has somewhere to put the migration instead of
// inventing the shape under pressure.
//
// SUDO_StatsStore.FOLDER is "$profile:SUDO_Stats/". $profile: is one of three
// filesystem prefixes and resolves on a dedicated server to the -profiles=
// folder, outside the PBO, so it survives a mod update. DeleteFile and CopyFile
// work only on $profile: and $saves:, so anything this may have to clean up has
// to live there anyway. Forward slashes only: a backslash in an Enforce string
// breaks the parser.

class SUDO_PlayerStats extends Managed
{
	static const int VERSION = 1;
	int m_Version;
	string m_Id;
	string m_Name;
	int m_Joins;
	int m_Deaths;
	int m_SecondsPlayed;

	void SUDO_PlayerStats()
	{
		// Stamped on creation, so anything this build writes is already current
		// and the ladder in SUDO_StatsStore.Load only ever sees files older
		// than itself.
		m_Version = VERSION;
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};

class SUDO_StatsStore extends Managed
{
	static const string FOLDER = "$profile:SUDO_Stats/";

	static void Prepare()
	{
		// Once, at boot. Writing into a folder that is not there fails quietly:
		// OpenFile returns 0 and SaveFile turns that into false plus a message
		// nobody reads unless they check it.
		MakeDirectory(FOLDER);
	}

	static string PathFor(string id)
	{
		return FOLDER + id + ".json";
	}

	static SUDO_PlayerStats Load(string id)
	{
		string path = PathFor(id);
		SUDO_PlayerStats data;

		if (!FileExist(path))
		{
			data = new SUDO_PlayerStats();
			data.m_Id = id;
			return data;
		}

		string error;
		if (!JsonFileLoader<SUDO_PlayerStats>.LoadFile(path, data, error))
		{
			// Do not write over it. SaveFile opens FileMode.WRITE, which
			// truncates, so a crash mid write leaves a truncated file; a caller
			// that ignores this bool starts from an empty object and the next
			// save overwrites the truncated file with an empty one, losing the
			// data twice. The bad file is kept instead, which is the difference
			// between an incident and a mystery.
			Print("[SUDO_Stats] " + path + " will not load: " + error);
			CopyFile(path, path + ".bad");

			data = new SUDO_PlayerStats();
			data.m_Id = id;
			return data;
		}

		if (!data)
		{
			data = new SUDO_PlayerStats();
			data.m_Id = id;
			return data;
		}

		// The migration ladder. Add a rung per version, oldest first.
		if (data.m_Version < SUDO_PlayerStats.VERSION)
		{
			data.m_Version = SUDO_PlayerStats.VERSION;
		}

		return data;
	}

	static bool Save(SUDO_PlayerStats data)
	{
		// There is no rename and no move proto anywhere in P:\scripts, so there
		// is no atomic replace to reach for. Writing beside the real file and
		// copying over it only when the write returned true narrows the window
		// rather than closing it, and that is the honest description of what
		// this does.
		if (!data)
		{
			return false;
		}

		string path = PathFor(data.m_Id);
		string temp = path + ".new";

		string error;
		if (!JsonFileLoader<SUDO_PlayerStats>.SaveFile(temp, data, error))
		{
			Print("[SUDO_Stats] " + temp + " will not save: " + error);
			return false;
		}

		CopyFile(temp, path);
		DeleteFile(temp);
		return true;
	}

	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
};
