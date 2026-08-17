// The scores, and the file they live in.
//
// Same safe write as the Log player stats to JSON template: write beside the
// real file, copy over it only when the write returned true, and never
// overwrite a file that failed to load. There is no rename proto in Enforce, so
// this narrows the window rather than closing it.
//
// One file for the whole board rather than one per player, because a
// leaderboard is read as a whole and rebuilt as a whole. A per player history
// is the other template.

class SUDO_PlayerScore extends Managed
{
	int m_Version;
	string m_Id;
	string m_Name;
	int m_Kills;
	int m_Deaths;
}

class SUDO_ScoreBoard extends Managed
{
	int m_Version;
	ref array<ref SUDO_PlayerScore> m_Rows;
}

class SUDO_ScoreStore extends Managed
{
	static const int VERSION = 1;
	static const string FOLDER = "$profile:SUDO_Scores/";

	static ref SUDO_ScoreStore s_Instance;

	ref map<string, ref SUDO_PlayerScore> m_Scores;
	bool m_Dirty;

	void SUDO_ScoreStore()
	{
		m_Scores = new map<string, ref SUDO_PlayerScore>();
	}

	static SUDO_ScoreStore Get()
	{
		if (!s_Instance)
		{
			MakeDirectory(FOLDER);
			s_Instance = new SUDO_ScoreStore();
			s_Instance.Load();
		}

		return s_Instance;
	}

	SUDO_PlayerScore Row(string id, string name)
	{
		SUDO_PlayerScore row = m_Scores.Get(id);
		if (!row)
		{
			row = new SUDO_PlayerScore();
			row.m_Version = VERSION;
			row.m_Id = id;
			m_Scores.Set(id, row);
		}

		if (name != "")
			row.m_Name = name;

		return row;
	}

	void AddKill(string id, string name)
	{
		SUDO_PlayerScore row = Row(id, name);
		row.m_Kills = row.m_Kills + 1;
		m_Dirty = true;
	}

	void AddDeath(string id, string name)
	{
		SUDO_PlayerScore row = Row(id, name);
		row.m_Deaths = row.m_Deaths + 1;
		m_Dirty = true;
	}

	// array<T>.Sort() orders primitives; its own doc says it "depends on
	// underlaying type", and it will not order a class by one of its fields. So
	// the top is selected explicitly: one pass per place, which is O(n * N) with
	// N of ten and is nothing.
	array<ref SUDO_PlayerScore> Top(int howMany)
	{
		array<ref SUDO_PlayerScore> taken = new array<ref SUDO_PlayerScore>();

		for (int place = 0; place < howMany; place++)
		{
			SUDO_PlayerScore best;

			for (int i = 0; i < m_Scores.Count(); i++)
			{
				SUDO_PlayerScore row = m_Scores.GetElement(i);
				if (row.m_Kills < 1)
					continue;
				if (taken.Find(row) > -1)
					continue;
				if (!best || row.m_Kills > best.m_Kills)
					best = row;
			}

			if (!best)
				break;

			taken.Insert(best);
		}

		return taken;
	}

	void Load()
	{
		string path = FOLDER + "scores.json";
		if (!FileExist(path))
			return;

		SUDO_ScoreBoard board;
		string error;
		if (!JsonFileLoader<SUDO_ScoreBoard>.LoadFile(path, board, error))
		{
			Print("[SUDO_Score] " + path + " will not load: " + error);
			CopyFile(path, path + ".bad");
			return;
		}

		if (!board || !board.m_Rows)
			return;

		for (int i = 0; i < board.m_Rows.Count(); i++)
		{
			SUDO_PlayerScore row = board.m_Rows.Get(i);
			if (row)
				m_Scores.Set(row.m_Id, row);
		}
	}

	void Save()
	{
		if (!m_Dirty)
			return;

		SUDO_ScoreBoard board = new SUDO_ScoreBoard();
		board.m_Version = VERSION;
		board.m_Rows = new array<ref SUDO_PlayerScore>();

		for (int i = 0; i < m_Scores.Count(); i++)
			board.m_Rows.Insert(m_Scores.GetElement(i));

		string path = FOLDER + "scores.json";
		string temp = path + ".new";

		string error;
		if (!JsonFileLoader<SUDO_ScoreBoard>.SaveFile(temp, board, error))
		{
			Print("[SUDO_Score] " + temp + " will not save: " + error);
			return;
		}

		CopyFile(temp, path);
		DeleteFile(temp);
		m_Dirty = false;
	}
}
