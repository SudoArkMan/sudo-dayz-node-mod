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
// The key is identity.GetId(). Not GetPlainId(), whose own doc line reads
// "plaintext unique id of player (cannot be used in database or logs)".

class SUDO_PlayerStats extends Managed
{
	// First member, and the reason the migration branch in Load below exists at
	// all. Forty eight vanilla files declare a static const int VERSION with a
	// hand written migration ladder after it; this is that ladder with one rung
	// in it, so the first person to add a field has somewhere to put the
	// migration instead of inventing the shape under pressure.
	int m_Version;
	string m_Id;
	string m_Name;
	int m_Joins;
	int m_Deaths;
	int m_SecondsPlayed;
}

class SUDO_StatsStore extends Managed
{
	static const int VERSION = 1;

	// $profile: resolves to the server's -profiles= folder, outside the PBO, so
	// it survives a mod update. DeleteFile and CopyFile work only on $profile:
	// and $saves:, so anything this may have to clean up has to live here
	// anyway. Forward slashes only: a backslash in an Enforce string breaks the
	// parser.
	static const string FOLDER = "$profile:SUDO_Stats/";

	static void Prepare()
	{
		// Writing into a folder that is not there fails quietly: OpenFile
		// returns 0 and SaveFile turns that into false plus a message nobody
		// reads unless they check it.
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
			data.m_Version = VERSION;
			data.m_Id = id;
			return data;
		}

		string error;
		if (!JsonFileLoader<SUDO_PlayerStats>.LoadFile(path, data, error))
		{
			// Do not write over it. A truncated file that gets overwritten with
			// an empty one loses the data twice, and the second time there is
			// nothing left to look at. The evidence is kept instead, which is
			// the difference between an incident and a mystery.
			Print("[SUDO_Stats] " + path + " will not load: " + error);
			CopyFile(path, path + ".bad");

			data = new SUDO_PlayerStats();
			data.m_Version = VERSION;
			data.m_Id = id;
			return data;
		}

		if (!data)
		{
			data = new SUDO_PlayerStats();
			data.m_Version = VERSION;
			data.m_Id = id;
			return data;
		}

		// The migration ladder. Add a rung per version, oldest first.
		if (data.m_Version < VERSION)
			data.m_Version = VERSION;

		return data;
	}

	static bool Save(SUDO_PlayerStats data)
	{
		if (!data)
			return false;

		string path = PathFor(data.m_Id);
		string temp = path + ".new";

		// SaveFile opens FileMode.WRITE, which truncates, then prints the whole
		// document and closes. A crash between the open and the close leaves a
		// truncated file. There is no rename or move proto in Enforce, so there
		// is no atomic replace to reach for: writing beside the real file and
		// copying over it only when the write returned true narrows the window
		// rather than closing it.
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
}
